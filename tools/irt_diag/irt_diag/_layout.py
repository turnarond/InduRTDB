"""v2 共享内存 Header 布局常量与解析（复刻 src/internal/irt_types.h）。

仅用于诊断读取，绝不写入段。CRC 覆盖区间与 src/core/irt_shm.c 的
irt_header_crc32_of 逐字节一致：[0:16]（magic/version/max_points/max_subscribers）
与 [48:72]（flags/保留/off_points/off_index/off_meta/off_subs）。scan_skipped
在保留区 [76:80]，不计入 CRC。
"""
from __future__ import annotations

import struct
import zlib

MAGIC = 0x1DBA1DBA
SHM_VERSION_V2 = 2
HEADER_V2_SIZE = 128
POINT_SIZE = 128


def parse_v2_header(raw: bytes) -> dict:
    """解析 128B v2 Header，返回健康所需的全部字段 + crc 校验结果。"""
    if len(raw) < HEADER_V2_SIZE:
        raise ValueError(f"header too short: {len(raw)} < {HEADER_V2_SIZE}")
    magic, version, max_points, max_subscribers = struct.unpack_from("<IIII", raw, 0)
    owner_pid = struct.unpack_from("<i", raw, 24)[0]
    crc_stored = struct.unpack_from("<I", raw, 44)[0]
    (flags, reserved_scan, off_points, off_index, off_meta,
     off_subs, index_count, scan_skipped) = struct.unpack_from("<IIIIIIII", raw, 48)
    crc_calc = zlib.crc32(raw[0:16] + raw[48:72]) & 0xFFFFFFFF
    return {
        "magic_ok": magic == MAGIC,
        "version": version,
        "max_points": max_points,
        "max_subscribers": max_subscribers,
        "owner_pid": owner_pid,
        "crc_ok": crc_stored == crc_calc,
        "crc_stored": crc_stored,
        "crc_calc": crc_calc,
        "flags": flags,
        "reserved_scan": reserved_scan,
        "off_points": off_points,
        "off_index": off_index,
        "off_meta": off_meta,
        "off_subs": off_subs,
        "index_count": index_count,
        "scan_skipped": scan_skipped,
    }
