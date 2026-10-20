"""迁移前后独立校验（与实现语言解耦）。

校验不涉及迁移写入逻辑：直接解析 v1/v2 源段与 v3 目标段字节，逐项比对。
任一不一致抛出 ValueError。返回统计字典。
"""
from __future__ import annotations

import mmap
import os
import struct
import zlib

from . import layout as L


def read_v2_segment(instance_id: str):
    """读取并解析一个当前版（v3）目标段。"""
    path = L.shm_path(instance_id)
    fd = os.open(path, os.O_RDONLY)
    try:
        hdr_raw = os.read(fd, L.HEADER_V2_SIZE)
        hdr = L.parse_v2_header(hdr_raw)
        if hdr["version"] != L.SHM_VERSION_CURRENT:
            raise ValueError(f"目标段非当前版 (version={hdr['version']})")
        max_points = hdr["max_points"]
        max_subscribers = hdr["max_subscribers"]
        total = L.total_size_v2(max_points, max_subscribers)
        data = bytearray(total)
        data[:len(hdr_raw)] = hdr_raw
        with mmap.mmap(fd, total, mmap.MAP_PRIVATE, mmap.PROT_READ) as mm:
            data[len(hdr_raw):] = mm[len(hdr_raw):total]
        off_points = hdr["off_points"]
        off_index = hdr["off_index"]
        off_meta = hdr["off_meta"]
        off_subs = hdr["off_subs"]
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


def validate(src_id: str, dst_id: str, source_ts_mode: str = "zero",
             sample: int = 0) -> dict:
    """独立校验 v1/v2 源段与 v3 目标段一致性。

    sample==0 表示全量比对；>0 表示顺序抽样数量（partition 断言始终全量）。
    返回统计字典；不一致抛 ValueError。
    """
    from . import migrate as M

    src = M.read_source_segment(src_id)
    dst = read_v2_segment(dst_id)

    # 1) Header / 容量一致
    if dst["max_points"] != src["max_points"]:
        raise ValueError(
            f"max_points 不一致: src={src['max_points']} dst={dst['max_points']}")
    if dst["max_subscribers"] != src["max_subscribers"]:
        raise ValueError("max_subscribers 不一致")
    if dst["version"] != L.SHM_VERSION_CURRENT:
        raise ValueError("目标段版本非当前版")

    # 2) CRC 重算
    with open(L.shm_path(dst_id), "rb") as f:
        hdr = f.read(L.HEADER_V2_SIZE)
    crc_calc = zlib.crc32(hdr[0:16] + hdr[48:72]) & 0xFFFFFFFF
    if crc_calc != dst["crc32"]:
        raise ValueError(
            f"v3 Header CRC 校验失败: stored={dst['crc32']:#x} "
            f"calc={crc_calc:#x}")

    N = src["max_points"]
    cap = L.layout_buckets(N)

    # 3) partition 断言（全量）：迁移后每个点都必须落在默认分区 0
    bad_partition = [
        pid for pid in range(N)
        if L.read_point_partition(dst["points"], pid) != 0
    ]
    if bad_partition:
        raise ValueError(
            f"迁移后存在非 0 partition 的点位: {bad_partition[:10]}"
            f"（共 {len(bad_partition)} 个）")

    # 4) 点位逐字节比对
    #    - source_timestamp_ns 区（112..120）按模式豁免；
    #    - partition 区（120..124）在两侧均屏蔽（迁移时有意置 0）。
    ids = list(range(N)) if sample == 0 else list(range(min(sample, N)))
    mismatches = []
    for pid in ids:
        a = bytearray(src["points"][pid * L.POINT_SIZE:
                                    (pid + 1) * L.POINT_SIZE])
        b = bytearray(dst["points"][pid * L.POINT_SIZE:
                                    (pid + 1) * L.POINT_SIZE])
        if source_ts_mode == "zero":
            a[L.SOURCE_TS_OFFSET:L.SOURCE_TS_OFFSET + 8] = b"\x00" * 8
            b[L.SOURCE_TS_OFFSET:L.SOURCE_TS_OFFSET + 8] = b"\x00" * 8
        a[L.PARTITION_OFFSET:L.PARTITION_OFFSET + 4] = b"\x00" * 4
        b[L.PARTITION_OFFSET:L.PARTITION_OFFSET + 4] = b"\x00" * 4
        if bytes(a) != bytes(b):
            mismatches.append(pid)

    # 5) 索引一致性：每个有名字的点都能按 (partition=0, name) 定位回自身
    idx_mismatch = []
    for pid in ids:
        name = L.read_point_name(src["points"], pid)
        if not name:
            continue
        got = L.index_probe(dst["index"], cap, dst["points"], name,
                            partition=0)
        if got != pid:
            idx_mismatch.append((pid, name, got))

    # 6) v2 源段：元数据区必须逐字节保留
    meta_preserved = None
    if src["version"] == L.SHM_VERSION_V2:
        if src.get("meta") != dst["meta"]:
            raise ValueError("v2 源段的元数据区未被原样保留")
        meta_preserved = True

    report = {
        "source_version": src["version"],
        "max_points": N,
        "checked": len(ids),
        "point_mismatches": mismatches,
        "index_mismatches": idx_mismatch,
        "crc_ok": True,
        "index_count": dst["index_count"],
        "index_capacity": cap,
        "partition_all_zero": True,
    }
    if meta_preserved is not None:
        report["meta_preserved"] = meta_preserved
    if mismatches or idx_mismatch:
        raise ValueError(
            f"校验失败: 点位不一致 {mismatches[:10]}, "
            f"索引不一致 {idx_mismatch[:10]}")
    return report
