"""irt_migrate 工具自身单元测试（不依赖 C 库）。

既可用 pytest 收集（test_* 函数），也可直接 `python3 tests/test_migrate.py`
运行（无 pytest 环境时自检）。覆盖：v1/v2 布局尺寸、Header CRC、
FNV-1a、索引重建与探测、v1->v3 与 v2->v3 完整往返、版本拒绝。
"""
import mmap
import os
import struct
import sys
import zlib

# 让脚本可直接运行（无 pytest / 未 pip 安装时也能 import 包）
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from irt_migrate import layout as L
from irt_migrate import migrate as M
from irt_migrate import validate as V


def _write_v1_segment(instance_id, max_points, max_subscribers, *, seed_names=4):
    """在 /dev/shm 写一个合成 v1 段，返回 points 字节。"""
    path = L.shm_path(instance_id)
    if os.path.exists(path):
        os.unlink(path)
    total = L.total_size_v1(max_points, max_subscribers)
    points = bytearray(max_points * L.POINT_SIZE)
    for pid in range(max_points):
        base = pid * L.POINT_SIZE
        struct.pack_into("<i", points, base + 0, pid * 7)
        struct.pack_into("<Q", points, base + 32, pid * 1000)
        struct.pack_into("<B", points, base + 40, 1)   # type=int32
        struct.pack_into("<B", points, base + 41, 0)   # quality good
        if pid < seed_names:
            name = f"tag_{pid:03d}".encode()
            points[base + L.NAME_OFFSET:base + L.NAME_OFFSET + len(name)] = name
        struct.pack_into("<Q", points, base + L.SOURCE_TS_OFFSET, pid * 11)
    hdr = bytearray(L.HEADER_V1_SIZE)
    struct.pack_into("<IIII", hdr, 0, L.MAGIC, L.SHM_VERSION_V1,
                     max_points, max_subscribers)
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        os.ftruncate(fd, total)
        with mmap.mmap(fd, total, mmap.MAP_SHARED, mmap.PROT_WRITE) as mm:
            mm[0:L.HEADER_V1_SIZE] = hdr
            mm[L.HEADER_V1_SIZE:L.HEADER_V1_SIZE + len(points)] = points
    finally:
        os.close(fd)
    return bytes(points)


def _make_v2_header(max_points, max_subscribers):
    """构造 version=2 的 128B Header（基于当前版构造器改写版本并重封 CRC）。"""
    hdr = bytearray(L.build_v2_header(max_points, max_subscribers))
    struct.pack_into("<I", hdr, 4, L.SHM_VERSION_V2)
    struct.pack_into("<I", hdr, 44, 0)
    crc = zlib.crc32(hdr[0:16] + hdr[48:72]) & 0xFFFFFFFF
    struct.pack_into("<I", hdr, 44, crc)
    return bytes(hdr)


