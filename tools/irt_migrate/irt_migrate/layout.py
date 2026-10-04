"""InduRTDB 共享内存段布局常量与底层字节工具。

本模块不依赖 C 库，纯 Python 复刻 v1/v2 共享内存布局，供迁移工具
（tools/irt_migrate）离线读写段。布局常量与 src/internal/irt_types.h、
src/core/irt_shm.h 严格一致 —— 任何改动须同步。
"""
from __future__ import annotations

import struct
import zlib

# ---- 通用常量 ----
MAGIC = 0x1DBA1DBA
SHM_VERSION_V1 = 1          # v3.3 及以前
SHM_VERSION_V2 = 2          # v3.4 布局 v2

HEADER_V1_SIZE = 64         # == v2.x InduRTDBHeader（旧）
HEADER_V2_SIZE = 128        # 布局 v2
POINT_SIZE = 128            # indurtdb_point_t（v1/v2 同尺寸、同偏移）
SUBS_ENTRY_SIZE = 16        # irt_subscriber_entry_t
INDEX_SLOT_SIZE = 8         # irt_index_slot_t {hash32, point_id32}
META_SIZE_PER_POINT = 32    # indurtdb_meta_t

# 点位内字段偏移（与 indurtdb_point_t 完全一致）
NAME_OFFSET = 45
NAME_LEN = 64
SOURCE_TS_OFFSET = 112      # source_timestamp_ns（8B）

# 索引槽特殊值（与 irt_index.h 一致）
INDEX_EMPTY = 0xFFFFFFFF
INDEX_TOMBSTONE = 0xFFFFFFFE

SHM_PREFIX = "/indurtdb_"


def shm_path(instance_id: str) -> str:
    return f"/dev/shm{SHM_PREFIX}{instance_id}"


# ---- 布局尺寸计算（复刻 irt_shm.h）----
def layout_buckets(max_points: int) -> int:
    """roundup_pow2(max(8, max_points*2)) —— 负载因子 <= 0.5。"""
    cap = 8
    while cap < max_points * 2:
        cap <<= 1
    return cap


def layout_index_size(max_points: int) -> int:
    return layout_buckets(max_points) * INDEX_SLOT_SIZE


def layout_meta_size(max_points: int) -> int:
    return max_points * META_SIZE_PER_POINT


def off_points_v2() -> int:
    return HEADER_V2_SIZE


def off_index_v2(max_points: int) -> int:
    return off_points_v2() + max_points * POINT_SIZE


def off_meta_v2(max_points: int) -> int:
    return off_index_v2(max_points) + layout_index_size(max_points)


def off_subs_v2(max_points: int) -> int:
    return off_meta_v2(max_points) + layout_meta_size(max_points)


def total_size_v2(max_points: int, max_subscribers: int) -> int:
    return (off_subs_v2(max_points)
            + max_subscribers * SUBS_ENTRY_SIZE)


def total_size_v1(max_points: int, max_subscribers: int) -> int:
    return (HEADER_V1_SIZE
            + max_points * POINT_SIZE
            + max_subscribers * SUBS_ENTRY_SIZE)


