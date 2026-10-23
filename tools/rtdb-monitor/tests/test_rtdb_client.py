#!/usr/bin/env python3
"""rtdb_client 协议编解码单测（不依赖 rtdbd 进程，纯结构校验）。"""
import socket
import struct
import sys
import os
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from rtdb_client import (  # noqa: E402
    REQ_HDR, RESP_HDR, WRITE_REQ, GET_REQ, GET_RESP, LIST_REQ, POINT_INFO,
    NOTIFY, FIND_REQ, FIND_RESP, META_REQ, META_PAYLOAD,
    CREATE_REQ, DELETE_REQ, RENAME_REQ,
    LOG_REQ, LOG_ENTRY, LOG_LEVEL_NAME, LOG_INFO, LOG_WARN, LOG_ERROR,
    OP_CREATE_POINT, OP_DELETE_POINT, OP_RENAME_POINT, OP_GET_LOG,
    OP_SET_SUBS, OP_BATCH_READ, OP_BATCH_WRITE, OP_CAPS, OP_SNAPSHOT_DONE,
    RTDBD_MAGIC, RTDBD_PROTO_VERSION, PARTITION_ALL,
    RTDBD_SET_SUBS_MAX, RTDBD_LEGACY_SET_SUBS_MAX, RTDBD_BATCH_MAX,
    RtdbClient, NotifyListener,
    probe_set_subs, probe_caps,
    CAPS, BATCH_READ_ITEM, BATCH_WRITE_ITEM, SNAPSHOT_DONE,
    CAPS_F_BATCH_READ, CAPS_F_BATCH_WRITE, CAPS_F_SNAPSHOT_DONE,
    SNAPSHOT_DONE_F_COMPLETE, NOTIFY_F_SNAPSHOT, ST_NO_MEM,
    RtdbError, NAME_TYPE,
    _decode_value, _encode_value, TYPE_INT32, TYPE_INT64, TYPE_UINT32,
    TYPE_FLOAT, TYPE_DOUBLE, TYPE_BOOL,
)


def test_struct_sizes_match_c_layout():
    # 与 rtdbd/include/rtdbd/protocol.h 的 static_assert 保持一致（v3：末尾追加 partition）
    assert RTDBD_PROTO_VERSION == 3
    assert PARTITION_ALL == 0xFFFFFFFF
    assert REQ_HDR.size == 12
    assert RESP_HDR.size == 12
    assert WRITE_REQ.size == 24
    assert GET_REQ.size == 4
    assert GET_RESP.size == 64
    assert LIST_REQ.size == 12          # max + offset + partition（v3）
    assert POINT_INFO.size == 76        # ... name[64] + partition（v3）
    assert NOTIFY.size == 32
    assert FIND_REQ.size == 68          # name[64] + partition（v3）
    assert FIND_RESP.size == 4
    assert META_REQ.size == 4
    assert META_PAYLOAD.size == 32
    # v3.6 CRUD（v3：末尾追加 partition）
    assert CREATE_REQ.size == 76
    assert DELETE_REQ.size == 8
    assert RENAME_REQ.size == 72
    # v3.6 运行日志
    assert LOG_REQ.size == 4
    assert LOG_ENTRY.size == 128
    # v3.11 批量/能力协商/快照屏障
    assert CAPS.size == 32
    assert BATCH_READ_ITEM.size + GET_RESP.size == 72
    assert BATCH_WRITE_ITEM.size == 8
    assert SNAPSHOT_DONE.size == 8


def test_v311_opcodes_and_flags():
    assert (OP_BATCH_READ, OP_BATCH_WRITE, OP_CAPS, OP_SNAPSHOT_DONE) == (19, 20, 21, 22)
    assert RTDBD_SET_SUBS_MAX == 10000
    assert RTDBD_LEGACY_SET_SUBS_MAX == 1024
    assert RTDBD_BATCH_MAX == 1024
    assert ST_NO_MEM == 5
    assert CAPS_F_BATCH_READ == 0x01
    assert CAPS_F_BATCH_WRITE == 0x02
    assert CAPS_F_SNAPSHOT_DONE == 0x04
    assert SNAPSHOT_DONE_F_COMPLETE == 0x01
    assert NOTIFY_F_SNAPSHOT == 0x01


