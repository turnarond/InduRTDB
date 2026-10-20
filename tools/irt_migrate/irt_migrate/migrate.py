"""InduRTDB v1/v2 -> v3 段迁移核心逻辑（纯 Python，不依赖 C 库）。

迁移范围（与方案设计 §6 一致）：
  - 点位值 / 类型 / 质量 / timestamp_ns（入库时刻）逐字节搬运；
  - 点位名（name）搬运，并在目标 v3 段内重建 name->id 索引（键含分区）；
  - v3 新增 partition 字段：v1/v2 源段无此语义，目标段一律置 0（默认分区）；
  - source_timestamp_ns（采集时刻）默认置 0（--source-ts-mode keep 可保留）；
  - v2 源段的元数据区（每点 32B）**原样保留**；v1 无元数据区，目标清零；
  - 订阅者心跳区清零（离线迁移，由新 rtdbd 重建）。

v2 与 v3 布局逐字节同构，仅 Header version 字段值与 partition 语义不同。
"""
from __future__ import annotations

import mmap
import os
import struct

from . import layout as L

_SOURCE_TS_MODES = ("zero", "keep")


def read_v1_segment(instance_id: str):
    """读取并解析一个 v1 段，返回字典。"""
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


def peek_segment_version(instance_id: str) -> int:
    """只读段 Header 前 8 字节，返回 (已校验 magic 的) version 字段。"""
    path = L.shm_path(instance_id)
    fd = os.open(path, os.O_RDONLY)
    try:
        head = os.read(fd, 8)
    finally:
        os.close(fd)
    if len(head) < 8:
        raise ValueError(
            f"段 {instance_id} 头部过短（{len(head)}B），无法识别版本")
    magic, version = struct.unpack_from("<II", head, 0)
    if magic != L.MAGIC:
        raise ValueError(
            f"段 {instance_id} magic 非法: {magic:#x} "
            f"(expect {L.MAGIC:#x})")
    return version


def read_v2_source_segment(instance_id: str):
    """读取并解析一个 v2 源段（128B Header，version=2）。

    v2/v3 布局逐字节同构，但本函数**只接受 version==2**：v3 段已是目标格式，
    拒绝作为迁移源。返回字典包含 points / index / meta / subs 及 Header 偏移，
    其中 meta 必须被完整捕获以供目标段原样保留。
    """
    path = L.shm_path(instance_id)
    fd = os.open(path, os.O_RDONLY)
    try:
        hdr_raw = os.read(fd, L.HEADER_V2_SIZE)
        hdr = L.parse_v2_header(hdr_raw)
        if hdr["version"] != L.SHM_VERSION_V2:
            raise ValueError(
                f"源段版本={hdr['version']}，本工具只迁移 v1/v2 -> v3"
                f"（v2 读取器不接受该版本）")
        max_points = hdr["max_points"]
        max_subscribers = hdr["max_subscribers"]
        total = L.total_size_v2(max_points, max_subscribers)

        off_points = hdr["off_points"]
        off_index = hdr["off_index"]
        off_meta = hdr["off_meta"]
        off_subs = hdr["off_subs"]
        if 0 in (off_points, off_index, off_meta, off_subs):
            raise ValueError(
                f"v2 源段 {instance_id} 的区段偏移未 seal（存在 0 值）；"
                f"该段可能未初始化完成，拒绝迁移")

        size = os.fstat(fd).st_size
        if size < total:
            raise ValueError(
                f"v2 源段 {instance_id} 实际大小 {size}B 小于布局所需 "
                f"{total}B；段已损坏，拒绝迁移")

        data = bytearray(total)
        data[:len(hdr_raw)] = hdr_raw
        with mmap.mmap(fd, total, mmap.MAP_PRIVATE, mmap.PROT_READ) as mm:
            data[len(hdr_raw):] = mm[len(hdr_raw):total]

        points = bytes(data[off_points:off_points
                            + max_points * L.POINT_SIZE])
        index = bytes(data[off_index:off_index
                           + L.layout_index_size(max_points)])
        meta = bytes(data[off_meta:off_meta
                          + L.layout_meta_size(max_points)])
        subs = bytes(data[off_subs:off_subs
                          + max_subscribers * L.SUBS_ENTRY_SIZE])
        return {
            "version": hdr["version"],
            "max_points": max_points,
            "max_subscribers": max_subscribers,
            "crc32": hdr["crc32"],
            "index_count": hdr["index_count"],
            "off_points": off_points,
            "off_index": off_index,
            "off_meta": off_meta,
            "off_subs": off_subs,
            "points": points,
            "index": index,
            "meta": meta,
            "subs": subs,
            "total": total,
        }
    finally:
        os.close(fd)


