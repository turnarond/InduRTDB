"""InduRTDB v1 -> v2 段迁移核心逻辑（纯 Python，不依赖 C 库）。

迁移范围（与方案设计 §6 一致）：
  - 点位值 / 类型 / 质量 / timestamp_ns（入库时刻）逐字节搬运；
  - 点位名（name）搬运，并在 v2 段内重建 name->id 索引；
  - source_timestamp_ns（采集时刻）默认置 0（--source-ts-mode keep 可保留）；
  - 元数据区（v2 新增）清零；订阅者心跳区清零（离线迁移，由新 rtdbd 重建）。
"""
from __future__ import annotations

import mmap
import os

from . import layout as L


def read_v1_segment(instance_id: str):
    """读取并解析一个 v1 段，返回命名元组式字典。"""
    path = L.shm_path(instance_id)
    fd = os.open(path, os.O_RDONLY)
    try:
        hdr_raw = os.read(fd, L.HEADER_V1_SIZE)
        version, max_points, max_subscribers = L.parse_v1_header(hdr_raw)
        total = L.total_size_v1(max_points, max_subscribers)
        # 读完整段
        data = bytearray(total)
        data[:len(hdr_raw)] = hdr_raw
        with mmap.mmap(fd, total, mmap.MAP_PRIVATE, mmap.PROT_READ) as mm:
            data[len(hdr_raw):] = mm[len(hdr_raw):total]
        points = bytes(data[L.HEADER_V1_SIZE:
                            L.HEADER_V1_SIZE + max_points * L.POINT_SIZE])
        subs_off = L.HEADER_V1_SIZE + max_points * L.POINT_SIZE
        subs = bytes(data[subs_off:subs_off
                          + max_subscribers * L.SUBS_ENTRY_SIZE])
        return {
            "version": version,
            "max_points": max_points,
            "max_subscribers": max_subscribers,
            "points": points,
            "subs": subs,
        }
    finally:
        os.close(fd)


def build_v2_segment(v1, source_ts_mode: str = "zero") -> bytes:
    """由 v1 数据构造完整 v2 段字节。"""
    N = v1["max_points"]
    M = v1["max_subscribers"]
    total = L.total_size_v2(N, M)

    raw = bytearray(total)
    # 1) Header（含 CRC）
    raw[0:L.HEADER_V2_SIZE] = L.build_v2_header(N, M)

    # 2) 点位区：逐字节搬运（同 128B 布局）
    op = L.off_points_v2()
    raw[op:op + N * L.POINT_SIZE] = v1["points"]
    if source_ts_mode == "zero":
        # source_timestamp_ns 置 0（采集时刻在离线迁移中无意义）
        for pid in range(N):
            base = op + pid * L.POINT_SIZE + L.SOURCE_TS_OFFSET
            raw[base:base + 8] = b"\x00" * 8

    # 3) 索引区：重建 name->id
    oi = L.off_index_v2(N)
    cap = L.layout_buckets(N)
    slots, index_count = L.build_index(v1["points"], N, cap)
    raw[oi:oi + len(slots)] = slots
    # 写回 index_count（header @72）
    import struct
    struct.pack_into("<I", raw, 72, index_count)

    # 4) 元数据区：清零（已为 0）；订阅者区：清零（离线迁移不搬运心跳）
    #    v1 的 subs 不搬运：迁移后由新 rtdbd 重新注册心跳。
    return bytes(raw)


def write_segment(instance_id: str, data: bytes, force: bool = False) -> bool:
    """将 v2 字节写入 /dev/shm/indurtdb_<id>。

    返回 True 表示目标段已存在并被覆盖（需 --force）。
    """
    path = L.shm_path(instance_id)
    existed = os.path.exists(path)
    if existed and not force:
        raise FileExistsError(
            f"目标段 {instance_id} 已存在；如需覆盖请加 --force")
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        os.ftruncate(fd, len(data))
        with mmap.mmap(fd, len(data), mmap.MAP_SHARED,
                       mmap.PROT_WRITE) as mm:
            mm[:] = data
            mm.flush()
    finally:
        os.close(fd)
    return existed


def migrate(src_id: str, dst_id: str, source_ts_mode: str = "zero",
            force: bool = False) -> dict:
    """执行一次 v1 -> v2 迁移。返回统计字典。"""
    v1 = read_v1_segment(src_id)
    v2 = build_v2_segment(v1, source_ts_mode=source_ts_mode)
    existed = write_segment(dst_id, v2, force=force)
    cap = L.layout_buckets(v1["max_points"])
    return {
        "src_id": src_id,
        "dst_id": dst_id,
        "max_points": v1["max_points"],
        "max_subscribers": v1["max_subscribers"],
        "index_capacity": cap,
        "dst_overwritten": existed,
        "source_ts_mode": source_ts_mode,
    }