def test_crud_opcodes():
    assert (OP_CREATE_POINT, OP_DELETE_POINT, OP_RENAME_POINT) == (12, 13, 14)


def test_crud_payload_roundtrip():
    name = b"Test.New_Point".ljust(64, b"\x00")
    b = CREATE_REQ.pack(20, TYPE_INT32, 3, name, 1)
    assert len(b) == 76
    pid, ptype, access, nm, part = CREATE_REQ.unpack(b)
    assert pid == 20 and ptype == TYPE_INT32 and access == 3 and part == 1
    assert nm.rstrip(b"\x00") == b"Test.New_Point"

    b2 = RENAME_REQ.pack(20, b"Test.Renamed".ljust(64, b"\x00"), 1)
    assert len(b2) == 72
    pid2, nm2, part2 = RENAME_REQ.unpack(b2)
    assert pid2 == 20 and nm2.rstrip(b"\x00") == b"Test.Renamed" and part2 == 1

    b3 = DELETE_REQ.pack(20, 1)
    assert len(b3) == 8 and DELETE_REQ.unpack(b3) == (20, 1)


def test_partition_payload_roundtrip():
    """v3 名称域请求体末尾 partition 的正确打包/解包往返。"""
    # LIST 请求：max, offset, partition
    b = LIST_REQ.pack(1024, 0, PARTITION_ALL)
    assert len(b) == 12
    assert LIST_REQ.unpack(b) == (1024, 0, PARTITION_ALL)
    assert LIST_REQ.unpack(LIST_REQ.pack(10, 5, 1)) == (10, 5, 1)

    # FIND 请求：name[64], partition
    b = FIND_REQ.pack(b"Pump_Start".ljust(64, b"\x00"), 2)
    assert len(b) == 68
    nm, part = FIND_REQ.unpack(b)
    assert nm.rstrip(b"\x00") == b"Pump_Start" and part == 2

    # POINT_INFO 响应项：以 partition 结尾
    b = POINT_INFO.pack(21, TYPE_INT32, 3, b"temp".ljust(64, b"\x00"), 1)
    assert len(b) == 76
    pid, ptype, access, nm, part = POINT_INFO.unpack(b)
    assert (pid, ptype, access, part) == (21, TYPE_INT32, 3, 1)
    assert nm.rstrip(b"\x00") == b"temp"


def test_log_entry_roundtrip():
    b = LOG_ENTRY.pack(1700000000123456789, LOG_WARN,
                       b"rtdbd: worker 42 exited, respawning".ljust(116, b"\x00"))
    assert len(b) == 128
    ts, level, msg = LOG_ENTRY.unpack(b)
    assert ts == 1700000000123456789 and level == LOG_WARN
    assert msg.rstrip(b"\x00") == b"rtdbd: worker 42 exited, respawning"

    b2 = LOG_REQ.pack(0)
    assert len(b2) == 4 and LOG_REQ.unpack(b2)[0] == 0
    assert OP_GET_LOG == 15
    assert LOG_LEVEL_NAME[LOG_INFO] == "INFO"
    assert LOG_LEVEL_NAME[LOG_ERROR] == "ERROR"


def test_create_point_type_resolution():
    """create_point 接受类型名；未知类型名必须报错（避免静默写成非法 type）。"""
    captured = {}

    class FakeClient:
        def __init__(self):
            pass

        def _ok(self, opcode, payload):
            captured["opcode"] = opcode
            captured["payload"] = payload
            return b""

    # 复用 RtdbClient 的方法实现（不建连）
    from rtdb_client import RtdbClient
    c = FakeClient()
    RtdbClient.create_point(c, 20, "Test.New_Point", "int32", 3)
    assert captured["opcode"] == OP_CREATE_POINT
    pid, ptype, access, nm, part = CREATE_REQ.unpack(captured["payload"])
    assert (pid, ptype, access, part) == (20, TYPE_INT32, 3, 0)  # 默认分区 0
    assert nm.rstrip(b"\x00") == b"Test.New_Point"

    # 显式分区
    RtdbClient.create_point(c, 21, "Part.One", "int32", 3, partition=1)
    assert CREATE_REQ.unpack(captured["payload"])[4] == 1

    try:
        RtdbClient.create_point(c, 21, "Bad", "int128")
        raise AssertionError("unknown type name should raise")
    except RtdbError:
        pass

    assert NAME_TYPE["string"] == 3