def read_source_segment(instance_id: str):
    """按段头 version 分派读取 v1 或 v2 源段。

    v3（已是目标格式）与任何未知版本一律拒绝。
    """
    version = peek_segment_version(instance_id)
    if version == L.SHM_VERSION_V1:
        return read_v1_segment(instance_id)
    if version == L.SHM_VERSION_V2:
        return read_v2_source_segment(instance_id)
    raise ValueError(
        f"source segment version={version} is not supported; "
        f"本工具只迁移 v1/v2 -> v3")


def build_target_segment(src: dict, source_ts_mode: str = "zero") -> bytes:
    """由 v1 或 v2 源数据构造完整 v3 段字节。

    关键语义：
      - partition（点位偏移 120，4B）无论源版本一律显式置 0；
      - v2 源段的 meta 区逐字节原样保留；v1 无 meta，保持全 0；
      - 索引从**最终目标点位字节**重建（而非源字节），键为 compose_hash(0, name)。
    """
    if source_ts_mode not in _SOURCE_TS_MODES:
        raise ValueError(
            f"未知 source_ts_mode={source_ts_mode!r}；"
            f"可选 {_SOURCE_TS_MODES}")

    N = src["max_points"]
    M = src["max_subscribers"]
    total = L.total_size_v2(N, M)

    raw = bytearray(total)
    # 1) Header（含 CRC；version 为 SHM_VERSION_CURRENT=3）
    raw[0:L.HEADER_V2_SIZE] = L.build_v2_header(N, M)

    # 2) 点位区：逐字节搬运（同 128B 布局），再统一改写 v3 语义字段
    op = L.off_points_v2()
    points = bytearray(src["points"])
    if len(points) != N * L.POINT_SIZE:
        raise ValueError(
            f"源 points 长度 {len(points)}B 与 max_points={N} 不符")
    for pid in range(N):
        base = pid * L.POINT_SIZE
        # v3 新增 partition：v1/v2 源段无此语义 -> 一律默认分区 0
        points[base + L.PARTITION_OFFSET:
               base + L.PARTITION_OFFSET + 4] = b"\x00" * 4
        if source_ts_mode == "zero":
            # source_timestamp_ns 置 0（采集时刻在离线迁移中无意义）
            points[base + L.SOURCE_TS_OFFSET:
                   base + L.SOURCE_TS_OFFSET + 8] = b"\x00" * 8
    target_points = bytes(points)
    raw[op:op + N * L.POINT_SIZE] = target_points

    # 3) 索引区：从最终目标点位字节重建 name->id（键含 partition，此处恒 0）
    oi = L.off_index_v2(N)
    cap = L.layout_buckets(N)
    slots, index_count = L.build_index(target_points, N, cap)
    raw[oi:oi + len(slots)] = slots
    # 写回 index_count（header @72，易变字段不入 CRC）
    struct.pack_into("<I", raw, 72, index_count)

    # 4) 元数据区：v2 源段原样保留；v1 无此区，保持全 0。
    src_meta = src.get("meta")
    if src_meta:
        om = L.off_meta_v2(N)
        meta_size = L.layout_meta_size(N)
        if len(src_meta) != meta_size:
            raise ValueError(
                f"源 meta 长度 {len(src_meta)}B 与布局所需 {meta_size}B 不符")
        raw[om:om + meta_size] = src_meta

    # 5) 订阅者区：保持全 0（离线迁移不搬运心跳，由新 rtdbd 重建）。
    return bytes(raw)


def build_v2_segment(src, source_ts_mode: str = "zero") -> bytes:
    """历史名称兼容包装：实际构造当前版（v3）段字节。"""
    return build_target_segment(src, source_ts_mode=source_ts_mode)


def write_segment(instance_id: str, data: bytes, force: bool = False) -> bool:
    """将段字节写入 /dev/shm/indurtdb_<id>。

    返回 True 表示目标段已存在并被覆盖（需 --force）。
    实现为 O_CREAT|O_RDWR + ftruncate + mmap 覆写（**不** unlink 旧段），
    因此同 id 原地迁移是安全的：源段内容在读取阶段已完整载入内存。
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
    """执行一次 v1/v2 -> v3 迁移。返回统计字典。"""
    src = read_source_segment(src_id)
    target = build_target_segment(src, source_ts_mode=source_ts_mode)
    existed = write_segment(dst_id, target, force=force)
    cap = L.layout_buckets(src["max_points"])
    return {
        "src_id": src_id,
        "dst_id": dst_id,
        "source_version": src["version"],
        "max_points": src["max_points"],
        "max_subscribers": src["max_subscribers"],
        "index_capacity": cap,
        "dst_overwritten": existed,
        "source_ts_mode": source_ts_mode,
    }