def _build_v2_segment(instance_id, max_points, max_subscribers, *,
                      seed_names=4, partition_seed=0, meta_seed=0xA5):
    """在 /dev/shm 写一个合成 v2 段，返回 (points, meta) 字节。

    partition_seed != 0 时在点位 120..123 写入非零垃圾（模拟 v2 reserved
    区残留），用于验证迁移会强制清零。
    """
    path = L.shm_path(instance_id)
    if os.path.exists(path):
        os.unlink(path)
    total = L.total_size_v2(max_points, max_subscribers)
    points = bytearray(max_points * L.POINT_SIZE)
    for pid in range(max_points):
        base = pid * L.POINT_SIZE
        struct.pack_into("<i", points, base + 0, 1000 + pid * 7)
        struct.pack_into("<d", points, base + 8, pid * 1.5)
        struct.pack_into("<Q", points, base + 32, pid * 1000)
        struct.pack_into("<B", points, base + 40, 1)   # type=int32
        struct.pack_into("<B", points, base + 41, 0)   # quality good
        if pid < seed_names:
            name = f"v2_tag_{pid:03d}".encode()
            points[base + L.NAME_OFFSET:base + L.NAME_OFFSET + len(name)] = name
        struct.pack_into("<Q", points, base + L.SOURCE_TS_OFFSET,
                         0x1111_0000 + pid)
        if partition_seed:
            struct.pack_into("<I", points, base + L.PARTITION_OFFSET,
                             (partition_seed + pid) & 0xFFFFFFFF)

    # v2 源索引（键不含分区，仅作填充；迁移会重建）
    cap = L.layout_buckets(max_points)
    slots, _ = L.build_index(bytes(points), max_points, cap)

    meta = bytearray(L.layout_meta_size(max_points))
    for pid in range(max_points):
        base = pid * L.META_SIZE_PER_POINT
        struct.pack_into("<I", meta, base + 0, 0xDEAD0000 + pid)
        struct.pack_into("<Q", meta, base + 8, 0x1234_0000 + pid)
        meta[base + 16:base + 20] = bytes([meta_seed, 1, 2, 3])

    raw = bytearray(total)
    raw[0:L.HEADER_V2_SIZE] = _make_v2_header(max_points, max_subscribers)
    op = L.off_points_v2()
    oi = L.off_index_v2(max_points)
    om = L.off_meta_v2(max_points)
    raw[op:op + len(points)] = points
    raw[oi:oi + len(slots)] = slots
    raw[om:om + len(meta)] = meta

    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        os.ftruncate(fd, total)
        with mmap.mmap(fd, total, mmap.MAP_SHARED, mmap.PROT_WRITE) as mm:
            mm[:] = raw
    finally:
        os.close(fd)
    return bytes(points), bytes(meta)


def _cleanup(*ids):
    for i in ids:
        p = L.shm_path(i)
        if os.path.exists(p):
            os.unlink(p)


def _point_equal_except(src_pts, dst_pts, pid, *, zero_ts):
    """比较单点：屏蔽 partition(120..124)；zero_ts 时再屏蔽 source_ts(112..120)。"""
    a = bytearray(src_pts[pid * L.POINT_SIZE:(pid + 1) * L.POINT_SIZE])
    b = bytearray(dst_pts[pid * L.POINT_SIZE:(pid + 1) * L.POINT_SIZE])
    if zero_ts:
        a[L.SOURCE_TS_OFFSET:L.SOURCE_TS_OFFSET + 8] = b"\x00" * 8
        b[L.SOURCE_TS_OFFSET:L.SOURCE_TS_OFFSET + 8] = b"\x00" * 8
    a[L.PARTITION_OFFSET:L.PARTITION_OFFSET + 4] = b"\x00" * 4
    b[L.PARTITION_OFFSET:L.PARTITION_OFFSET + 4] = b"\x00" * 4
    return bytes(a) == bytes(b)


def test_layout_sizes():
    assert L.HEADER_V1_SIZE == 64
    assert L.HEADER_V2_SIZE == 128
    assert L.POINT_SIZE == 128
    assert L.layout_buckets(16) == 32
    assert L.layout_buckets(1) == 8
    assert L.layout_index_size(16) == 32 * 8
    assert L.layout_meta_size(16) == 16 * 32


def test_fnv1a_matches_known():
    assert L.fnv1a32(b"a") == 0xE40C292C
    assert L.fnv1a32(b"") == 0x811C9DC5


def test_v2_header_crc_and_offsets():
    import zlib
    N = 16
    hdr = L.build_v2_header(N, 4)
    magic, version, mp, ms = struct.unpack_from("<IIII", hdr, 0)
    assert magic == L.MAGIC
    assert version == L.SHM_VERSION_CURRENT
    assert mp == N and ms == 4
    op, oi, om, os_ = struct.unpack_from("<IIII", hdr, 56)
    assert op == L.off_points_v2()
    assert oi == L.off_index_v2(N)
    assert om == L.off_meta_v2(N)
    assert os_ == L.off_subs_v2(N)
    crc = zlib.crc32(hdr[0:16] + hdr[48:72]) & 0xFFFFFFFF
    stored = struct.unpack_from("<I", hdr, 44)[0]
    assert crc == stored