def test_req_resp_hdr_layout():
    b = REQ_HDR.pack(0x52424431, RTDBD_PROTO_VERSION, 10, 4)
    assert len(b) == 12
    magic, version, opcode, plen = REQ_HDR.unpack(b)
    assert magic == 0x52424431 and version == 3 and opcode == 10 and plen == 4


def test_value_roundtrip_numeric():
    # int32 负值
    bits = _encode_value(-12345, TYPE_INT32)
    assert _decode_value(bits, b"", TYPE_INT32) == -12345
    # int64
    bits = _encode_value(9000000000, TYPE_INT64)
    assert _decode_value(bits, b"", TYPE_INT64) == 9000000000
    # uint32
    bits = _encode_value(4000000000, TYPE_UINT32)
    assert _decode_value(bits, b"", TYPE_UINT32) == 4000000000
    # float
    bits = _encode_value(3.5, TYPE_FLOAT)
    assert abs(_decode_value(bits, b"", TYPE_FLOAT) - 3.5) < 1e-6
    # double
    bits = _encode_value(2.718281828, TYPE_DOUBLE)
    assert abs(_decode_value(bits, b"", TYPE_DOUBLE) - 2.718281828) < 1e-12
    # bool
    assert _decode_value(_encode_value(True, TYPE_BOOL), b"", TYPE_BOOL) is True
    assert _decode_value(_encode_value(False, TYPE_BOOL), b"", TYPE_BOOL) is False


def test_get_resp_roundtrip():
    import ctypes
    vbits = ctypes.c_int32(-7).value & 0xFFFFFFFF
    b = GET_RESP.pack(10, TYPE_INT32, 0, vbits, b"motor1\x00" * 4, 111, 222)
    assert len(b) == 64
    pid, ptype, quality, vb, vstr, ts, sts = GET_RESP.unpack(b)
    assert pid == 10 and ptype == TYPE_INT32 and vb == vbits


def test_point_info_roundtrip():
    b = POINT_INFO.pack(11, TYPE_INT32, 3, b"Pump.Start\x00\x00\x00\x00\x00\x00\x00\x00", 0)
    assert len(b) == 76
    pid, ptype, access, name, part = POINT_INFO.unpack(b)
    assert pid == 11 and access == 3 and part == 0 and name.rstrip(b"\x00") == b"Pump.Start"



def _recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("closed")
        buf += chunk
    return bytes(buf)


def _mock_server(status=0, body=b""):
    """返回 (path, captured, thread)。服务端处理一次请求后回 RESP_HDR + body。"""
    td = tempfile.mkdtemp(prefix="rtdb_client_")
    path = os.path.join(td, "s.sock")
    captured = {}
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(1)

    def run():
        conn, _ = srv.accept()
        try:
            hdr = _recv_exact(conn, REQ_HDR.size)
            magic, version, opcode, plen = REQ_HDR.unpack(hdr)
            payload = _recv_exact(conn, plen) if plen else b""
            captured.update(magic=magic, version=version, opcode=opcode, payload=payload)
            conn.sendall(RESP_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION, status, len(body)) + body)
        finally:
            conn.close()
            srv.close()

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return path, captured, t


def test_set_subs_roundtrip():
    path, captured, t = _mock_server()
    with RtdbClient(path) as c:
        c.set_subs([3, 1, 2])
    t.join(2)
    assert captured["opcode"] == OP_SET_SUBS == 18
    assert captured["version"] == RTDBD_PROTO_VERSION
    count = struct.unpack("<I", captured["payload"][:4])[0]
    ids = struct.unpack("<3I", captured["payload"][4:])
    assert count == 3 and ids == (3, 1, 2)


def test_set_subs_legacy_cap():
    path, captured, t = _mock_server()
    with RtdbClient(path) as c:
        c.set_subs(list(range(RTDBD_LEGACY_SET_SUBS_MAX + 1)))
    t.join(2)
    count = struct.unpack("<I", captured["payload"][:4])[0]
    assert count == RTDBD_LEGACY_SET_SUBS_MAX


