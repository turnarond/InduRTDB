#!/usr/bin/env python3
"""rtdb-monitor 的 rtdbd UDS 协议客户端（纯 Python，零第三方依赖）。

复刻 rtdbd/include/rtdbd/protocol.h 的二进制报文，连接 rtdbd 守护进程，
提供：PING / GET（按 id 读值）/ LIST（枚举点位）/ WRITE（设值）/
FIND_BY_NAME / GET_META / SUBSCRIBE→NOTIFY（实时推送）。

布局（小端 <）与 C 端逐字节一致，改动协议结构须同步此处。
"""
from __future__ import annotations

import ctypes
import socket
import struct
import threading
from typing import Callable, Optional

# ---- 协议常量（与 protocol.h 一致） ----
RTDBD_MAGIC = 0x52424431
RTDBD_PROTO_VERSION = 2

# 操作码
OP_PING = 1
OP_WRITE = 2
OP_AUDIT_DUMP = 3
OP_SUBSCRIBE = 4
OP_UNSUBSCRIBE = 5
OP_NOTIFY = 6
OP_FIND_BY_NAME = 7
OP_GET_META = 8
OP_SET_META = 9
OP_GET = 10
OP_LIST = 11

# 状态码
ST_OK = 0
ST_BAD_REQUEST = 1
ST_DENIED = 2
ST_INTERNAL = 3
ST_NOT_FOUND = 4
ST_NOT_IMPLEMENTED = 9

# 点位类型（与 indurtdb.h 一致）
TYPE_BOOL = 0
TYPE_INT32 = 1
TYPE_DOUBLE = 2
TYPE_STRING = 3
TYPE_INT64 = 4
TYPE_UINT32 = 5
TYPE_FLOAT = 6

# ---- 结构体（小端，与 C 布局对齐） ----
REQ_HDR = struct.Struct("<IHHI")          # magic, version, opcode, payload_len
RESP_HDR = struct.Struct("<IHHI")         # magic, version, status, payload_len
WRITE_REQ = struct.Struct("<IBxxxQQ")     # point_id, type, reserved[3], value_bits, source_ts_ns
GET_REQ = struct.Struct("<I")             # point_id
GET_RESP = struct.Struct("<IBB2xQ32sQQ")  # point_id, type, quality, reserved[2], value_bits, value_str[32], ts, source_ts
LIST_REQ = struct.Struct("<II")           # max, offset
POINT_INFO = struct.Struct("<IBB2x64s")   # point_id, type, access, reserved[2], name[64]
NOTIFY = struct.Struct("<IB3xQQQ")        # point_id, type, reserved[3], value_bits, ts, source_ts
FIND_REQ = struct.Struct("<64s")          # name[64]
FIND_RESP = struct.Struct("<I")           # point_id
META_REQ = struct.Struct("<I")            # point_id
META_PAYLOAD = struct.Struct("<ddfI8x")   # eur_min, eur_max, deadband, flags, reserved[8]

TYPE_NAME = {
    TYPE_BOOL: "bool", TYPE_INT32: "int32", TYPE_DOUBLE: "double", TYPE_STRING: "string",
    TYPE_INT64: "int64", TYPE_UINT32: "uint32", TYPE_FLOAT: "float",
}
NAME_TYPE = {v: k for k, v in TYPE_NAME.items()}


class RtdbError(Exception):
    """rtdbd 协议层错误：status 非 OK 或连接异常。"""


def _decode_value(value_bits: int, value_str: bytes, ptype: int):
    """按类型从 value_bits / value_str 还原 Python 值。"""
    if ptype == TYPE_BOOL:
        return (value_bits & 1) != 0
    if ptype == TYPE_INT32:
        return ctypes.c_int32(value_bits & 0xFFFFFFFF).value
    if ptype == TYPE_INT64:
        return ctypes.c_int64(value_bits).value
    if ptype == TYPE_UINT32:
        return value_bits & 0xFFFFFFFF
    if ptype == TYPE_FLOAT:
        return struct.unpack("<f", struct.pack("<Q", value_bits)[:4])[0]
    if ptype == TYPE_DOUBLE:
        return struct.unpack("<d", struct.pack("<Q", value_bits))[0]
    if ptype == TYPE_STRING:
        return value_str.rstrip(b"\x00").decode("utf-8", "replace")
    return value_bits