def test_index_build_and_probe():
    N = 8
    points = bytearray(N * L.POINT_SIZE)
    names = {}
    for pid in range(N):
        name = f"p{pid}".encode()
        points[pid * L.POINT_SIZE + L.NAME_OFFSET:
               pid * L.POINT_SIZE + L.NAME_OFFSET + len(name)] = name
        names[name] = pid
    cap = L.layout_buckets(N)
    slots, count = L.build_index(bytes(points), N, cap)
    assert count == N
    for name, pid in names.items():
        assert L.index_probe(slots, cap, bytes(points), name) == pid


def test_compose_hash_matches_c_golden():
    """与 tests/unit/test_c_index.cpp:IndexHashGolden.MatchesPythonMirror 同一组黄金值。

    两侧漂移会让迁移后的段里 find_by_name 整体查不到（T2 实施中已实际踩到），
    故在此钉死；覆盖低位 / 高位分区与空名。
    """
    vec = [
        (0, b"temp", 0x047106E8),
        (0, b"tag.1.value", 0x39FFD406),
        (1, b"temp", 0x39DF487E),
        (255, b"pt.3", 0x437B37DD),
        (1023, b"x7", 0x865989B0),
        (0, b"", 0xAE44FAF5),
        (65535, b"dup", 0x8696BD80),
        (65536, b"temp", 0xFF29360A),
        (256, b"dup", 0xA8B6E02D),
        (16777216, b"dup", 0x9CF9FD66),
        (4294967295, b"max", 0x733CC768),
    ]
    for part, name, expect in vec:
        got = L.compose_hash(part, name)
        assert got == expect, (part, name, hex(got), hex(expect))


def test_index_partition_scoped():
    """索引键含 partition：同名跨分区互相隔离，未注册分区不得串名。"""
    N = 8
    points = bytearray(N * L.POINT_SIZE)

    def put(pid, name, part):
        off = pid * L.POINT_SIZE
        points[off + L.NAME_OFFSET: off + L.NAME_OFFSET + len(name)] = name
        struct.pack_into("<I", points, off + L.PARTITION_OFFSET, part)

    put(0, b"temp", 0)
    put(1, b"temp", 1)
    cap = L.layout_buckets(N)
    slots, count = L.build_index(bytes(points), N, cap)
    assert count == 2, f"同名跨分区应是两个独立条目, got {count}"
    assert L.index_probe(slots, cap, bytes(points), b"temp", 0) == 0
    assert L.index_probe(slots, cap, bytes(points), b"temp", 1) == 1
    assert L.index_probe(slots, cap, bytes(points), b"temp", 2) is None


def test_migrate_roundtrip():
    sid = "ut_v1_xyz"
    dst = "ut_v2_xyz"
    for p in (L.shm_path(sid), L.shm_path(dst)):
        if os.path.exists(p):
            os.unlink(p)
    _write_v1_segment(sid, 16, 4, seed_names=8)
    stat = M.migrate(sid, dst, source_ts_mode="zero", force=True)
    assert stat["source_version"] == L.SHM_VERSION_V1
    rep = V.validate(sid, dst, source_ts_mode="zero", sample=0)
    assert rep["point_mismatches"] == [], rep
    assert rep["index_mismatches"] == [], rep
    assert rep["crc_ok"] is True
    assert rep["source_version"] == L.SHM_VERSION_V1
    assert rep["partition_all_zero"] is True
    v2 = V.read_v2_segment(dst)
    for pid in range(16):
        sts = struct.unpack_from(
            "<Q", v2["points"], pid * L.POINT_SIZE + L.SOURCE_TS_OFFSET)[0]
        assert sts == 0, pid
    cap = L.layout_buckets(16)
    assert L.index_probe(v2["index"], cap, v2["points"], b"tag_002") == 2
    for p in (L.shm_path(sid), L.shm_path(dst)):
        if os.path.exists(p):
            os.unlink(p)


