"""irt_migrate 工具自身单元测试（不依赖 C 库）。

既可用 pytest 收集（test_* 函数），也可直接 `python3 tests/test_migrate.py`
运行（无 pytest 环境时自检）。覆盖：v1/v2 布局尺寸、Header CRC、
FNV-1a、索引重建与探测、以及一次完整「合成 v1 段 -> 迁移 -> 独立校验」往返。
"""
import mmap
import os
import struct
import sys

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
    assert version == L.SHM_VERSION_V2
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


def test_migrate_roundtrip():
    sid = "ut_v1_xyz"
    dst = "ut_v2_xyz"
    for p in (L.shm_path(sid), L.shm_path(dst)):
        if os.path.exists(p):
            os.unlink(p)
    _write_v1_segment(sid, 16, 4, seed_names=8)
    M.migrate(sid, dst, source_ts_mode="zero", force=True)
    rep = V.validate(sid, dst, source_ts_mode="zero", sample=0)
    assert rep["point_mismatches"] == [], rep
    assert rep["index_mismatches"] == [], rep
    assert rep["crc_ok"] is True
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


def test_validate_rejects_nonv1_source():
    sid = "ut_bad_src"
    sp = L.shm_path(sid)
    if os.path.exists(sp):
        os.unlink(sp)
    N = 4
    total = L.total_size_v2(N, 0)
    fd = os.open(sp, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        os.ftruncate(fd, total)
        with mmap.mmap(fd, total, mmap.MAP_SHARED, mmap.PROT_WRITE) as mm:
            mm[0:L.HEADER_V2_SIZE] = L.build_v2_header(N, 0)
    finally:
        os.close(fd)
    try:
        M.read_v1_segment(sid)
        raise AssertionError("应拒绝非 v1 源段")
    except ValueError:
        pass
    finally:
        if os.path.exists(sp):
            os.unlink(sp)


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