def _encode_value(value, ptype: int) -> int:
    """按类型把 Python 值编码为 value_bits（u64）。STRING 不支持（8B 负载限制）。"""
    if ptype == TYPE_BOOL:
        return 1 if value else 0
    if ptype == TYPE_INT32:
        return ctypes.c_int32(int(value)).value & 0xFFFFFFFF
    if ptype == TYPE_INT64:
        return ctypes.c_int64(int(value)).value & 0xFFFFFFFFFFFFFFFF
    if ptype == TYPE_UINT32:
        return int(value) & 0xFFFFFFFF
    if ptype == TYPE_FLOAT:
        return struct.unpack("<Q", struct.pack("<f", float(value)) + b"\x00\x00\x00\x00")[0]
    if ptype == TYPE_DOUBLE:
        return struct.unpack("<Q", struct.pack("<d", float(value)))[0]
    raise RtdbError(f"type {ptype} not writable via 8B value_bits")


class RtdbClient:
    """请求/响应客户端（同步，单连接）。"""

    def __init__(self, sock_path: str, timeout: float = 2.0):
        self.sock_path = sock_path
        self.timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._lock = threading.Lock()

    # ---- 连接 ----
    def connect(self) -> "RtdbClient":
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(self.timeout)
        s.connect(self.sock_path)
        self._sock = s
        return self

    def close(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    # ---- 底层收发 ----
    def _request(self, opcode: int, payload: bytes = b"") -> tuple[int, bytes]:
        if not self._sock:
            raise RtdbError("not connected")
        req = REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION, opcode, len(payload))
        with self._lock:
            self._sock.sendall(req + payload)
            hdr = self._recv_exact(RESP_HDR.size)
            magic, version, status, plen = RESP_HDR.unpack(hdr)
            if magic != RTDBD_MAGIC:
                raise RtdbError("bad response magic")
            if version != RTDBD_PROTO_VERSION:
                raise RtdbError(f"unsupported server version {version}")
            body = self._recv_exact(plen) if plen else b""
        return status, body

    def _recv_exact(self, n: int) -> bytes:
        assert self._sock is not None
        buf = bytearray()
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise RtdbError("connection closed")
            buf += chunk
        return bytes(buf)

    def _ok(self, opcode: int, payload: bytes = b"") -> bytes:
        status, body = self._request(opcode, payload)
        if status != ST_OK:
            raise RtdbError(f"opcode {opcode} -> status {status}")
        return body

    # ---- 高层 API ----
    def ping(self) -> bool:
        status, _ = self._request(OP_PING)
        return status == ST_OK

    def get_point(self, point_id: int) -> dict:
        body = self._ok(OP_GET, GET_REQ.pack(point_id))
        pid, ptype, quality, vbits, vstr, ts, sts = GET_RESP.unpack(body)
        return {
            "id": pid,
            "type": ptype,
            "typeName": TYPE_NAME.get(ptype, str(ptype)),
            "quality": quality,
            "value": _decode_value(vbits, vstr, ptype),
            "ts": ts,
            "sourceTs": sts,
        }

    def list_points(self, max_per_page: int = 1024) -> list[dict]:
        """分页拉全量已注册点位，聚合成列表。"""
        out: list[dict] = []
        offset = 0
        while True:
            body = self._ok(OP_LIST, LIST_REQ.pack(max_per_page, offset))
            if not body:
                break
            n = len(body) // POINT_INFO.size
            for i in range(n):
                pid, ptype, access, name = POINT_INFO.unpack(
                    body[i * POINT_INFO.size:(i + 1) * POINT_INFO.size]
                )
                out.append({
                    "id": pid,
                    "type": ptype,
                    "typeName": TYPE_NAME.get(ptype, str(ptype)),
                    "access": access,
                    "name": name.rstrip(b"\x00").decode("utf-8", "replace"),
                })
            if n < max_per_page:
                break
            offset += n
        return out

    def write_point(self, point_id: int, ptype: int, value, source_ts_ns: int = 0) -> None:
        vbits = _encode_value(value, ptype)
        self._ok(OP_WRITE, WRITE_REQ.pack(point_id, ptype, vbits, source_ts_ns))

    def find_by_name(self, name: str) -> int:
        body = self._ok(OP_FIND_BY_NAME, FIND_REQ.pack(name.encode("utf-8")[:64].ljust(64, b"\x00")))
        return FIND_RESP.unpack(body)[0]

    def get_meta(self, point_id: int) -> dict:
        body = self._ok(OP_GET_META, META_REQ.pack(point_id))
        eur_min, eur_max, deadband, flags = META_PAYLOAD.unpack(body)
        return {"eurMin": eur_min, "eurMax": eur_max, "deadband": deadband, "flags": flags}