def test_v2_to_v3_preserves_points():
    """v2 -> v3：除 partition(120..123) 与按模式处理的 source_ts 外逐字节保留，
    且 v2 元数据区逐字节原样保留。"""
    sid = "ut_v2src_preserve"
    dst = "ut_v3dst_preserve"
    _cleanup(sid, dst)
    N, Msub = 16, 4
    try:
        src_points, src_meta = _build_v2_segment(
            sid, N, Msub, seed_names=8, partition_seed=0)
        stat = M.migrate(sid, dst, source_ts_mode="zero", force=True)
        assert stat["source_version"] == L.SHM_VERSION_V2

        dst_seg = V.read_v2_segment(dst)
        assert dst_seg["version"] == L.SHM_VERSION_V3
        assert dst_seg["max_points"] == N
        assert dst_seg["max_subscribers"] == Msub
        assert dst_seg["meta"] == src_meta, "v2 元数据区必须逐字节原样保留"

        for pid in range(N):
            assert _point_equal_except(src_points, dst_seg["points"], pid,
                                       zero_ts=True), pid
            sts = struct.unpack_from(
                "<Q", dst_seg["points"],
                pid * L.POINT_SIZE + L.SOURCE_TS_OFFSET)[0]
            assert sts == 0, f"pid={pid} source_ts 应置 0"

        # 索引按 (partition=0, name) 解析到正确 id
        cap = L.layout_buckets(N)
        for pid in range(8):
            name = L.read_point_name(src_points, pid)
            assert L.index_probe(dst_seg["index"], cap, dst_seg["points"],
                                 name, partition=0) == pid

        rep = V.validate(sid, dst, source_ts_mode="zero", sample=0)
        assert rep["source_version"] == L.SHM_VERSION_V2
        assert rep["partition_all_zero"] is True
        assert rep["meta_preserved"] is True
        assert rep["point_mismatches"] == [] and rep["index_mismatches"] == []
    finally:
        _cleanup(sid, dst)


def test_v2_to_v3_keep_source_ts():
    """--source-ts-mode keep：source_timestamp_ns 逐字节保留。"""
    sid = "ut_v2src_keepts"
    dst = "ut_v3dst_keepts"
    _cleanup(sid, dst)
    N, Msub = 8, 2
    try:
        src_points, _ = _build_v2_segment(sid, N, Msub, seed_names=4)
        M.migrate(sid, dst, source_ts_mode="keep", force=True)
        dst_seg = V.read_v2_segment(dst)
        for pid in range(N):
            assert _point_equal_except(src_points, dst_seg["points"], pid,
                                       zero_ts=False), pid
            got = struct.unpack_from(
                "<Q", dst_seg["points"],
                pid * L.POINT_SIZE + L.SOURCE_TS_OFFSET)[0]
            assert got == 0x1111_0000 + pid, pid
        rep = V.validate(sid, dst, source_ts_mode="keep", sample=0)
        assert rep["meta_preserved"] is True
    finally:
        _cleanup(sid, dst)


def test_v2_to_v3_partition_zeroed():
    """v2 源段 120..123 的非零残留必须被强制清零。"""
    sid = "ut_v2src_part"
    dst = "ut_v3dst_part"
    _cleanup(sid, dst)
    N, Msub = 8, 2
    try:
        src_points, _ = _build_v2_segment(
            sid, N, Msub, seed_names=4, partition_seed=0xCAFEBABE)
        for pid in range(N):
            assert L.read_point_partition(src_points, pid) != 0, pid
        M.migrate(sid, dst, source_ts_mode="zero", force=True)
        dst_seg = V.read_v2_segment(dst)
        for pid in range(N):
            assert L.read_point_partition(dst_seg["points"], pid) == 0, pid
        rep = V.validate(sid, dst, source_ts_mode="zero", sample=0)
        assert rep["partition_all_zero"] is True
    finally:
        _cleanup(sid, dst)