def test_set_subs_negotiated_cap():
    path, captured, t = _mock_server()
    with RtdbClient(path) as c:
        c.max_subs = RTDBD_SET_SUBS_MAX
        c.set_subs(list(range(RTDBD_SET_SUBS_MAX + 1)))
    t.join(2)
    count = struct.unpack("<I", captured["payload"][:4])[0]
    assert count == RTDBD_SET_SUBS_MAX


def test_set_subs_empty_payload():
    path, captured, t = _mock_server()
    with RtdbClient(path) as c:
        c.set_subs([])
    t.join(2)
    assert captured["opcode"] == OP_SET_SUBS
    assert captured["payload"] == struct.pack("<I", 0)


def test_set_subs_error_status():
    path, _captured, t = _mock_server(status=1)
    with RtdbClient(path) as c:
        try:
            c.set_subs([1])
            raise AssertionError("BAD_REQUEST should raise RtdbError")
        except RtdbError:
            pass
    t.join(2)


def test_probe_falls_back_on_error():
    class OldClient:
        def set_subs(self, ids):
            raise RtdbError("unknown opcode")

    assert probe_set_subs(OldClient()) is False

    path, _captured, t = _mock_server(status=1)
    with RtdbClient(path) as c:
        assert probe_set_subs(c) is False
    t.join(2)


def test_caps_roundtrip_and_limits():
    body = CAPS.pack(
        RTDBD_PROTO_VERSION, 128, 32, 10000, 1024, 1024,
        CAPS_F_BATCH_READ | CAPS_F_BATCH_WRITE | CAPS_F_SNAPSHOT_DONE, 0,
    )
    path, captured, t = _mock_server(body=body)
    with RtdbClient(path) as c:
        caps = c.caps()
        assert caps["max_clients"] == 128
        assert caps["max_subs_per_conn"] == 10000
        assert caps["max_batch"] == 1024
        assert c.max_subs == 10000
        assert c.max_batch == 1024
        # 协商后 SET_SUBS 不再按旧 1024 截断
        c.max_subs = 10000
        c._ok = lambda opcode, payload=b"": b""
        c.set_subs(list(range(1500)))
    t.join(2)
    assert captured["opcode"] == OP_CAPS
    assert captured["payload"] == b""


def test_probe_caps_returns_none_on_old_server():
    path, _captured, t = _mock_server(status=1)
    with RtdbClient(path) as c:
        assert probe_caps(c) is None
    t.join(2)


def test_batch_read_roundtrip():
    item = BATCH_READ_ITEM.pack(10, 0, 0) + GET_RESP.pack(
        10, TYPE_INT32, 2, 42, b"", 111, 222,
    )
    fail = BATCH_READ_ITEM.pack(11, 4, 0) + GET_RESP.pack(
        0, TYPE_INT32, 0, 0, b"", 0, 0,
    )
    body = struct.pack("<I", 2) + item + fail
    path, captured, t = _mock_server(body=body)
    with RtdbClient(path) as c:
        rows = c.batch_read([10, 11])
    t.join(2)
    assert captured["opcode"] == OP_BATCH_READ
    assert struct.unpack("<I", captured["payload"][:4])[0] == 2
    assert struct.unpack("<2I", captured["payload"][4:]) == (10, 11)
    assert rows[0]["id"] == 10 and rows[0]["status"] == 0
    assert rows[0]["value"] == 42 and rows[0]["quality"] == 2
    assert rows[1]["id"] == 11 and rows[1]["status"] == 4


def test_batch_write_roundtrip():
    body = struct.pack("<I", 2) + BATCH_WRITE_ITEM.pack(5, 0, 0) + \
        BATCH_WRITE_ITEM.pack(6, 4, 0)
    path, captured, t = _mock_server(body=body)
    with RtdbClient(path) as c:
        rows = c.batch_write([(5, TYPE_INT32, 7), (6, TYPE_DOUBLE, 1.5)])
    t.join(2)
    assert captured["opcode"] == OP_BATCH_WRITE
    assert struct.unpack("<I", captured["payload"][:4])[0] == 2
    assert rows == [{"id": 5, "status": 0}, {"id": 6, "status": 4}]