# ---- v1 Header 解析 ----
def parse_v1_header(raw: bytes):
    """解析 v1 64B 头部。返回 (version, max_points, max_subscribers)。"""
    if len(raw) < 44:
        raise ValueError("v1 header too short")
    magic, version, max_points, max_subscribers = struct.unpack_from(
        "<IIII", raw, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic: {magic:#x}")
    if version != SHM_VERSION_V1:
        raise ValueError(
            f"source segment is not v1 (version={version}); "
            f"本工具只迁移 v1 -> v2")
    if max_points == 0:
        raise ValueError("v1 header max_points == 0")
    return version, max_points, max_subscribers


# ---- v2 Header 构造 ----
def build_v2_header(max_points: int, max_subscribers: int) -> bytes:
    """构造 128B v2 Header（含 CRC32）。

    CRC 覆盖 [0:16]（magic/version/max_points/max_subscribers）
    与 [48:72]（flags/scan_skipped/off_points/off_index/off_meta/off_subs），
    与 src/core/irt_shm.c 的 irt_header_crc32_of 逐字节一致
    （zlib.crc32 即标准 CRC-32/IEEE 802.3，初值与末异或均为 0xFFFFFFFF）。
    """
    raw = bytearray(HEADER_V2_SIZE)
    op = off_points_v2()
    oi = off_index_v2(max_points)
    om = off_meta_v2(max_points)
    os_ = off_subs_v2(max_points)
    # [0:16] magic/version/max_points/max_subscribers
    struct.pack_into("<IIII", raw, 0, MAGIC, SHM_VERSION_V2,
                     max_points, max_subscribers)
    # [48:56] flags=0, scan_skipped=0
    struct.pack_into("<II", raw, 48, 0, 0)
    # [56:72] 四个区段偏移
    struct.pack_into("<IIII", raw, 56, op, oi, om, os_)
    # [44:48] crc32（先置 0 再算）
    crc = zlib.crc32(raw[0:16] + raw[48:72]) & 0xFFFFFFFF
    struct.pack_into("<I", raw, 44, crc)
    return bytes(raw)


# ---- 点位名读取（复刻 irt_index_name_eq 的取名字节）----
def read_point_name(points: bytes, point_id: int) -> bytes:
    base = point_id * POINT_SIZE + NAME_OFFSET
    chunk = points[base:base + NAME_LEN]
    nul = chunk.find(b"\x00")
    if nul < 0:
        return chunk
    return chunk[:nul]


def name_eq(points: bytes, point_id: int, name: bytes) -> bool:
    return read_point_name(points, point_id) == name


# ---- FNV-1a 32 哈希（复刻 irt_index.c:irt_hash_fnv1a32）----
def fnv1a32(name: bytes) -> int:
    h = 0x811C9DC5
    for b in name:
        if b == 0:
            break
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


# ---- 索引重建（复刻 irt_index_insert_locked 的纯逻辑）----
def build_index(points: bytes, max_points: int, capacity: int):
    """重建 name->id 开放寻址索引。

    返回 (slots: bytes[capacity*8], index_count: int)。
    算法与 src/core/irt_index.c 的 irt_index_insert_locked 逐字节一致：
    线性探测 + 墓碑复用；同名重注册时更新 id 且不增计数。
    """
    mask = capacity - 1
    slots = bytearray(capacity * INDEX_SLOT_SIZE)
    # 全 0xFF => point_id = EMPTY
    for i in range(capacity):
        struct.pack_into("<II", slots, i * INDEX_SLOT_SIZE, 0, INDEX_EMPTY)

    index_count = 0
    for pid in range(max_points):
        name = read_point_name(points, pid)
        if not name:
            continue
        h = fnv1a32(name)
        home = h & mask
        tomb = capacity
        placed = False
        updated = False
        for d in range(capacity):
            i = (home + d) & mask
            sh, spid = struct.unpack_from("<II", slots, i * INDEX_SLOT_SIZE)
            if spid == INDEX_EMPTY:
                dst = tomb if tomb != capacity else i
                struct.pack_into("<II", slots, dst * INDEX_SLOT_SIZE, h, pid)
                placed = True
                break
            if spid == INDEX_TOMBSTONE:
                if tomb == capacity:
                    tomb = i
                continue
            if sh == h and name_eq(points, spid, name):
                # 同名重注册：更新 id，不增计数（与 C 一致）
                struct.pack_into("<II", slots, i * INDEX_SLOT_SIZE, h, pid)
                updated = True
                placed = True
                break
        if not placed and tomb != capacity:
            struct.pack_into("<II", slots, tomb * INDEX_SLOT_SIZE, h, pid)
            placed = True
        if placed and not updated:
            index_count += 1
    return bytes(slots), index_count


# ---- 索引探测（供校验：按名定位 id）----
def index_probe(slots: bytes, capacity: int, points: bytes,
                name: bytes):
    """复刻 irt_index_probe。命中返回 point_id，否则 None。"""
    mask = capacity - 1
    h = fnv1a32(name)
    home = h & mask
    for d in range(capacity):
        i = (home + d) & mask
        sh, spid = struct.unpack_from("<II", slots, i * INDEX_SLOT_SIZE)
        if spid == INDEX_EMPTY:
            return None
        if spid == INDEX_TOMBSTONE:
            continue
        if sh == h and name_eq(points, spid, name):
            return spid
    return None