class NotifyListener:
    """订阅变更通知的后台连接（独立 socket，避免与请求/响应互相阻塞）。

    收到 NOTIFY 帧后回调 on_notify(point_id, info_dict)。
    """

    def __init__(self, sock_path: str, on_notify: Callable[[int, dict], None], timeout: float = 2.0):
        self.sock_path = sock_path
        self.on_notify = on_notify
        self.timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._subscribed: set[int] = set()

    def start(self) -> None:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(self.timeout)
        s.connect(self.sock_path)
        self._sock = s
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def subscribe(self, point_ids: list[int]) -> None:
        if not self._sock:
            raise RtdbError("listener not started")
        for pid in point_ids:
            self._sock.sendall(REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION, OP_SUBSCRIBE, 4))
            self._sock.sendall(struct.Struct("<I").pack(pid))
            self._subscribed.add(pid)

    def _loop(self) -> None:
        assert self._sock is not None
        try:
            while not self._stop.is_set():
                # 订阅确认回的是 RESP_HDR，NOTIFY 推送回的是 REQ_HDR（opcode=OP_NOTIFY）；
                # 两者二进制布局相同（magic, version, kind, payload_len），故统一按 REQ_HDR 解。
                hdr = self._recv_exact(REQ_HDR.size)
                magic, version, kind, plen = REQ_HDR.unpack(hdr)
                body = self._recv_exact(plen) if plen else b""
                if magic != RTDBD_MAGIC or version != RTDBD_PROTO_VERSION:
                    break
                if kind == OP_NOTIFY and plen == NOTIFY.size:
                    pid, ptype, _pad, vbits, ts, sts = NOTIFY.unpack(body)
                    self.on_notify(pid, {
                        "id": pid, "type": ptype,
                        "typeName": TYPE_NAME.get(ptype, str(ptype)),
                        "value": _decode_value(vbits, b"", ptype),
                        "ts": ts, "sourceTs": sts,
                    })
                # 其余（订阅确认等响应）忽略
        except (OSError, RtdbError):
            pass

    def _recv_exact(self, n: int) -> bytes:
        assert self._sock is not None
        buf = bytearray()
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise RtdbError("connection closed")
            buf += chunk
        return bytes(buf)

    def stop(self) -> None:
        self._stop.set()
        if self._sock:
            try:
                self._sock.close()
            finally:
                self._sock = None


if __name__ == "__main__":
    import sys
    path = sys.argv[1] if len(sys.argv) > 1 else "/run/indurtdb/default.sock"
    with RtdbClient(path) as c:
        print("ping:", c.ping())
        pts = c.list_points()
        print(f"points: {len(pts)}")
        for p in pts[:10]:
            print("  ", p)