def test_v2_to_v3_checksum():
    """目标段 CRC 正确、validate 通过；篡改一个字节后 validate 必须失败。"""
    sid = "ut_v2src_crc"
    dst = "ut_v3dst_crc"
    _cleanup(sid, dst)
    N, Msub = 8, 2
    try:
        _build_v2_segment(sid, N, Msub, seed_names=4)
        M.migrate(sid, dst, source_ts_mode="zero", force=True)
        with open(L.shm_path(dst), "rb") as f:
            hdr = f.read(L.HEADER_V2_SIZE)
        stored = struct.unpack_from("<I", hdr, 44)[0]
        calc = zlib.crc32(hdr[0:16] + hdr[48:72]) & 0xFFFFFFFF
        assert stored == calc
        assert struct.unpack_from("<I", hdr, 4)[0] == L.SHM_VERSION_V3
        V.validate(sid, dst, source_ts_mode="zero", sample=0)

        # 篡改点位区一个字节 -> CRC 不受影响，但点位比对必须失败
        path = L.shm_path(dst)
        fd = os.open(path, os.O_RDWR)
        try:
            off = L.off_points_v2() + 5
            orig = os.pread(fd, 1, off)
            os.pwrite(fd, bytes([orig[0] ^ 0xFF]), off)
        finally:
            os.close(fd)
        try:
            V.validate(sid, dst, source_ts_mode="zero", sample=0)
            raise AssertionError("篡改点位后 validate 应失败")
        except ValueError:
            pass

        # 篡改 Header 的 CRC 覆盖区 -> CRC 重算必须失败
        fd = os.open(path, os.O_RDWR)
        try:
            orig = os.pread(fd, 1, 60)      # off_index 字节，在 CRC 范围 [48:72]
            os.pwrite(fd, bytes([orig[0] ^ 0xFF]), 60)
        finally:
            os.close(fd)
        try:
            V.validate(sid, dst, source_ts_mode="zero", sample=0)
            raise AssertionError("篡改 Header CRC 覆盖区后 validate 应失败")
        except ValueError as e:
            assert "CRC" in str(e), e
    finally:
        _cleanup(sid, dst)


def test_refuses_wrong_source_version():
    """v3（已是目标格式）与未知版本（如 7）都必须拒绝作为迁移源。"""
    cases = [
        ("ut_src_v3", L.SHM_VERSION_V3),
        ("ut_src_bogus", 7),
    ]
    for sid, version in cases:
        _cleanup(sid)
        N = 4
        total = L.total_size_v2(N, 0)
        hdr = bytearray(L.build_v2_header(N, 0))
        struct.pack_into("<I", hdr, 4, version)
        struct.pack_into("<I", hdr, 44, 0)
        struct.pack_into("<I", hdr, 44,
                         zlib.crc32(hdr[0:16] + hdr[48:72]) & 0xFFFFFFFF)
        fd = os.open(L.shm_path(sid), os.O_CREAT | os.O_RDWR, 0o600)
        try:
            os.ftruncate(fd, total)
            with mmap.mmap(fd, total, mmap.MAP_SHARED,
                           mmap.PROT_WRITE) as mm:
                mm[0:L.HEADER_V2_SIZE] = hdr
        finally:
            os.close(fd)
        try:
            M.read_source_segment(sid)
            raise AssertionError(f"应拒绝 version={version} 源段")
        except ValueError as e:
            assert "not supported" in str(e), e
        try:
            M.migrate(sid, sid + "_dst", force=True)
            raise AssertionError(f"migrate 应拒绝 version={version} 源段")
        except ValueError:
            pass
        finally:
            _cleanup(sid, sid + "_dst")


def test_read_source_dispatches_v1_and_v2():
    """调度器：v1 -> v1 读取器，v2 -> v2 读取器（带 meta）。"""
    sid1 = "ut_dispatch_v1"
    sid2 = "ut_dispatch_v2"
    _cleanup(sid1, sid2)
    try:
        _write_v1_segment(sid1, 8, 2, seed_names=2)
        s1 = M.read_source_segment(sid1)
        assert s1["version"] == L.SHM_VERSION_V1
        assert "meta" not in s1

        _build_v2_segment(sid2, 8, 2, seed_names=2)
        s2 = M.read_source_segment(sid2)
        assert s2["version"] == L.SHM_VERSION_V2
        assert len(s2["meta"]) == L.layout_meta_size(8)
    finally:
        _cleanup(sid1, sid2)


def _run_all():
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    failed = 0
    for fn in tests:
        try:
            fn()
            print(f"  PASS  {fn.__name__}")
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"  FAIL  {fn.__name__}: {e}")
    print(f"{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
