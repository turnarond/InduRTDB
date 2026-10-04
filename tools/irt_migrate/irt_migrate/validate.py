"""迁移前后独立校验（与实现语言解耦）。

校验不涉及迁移写入逻辑：直接解析 v1 源段与 v2 目标段字节，
逐项比对。任一不一致抛出 ValueError。返回统计字典。
"""
from __future__ import annotations

import mmap
import os
import struct

from . import layout as L


def read_v2_segment(instance_id: str):
    path = L.shm_path(instance_id)
    fd = os.open(path, os.O_RDONLY)
    try:
        hdr_raw = os.read(fd, L.HEADER_V2_SIZE)
        (magic, version, max_points, max_subscribers,
         write_seq, owner_pid, writes, timeouts,
         crc32, flags, reserved_scan,
         off_points, off_index, off_meta, off_subs,
         index_count, scan_skipped) = struct.unpack_from("<IIIIQiQQIIIIIIIII", hdr_raw, 0)
        if magic != L.MAGIC:
            raise ValueError(f"bad magic: {magic:#x}")
        if version != L.SHM_VERSION_V2:
            raise ValueError(f"目标段非 v2 (version={version})")
        total = L.total_size_v2(max_points, max_subscribers)
        data = bytearray(total)
        data[:len(hdr_raw)] = hdr_raw
        with mmap.mmap(fd, total, mmap.MAP_PRIVATE, mmap.PROT_READ) as mm:
            data[len(hdr_raw):] = mm[len(hdr_raw):total]
        points = bytes(data[off_points:off_points + max_points * L.POINT_SIZE])
        index = bytes(data[off_index:off_index
                           + L.layout_index_size(max_points)])
        return {
            "version": version,
            "max_points": max_points,
            "max_subscribers": max_subscribers,
            "crc32": crc32,
            "index_count": index_count,
            "off_points": off_points,
            "off_index": off_index,
            "off_meta": off_meta,
            "off_subs": off_subs,
            "points": points,
            "index": index,
            "total": total,
        }
    finally:
        os.close(fd)


def validate(src_id: str, dst_id: str, source_ts_mode: str = "zero",
             sample: int = 0) -> dict:
    """独立校验 v1 源段与 v2 目标段一致性。

    sample==0 表示全量比对；>0 表示随机/顺序抽样数量。
    返回统计字典；不一致抛 ValueError。
    """
    from . import migrate as M

    v1 = M.read_v1_segment(src_id)
    v2 = read_v2_segment(dst_id)

    # 1) Header / 容量一致
    if v2["max_points"] != v1["max_points"]:
        raise ValueError(
            f"max_points 不一致: v1={v1['max_points']} v2={v2['max_points']}")
    if v2["max_subscribers"] != v1["max_subscribers"]:
        raise ValueError("max_subscribers 不一致")
    if v2["version"] != L.SHM_VERSION_V2:
        raise ValueError("目标段版本非 v2")

    # 2) CRC 重算
    with open(L.shm_path(dst_id), "rb") as f:
        hdr = f.read(L.HEADER_V2_SIZE)
    crc_calc = __import__("zlib").crc32(hdr[0:16] + hdr[48:72]) & 0xFFFFFFFF
    if crc_calc != v2["crc32"]:
        raise ValueError(
            f"v2 Header CRC 校验失败: stored={v2['crc32']:#x} "
            f"calc={crc_calc:#x}")

    N = v1["max_points"]
    cap = L.layout_buckets(N)

    # 3) 点位逐字节比对（除 source_timestamp 区）
    ids = list(range(N)) if sample == 0 else list(range(min(sample, N)))
    mismatches = []
    for pid in ids:
        a = v1["points"][pid * L.POINT_SIZE:(pid + 1) * L.POINT_SIZE]
        b = v2["points"][pid * L.POINT_SIZE:(pid + 1) * L.POINT_SIZE]
        # source_timestamp_ns 区（偏移 112, 8B）按模式豁免
        cmp_a = bytearray(a)
        cmp_b = bytearray(b)
        if source_ts_mode == "zero":
            cmp_a[L.SOURCE_TS_OFFSET:L.SOURCE_TS_OFFSET + 8] = b"\x00" * 8
            cmp_b[L.SOURCE_TS_OFFSET:L.SOURCE_TS_OFFSET + 8] = b"\x00" * 8
        if bytes(cmp_a) != bytes(cmp_b):
            mismatches.append(pid)

    # 4) 索引一致性：每个有名字的点都能按名定位回自身
    idx_mismatch = []
    for pid in ids:
        name = L.read_point_name(v1["points"], pid)
        if not name:
            continue
        got = L.index_probe(v2["index"], cap, v2["points"], name)
        if got != pid:
            idx_mismatch.append((pid, name, got))

    report = {
        "max_points": N,
        "checked": len(ids),
        "point_mismatches": mismatches,
        "index_mismatches": idx_mismatch,
        "crc_ok": True,
        "index_count": v2["index_count"],
        "index_capacity": cap,
    }
    if mismatches or idx_mismatch:
        raise ValueError(
            f"校验失败: 点位不一致 {mismatches[:10]}, "
            f"索引不一致 {idx_mismatch[:10]}")
    return report