def test_batch_size_guard():
    class Dummy:
        max_batch = 2
        def _ok(self, opcode, payload=b""):
            raise AssertionError("must reject before sending")

    for call in (lambda: RtdbClient.batch_read(Dummy(), []),
                 lambda: RtdbClient.batch_read(Dummy(), [1, 2, 3]),
                 lambda: RtdbClient.batch_write(Dummy(), []),
                 lambda: RtdbClient.batch_write(Dummy(), [(1, TYPE_INT32, 1)] * 3)):
        try:
            call()
            raise AssertionError("batch guard did not fire")
        except RtdbError:
            pass


def test_notify_snapshot_flag_unpack():
    got = {}
    ev = threading.Event()

    def on_notify(pid, info):
        got.update(info)
        ev.set()

    l = NotifyListener("/tmp/unused-notify.sock", on_notify)
    server, client = socket.socketpair()
    l._sock = client
    t = threading.Thread(target=l._loop, daemon=True)
    t.start()
    body = bytearray(NOTIFY.pack(8, TYPE_INT32, 0, 42, 111, 222))
    body[7] = NOTIFY_F_SNAPSHOT
    server.sendall(REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION, 6, len(body)) + body)
    assert ev.wait(2)
    assert got["id"] == 8 and got["snapshot"] is True
    l.stop()
    server.close()
    t.join(2)


def test_notify_quality_unpack():
    got = {}
    ev = threading.Event()

    def on_notify(pid, info):
        got["pid"] = pid
        got.update(info)
        ev.set()

    l = NotifyListener("/tmp/unused-notify.sock", on_notify)
    server, client = socket.socketpair()
    l._sock = client
    t = threading.Thread(target=l._loop, daemon=True)
    t.start()
    body = NOTIFY.pack(7, TYPE_INT32, 2, 42, 111, 222)
    server.sendall(REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION, 6, len(body)) + body)
    assert ev.wait(2)
    assert got["pid"] == 7
    assert got["quality"] == 2
    assert got["value"] == 42
    l.stop()
    server.close()
    t.join(2)


def test_listener_snapshot_done_callback():
    got = {}
    ev = threading.Event()

    def on_done(info):
        got.update(info)
        ev.set()

    l = NotifyListener("/tmp/unused-notify.sock", lambda pid, info: None,
                       on_snapshot_done=on_done)
    server, client = socket.socketpair()
    l._sock = client
    t = threading.Thread(target=l._loop, daemon=True)
    t.start()
    body = SNAPSHOT_DONE.pack(3, SNAPSHOT_DONE_F_COMPLETE)
    server.sendall(REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION, OP_SNAPSHOT_DONE,
                                len(body)) + body)
    assert ev.wait(2)
    assert got == {"count": 3, "flags": SNAPSHOT_DONE_F_COMPLETE}
    l.stop()
    server.close()
    t.join(2)


def test_listener_set_subs_replaces_subscription():
    l = NotifyListener("/tmp/unused-notify.sock", lambda pid, info: None)
    server, client = socket.socketpair()
    l._sock = client
    l.set_subs([5, 6])
    hdr = _recv_exact(server, REQ_HDR.size)
    magic, version, opcode, plen = REQ_HDR.unpack(hdr)
    payload = _recv_exact(server, plen)
    assert (magic, version, opcode) == (RTDBD_MAGIC, RTDBD_PROTO_VERSION, OP_SET_SUBS)
    assert struct.unpack("<I", payload[:4])[0] == 2
    assert struct.unpack("<2I", payload[4:]) == (5, 6)
    assert l._subscribed == {5, 6}
    l.stop()
    server.close()


if __name__ == "__main__":
    test_struct_sizes_match_c_layout()
    test_req_resp_hdr_layout()
    test_value_roundtrip_numeric()
    test_get_resp_roundtrip()
    test_point_info_roundtrip()
    test_crud_opcodes()
    test_crud_payload_roundtrip()
    test_partition_payload_roundtrip()
    test_log_entry_roundtrip()
    test_create_point_type_resolution()
    print("ALL OK")
