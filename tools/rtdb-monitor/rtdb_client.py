#!/usr/bin/env python3
"""rtdb-monitor 的 rtdbd UDS 协议客户端（纯 Python，零第三方依赖）。

复刻 rtdbd/include/rtdbd/protocol.h 的二进制报文，连接 rtdbd 守护进程，
提供：PING / GET（按 id 读值）/ LIST（枚举点位）/ WRITE（设值）/
FIND_BY_NAME / GET_META / SUBSCRIBE→NOTIFY（实时推送）。

协议版本：**v3**（v3.9 T5 起，点位分区）。名称域请求体
（FIND_BY_NAME / CREATE_POINT / RENAME_POINT / DELETE_POINT / LIST）在末尾追加
`uint32 partition`；LIST 响应项 `POINT_INFO` 追加 `partition`。
`OP_GET/WRITE/GET_META/GET_LOG` 负载未变。v2 及更早客户端被 rtdbd 拒绝。

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
RTDBD_PROTO_VERSION = 3

# OP_LIST 的分区哨兵：请求 partition == 该值时列出**全部**分区点位（不过滤）。
PARTITION_ALL = 0xFFFFFFFF

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
OP_CREATE_POINT = 12
OP_DELETE_POINT = 13
OP_RENAME_POINT = 14
OP_GET_LOG = 15
OP_SET_SUBS = 18
OP_BATCH_READ = 19
OP_BATCH_WRITE = 20
OP_CAPS = 21
OP_SNAPSHOT_DONE = 22

# v3.11 协议上限；协商成功前按旧服务端 1024 保守回退。
RTDBD_SET_SUBS_MAX = 10000
RTDBD_LEGACY_SET_SUBS_MAX = 1024
RTDBD_BATCH_MAX = 1024

CAPS_F_BATCH_READ = 0x01
CAPS_F_BATCH_WRITE = 0x02
CAPS_F_SNAPSHOT_DONE = 0x04
SNAPSHOT_DONE_F_COMPLETE = 0x01
NOTIFY_F_SNAPSHOT = 0x01

# 状态码
ST_OK = 0
ST_BAD_REQUEST = 1
ST_DENIED = 2
ST_INTERNAL = 3
ST_NOT_FOUND = 4
ST_NO_MEM = 5
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
LIST_REQ = struct.Struct("<III")          # max, offset, partition（v3）
POINT_INFO = struct.Struct("<IBB2x64sI")  # point_id, type, access, reserved[2], name[64], partition（v3，76B）
NOTIFY = struct.Struct("<IBB2xQQQ")      # point_id, type, quality, reserved[2], value_bits, ts, source_ts
FIND_REQ = struct.Struct("<64sI")         # name[64], partition（v3，68B）
FIND_RESP = struct.Struct("<I")           # point_id
META_REQ = struct.Struct("<I")            # point_id
META_PAYLOAD = struct.Struct("<ddfI8x")   # eur_min, eur_max, deadband, flags, reserved[8]
CREATE_REQ = struct.Struct("<IBB2x64sI")  # point_id, type, access, reserved[2], name[64], partition（v3，76B）
DELETE_REQ = struct.Struct("<II")         # point_id, partition（v3，8B）
RENAME_REQ = struct.Struct("<I64sI")      # point_id, name[64], partition（v3，72B）
LOG_REQ = struct.Struct("<I")             # max
LOG_ENTRY = struct.Struct("<QB3x116s")    # ts_ns, level, reserved[3], msg[116]

# v3.11：CAPS 32B；批量读写逐项状态；SNAPSHOT_DONE 8B。
CAPS = struct.Struct("<IIIIIIII")          # proto, max_clients, initial_clients, max_subs, initial_subs, max_batch, flags, reserved
BATCH_READ_ITEM = struct.Struct("<IHH")    # point_id, status, reserved（后接 GET_RESP 64B）
BATCH_WRITE_ITEM = struct.Struct("<IHH")   # point_id, status, reserved
SNAPSHOT_DONE = struct.Struct("<II")       # count, flags

# 日志级别（与 protocol.h 的 RTDBD_LOG_* 一致）
LOG_INFO = 0
LOG_WARN = 1
LOG_ERROR = 2
LOG_LEVEL_NAME = {LOG_INFO: "INFO", LOG_WARN: "WARN", LOG_ERROR: "ERROR"}

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
        self.caps_info: Optional[dict] = None
        self.max_subs = RTDBD_LEGACY_SET_SUBS_MAX
        self.max_batch = RTDBD_BATCH_MAX

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

    @property
    def connected(self) -> bool:
        return self._sock is not None

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

    def list_points(self, max_per_page: int = 1024, partition: Optional[int] = None) -> list[dict]:
        """分页拉全量已注册点位，聚合成列表。

        partition 为 None 时用 PARTITION_ALL（列出全部分区，保留 v3.8 行为）；
        指定分区号则只返回该分区的点位。每项均携带其 `partition` 字段。
        """
        filter_partition = PARTITION_ALL if partition is None else int(partition)
        out: list[dict] = []
        offset = 0
        while True:
            body = self._ok(OP_LIST, LIST_REQ.pack(max_per_page, offset, filter_partition))
            if not body:
                break
            n = len(body) // POINT_INFO.size
            for i in range(n):
                pid, ptype, access, name, part = POINT_INFO.unpack(
                    body[i * POINT_INFO.size:(i + 1) * POINT_INFO.size]
                )
                out.append({
                    "id": pid,
                    "type": ptype,
                    "typeName": TYPE_NAME.get(ptype, str(ptype)),
                    "access": access,
                    "name": name.rstrip(b"\x00").decode("utf-8", "replace"),
                    "partition": part,
                })
            if n < max_per_page:
                break
            offset += n
        return out

    def write_point(self, point_id: int, ptype: int, value, source_ts_ns: int = 0) -> None:
        vbits = _encode_value(value, ptype)
        self._ok(OP_WRITE, WRITE_REQ.pack(point_id, ptype, vbits, source_ts_ns))

    def caps(self) -> dict:
        """OP_CAPS：取服务端容量并缓存升级本连接的上限。"""
        body = self._ok(OP_CAPS)
        if len(body) != CAPS.size:
            raise RtdbError(f"bad CAPS payload size {len(body)}")
        (proto, max_clients, initial_clients, max_subs,
         initial_subs, max_batch, flags, reserved) = CAPS.unpack(body)
        self.caps_info = {
            "proto_version": proto,
            "max_clients": max_clients,
            "initial_clients": initial_clients,
            "max_subs_per_conn": max_subs,
            "initial_subs_per_conn": initial_subs,
            "max_batch": max_batch,
            "flags": flags,
            "reserved": reserved,
        }
        self.max_subs = max_subs or RTDBD_LEGACY_SET_SUBS_MAX
        self.max_batch = max_batch or RTDBD_BATCH_MAX
        return self.caps_info

    def batch_read(self, point_ids: list[int]) -> list[dict]:
        """BATCH_READ：逐项返回 id/status/value；单项失败不抛异常。"""
        ids = [int(pid) for pid in point_ids]
        if not ids or len(ids) > self.max_batch:
            raise RtdbError(f"batch size must be in [1,{self.max_batch}]")
        payload = struct.pack("<I", len(ids)) + struct.pack(f"<{len(ids)}I", *ids)
        body = self._ok(OP_BATCH_READ, payload)
        item_size = BATCH_READ_ITEM.size + GET_RESP.size
        if len(body) != 4 + len(ids) * item_size:
            raise RtdbError("bad BATCH_READ response size")
        count = struct.unpack_from("<I", body, 0)[0]
        if count != len(ids):
            raise RtdbError("BATCH_READ count mismatch")
        out: list[dict] = []
        off = 4
        for _ in range(count):
            pid, status, _res = BATCH_READ_ITEM.unpack_from(body, off)
            off += BATCH_READ_ITEM.size
            rpid, ptype, quality, vbits, vstr, ts, sts = GET_RESP.unpack_from(body, off)
            off += GET_RESP.size
            item = {"id": pid, "status": status}
            if status == ST_OK and rpid == pid:
                item.update({
                    "type": ptype,
                    "typeName": TYPE_NAME.get(ptype, str(ptype)),
                    "quality": quality,
                    "value": _decode_value(vbits, vstr, ptype),
                    "ts": ts,
                    "sourceTs": sts,
                })
            out.append(item)
        return out

    def batch_write(self, writes: list[tuple]) -> list[dict]:
        """BATCH_WRITE：每项 (point_id, type, value[, source_ts_ns])，逐项返回状态。"""
        if not writes or len(writes) > self.max_batch:
            raise RtdbError(f"batch size must be in [1,{self.max_batch}]")
        chunks = []
        for item in writes:
            pid, ptype, value = int(item[0]), int(item[1]), item[2]
            source_ts = int(item[3]) if len(item) > 3 else 0
            chunks.append(WRITE_REQ.pack(pid, ptype, _encode_value(value, ptype), source_ts))
        payload = struct.pack("<I", len(writes)) + b"".join(chunks)
        body = self._ok(OP_BATCH_WRITE, payload)
        if len(body) != 4 + len(writes) * BATCH_WRITE_ITEM.size:
            raise RtdbError("bad BATCH_WRITE response size")
        count = struct.unpack_from("<I", body, 0)[0]
        if count != len(writes):
            raise RtdbError("BATCH_WRITE count mismatch")
        out: list[dict] = []
        off = 4
        for _ in range(count):
            pid, status, _res = BATCH_WRITE_ITEM.unpack_from(body, off)
            off += BATCH_WRITE_ITEM.size
            out.append({"id": pid, "status": status})
        return out

    def set_subs(self, point_ids: list[int]) -> None:
        """v3.10：连接级批量订阅（整组替换），响应为空负载。"""
        ids = [int(pid) for pid in point_ids][:self.max_subs]
        payload = struct.pack("<I", len(ids))
        if ids:
            payload += struct.pack(f"<{len(ids)}I", *ids)
        self._ok(OP_SET_SUBS, payload)

    def find_by_name(self, name: str, partition: int = 0) -> int:
        body = self._ok(OP_FIND_BY_NAME, FIND_REQ.pack(
            name.encode("utf-8")[:64].ljust(64, b"\x00"), int(partition)))
        return FIND_RESP.unpack(body)[0]

    def get_meta(self, point_id: int) -> dict:
        body = self._ok(OP_GET_META, META_REQ.pack(point_id))
        eur_min, eur_max, deadband, flags = META_PAYLOAD.unpack(body)
        return {"eurMin": eur_min, "eurMax": eur_max, "deadband": deadband, "flags": flags}

    # ---- v3.6 管控写通道：点位 CRUD（服务端须放行 uid 策略） ----
    def create_point(self, point_id: int, name: str, ptype: int, access: int = 3,
                     partition: int = 0) -> None:
        """注册点位。ptype 可为 int 或类型名（如 'int32'）。access 默认读写(3)。

        partition 为目标分区（(partition, name) 为唯一键），默认 0。
        """
        if isinstance(ptype, str):
            if ptype not in NAME_TYPE:
                raise RtdbError(f"unknown type name: {ptype}")
            ptype = NAME_TYPE[ptype]
        ptype = int(ptype)
        if not 0 <= ptype <= TYPE_FLOAT:
            raise RtdbError(f"type out of range: {ptype}")
        self._ok(OP_CREATE_POINT, CREATE_REQ.pack(
            int(point_id), ptype, int(access),
            name.encode("utf-8")[:64].ljust(64, b"\x00"), int(partition),
        ))

    def delete_point(self, point_id: int, partition: int = 0) -> None:
        """注销点位（清空 name 并移除 name→id 索引）。点位须属 partition，否则 NOT_FOUND。"""
        self._ok(OP_DELETE_POINT, DELETE_REQ.pack(int(point_id), int(partition)))

    def rename_point(self, point_id: int, name: str, partition: int = 0) -> None:
        """重命名点位（同步维护 name→id 索引）。点位须属 partition，否则 NOT_FOUND。"""
        self._ok(OP_RENAME_POINT, RENAME_REQ.pack(
            int(point_id), name.encode("utf-8")[:64].ljust(64, b"\x00"), int(partition),
        ))

    def get_logs(self, max_n: int = 0) -> list[dict]:
        """读取 rtdbd 运行日志（最旧→最新）。max_n=0 表示全部（服务端上限 128 条）。"""
        body = self._ok(OP_GET_LOG, LOG_REQ.pack(int(max_n)))
        out = []
        for i in range(len(body) // LOG_ENTRY.size):
            ts, level, msg = LOG_ENTRY.unpack(
                body[i * LOG_ENTRY.size:(i + 1) * LOG_ENTRY.size]
            )
            out.append({
                "ts": ts,
                "level": level,
                "levelName": LOG_LEVEL_NAME.get(level, str(level)),
                "msg": msg.rstrip(b"\x00").decode("utf-8", "replace"),
            })
        return out


class NotifyListener:
    """订阅变更通知的后台连接（独立 socket，避免与请求/响应互相阻塞）。

    收到 NOTIFY 帧后回调 on_notify(point_id, info_dict)。
    """

    def __init__(self, sock_path: str, on_notify: Callable[[int, dict], None],
                 timeout: float = 2.0,
                 on_snapshot_done: Optional[Callable[[dict], None]] = None):
        self.sock_path = sock_path
        self.on_notify = on_notify
        self.on_snapshot_done = on_snapshot_done
        self.timeout = timeout
        self.caps_info: Optional[dict] = None
        self.max_subs = RTDBD_LEGACY_SET_SUBS_MAX
        self.max_batch = RTDBD_BATCH_MAX
        self._caps_negotiated = False
        self._sock: Optional[socket.socket] = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._subscribed: set[int] = set()
        # subscribe() 可能同时被事件循环（WS 端点）与线程池（REST CRUD）调用；
        # header/body 必须作为一整帧原子发送，否则会交错破坏协议帧。
        self._send_lock = threading.Lock()

    def start(self, negotiate: bool = True) -> None:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(self.timeout)
        s.connect(self.sock_path)
        if negotiate:
            try:
                self._negotiate(s)
            except (RtdbError, OSError):
                # 旧服务端：CAPS 可能返回错误并断链；重连后按 legacy 模式工作。
                s.close()
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(self.timeout)
                s.connect(self.sock_path)
        # 监听线程可能被调度延迟；放大内核接收缓冲吸收通知突发。
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self._sock = s
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def _negotiate(self, sock: socket.socket) -> None:
        """在监听线程启动前同步完成 CAPS 握手，避免与推送帧竞争。"""
        sock.sendall(REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION, OP_CAPS, 0))
        hdr = _recv_exact_sock(sock, RESP_HDR.size)
        magic, version, status, plen = RESP_HDR.unpack(hdr)
        if magic != RTDBD_MAGIC or version != RTDBD_PROTO_VERSION or status != ST_OK:
            raise RtdbError("CAPS rejected")
        if plen != CAPS.size:
            raise RtdbError("CAPS payload mismatch")
        body = _recv_exact_sock(sock, plen)
        (proto, max_clients, initial_clients, max_subs,
         initial_subs, max_batch, flags, reserved) = CAPS.unpack(body)
        self.caps_info = {
            "proto_version": proto,
            "max_clients": max_clients,
            "initial_clients": initial_clients,
            "max_subs_per_conn": max_subs,
            "initial_subs_per_conn": initial_subs,
            "max_batch": max_batch,
            "flags": flags,
            "reserved": reserved,
        }
        self.max_subs = max_subs or RTDBD_LEGACY_SET_SUBS_MAX
        self.max_batch = max_batch or RTDBD_BATCH_MAX
        self._caps_negotiated = True

    def subscribe(self, point_ids: list[int]) -> None:
        sock = self._sock
        if not sock:
            raise RtdbError("listener not started")
        with self._send_lock:
            for pid in point_ids:
                pid = int(pid)
                # 去重：前端每次收到 list 都会订阅当前清单，CRUD 后服务端也会
                # 重新订阅；重复发送会耗尽 rtdbd 的连接订阅上限（RTDBD_SUB_MAX）。
                if pid in self._subscribed:
                    continue
                sock.sendall(
                    REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION, OP_SUBSCRIBE, 4)
                    + struct.pack("<I", pid)
                )
                self._subscribed.add(pid)

    def set_subs(self, point_ids: list[int]) -> None:
        """v3.10：整组替换订阅。响应由监听线程按普通 RESP_HDR 忽略。"""
        sock = self._sock
        if not sock:
            raise RtdbError("listener not started")
        ids = [int(pid) for pid in point_ids][:self.max_subs]
        payload = struct.pack("<I", len(ids))
        if ids:
            payload += struct.pack(f"<{len(ids)}I", *ids)
        with self._send_lock:
            sock.sendall(REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION,
                                      OP_SET_SUBS, len(payload)) + payload)
            self._subscribed = set(ids)

    def replace_subs_legacy(self, point_ids: list[int], limit: int = 32) -> list[int]:
        """旧服务端（无 SET_SUBS）整组替换：UNSUBSCRIBE 旧点后逐点订阅新点。"""
        sock = self._sock
        if not sock:
            raise RtdbError("listener not started")
        ids: list[int] = []
        seen: set[int] = set()
        for raw in point_ids:
            pid = int(raw)
            if pid not in seen:
                seen.add(pid)
                ids.append(pid)
            if len(ids) >= limit:
                break
        target = set(ids)
        with self._send_lock:
            for pid in sorted(self._subscribed - target):
                sock.sendall(
                    REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION, OP_UNSUBSCRIBE, 4)
                    + struct.pack("<I", pid)
                )
            self._subscribed = target
            for pid in ids:
                sock.sendall(
                    REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION, OP_SUBSCRIBE, 4)
                    + struct.pack("<I", pid)
                )
        return ids

    def _loop(self) -> None:
        assert self._sock is not None
        sock = self._sock
        buf = bytearray()
        while not self._stop.is_set():
            # 一次 recv 可能带回多帧；按帧长度切分，避免每帧两次 recv syscall。
            try:
                chunk = sock.recv(65536)
            except (socket.timeout, TimeoutError):
                # 空闲超时不是断链；帧内超时无法回退已读字节，只能断开。
                if buf:
                    break
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk

            # 订阅确认回的是 RESP_HDR，NOTIFY 推送回的是 REQ_HDR（opcode=OP_NOTIFY）；
            # 两者二进制布局相同（magic, version, kind, payload_len），故统一按 REQ_HDR 解。
            while len(buf) >= REQ_HDR.size:
                magic, version, kind, plen = REQ_HDR.unpack_from(buf, 0)
                if magic != RTDBD_MAGIC or version != RTDBD_PROTO_VERSION:
                    return
                if plen > 1_048_576:
                    return
                total = REQ_HDR.size + plen
                if len(buf) < total:
                    break
                body = bytes(buf[REQ_HDR.size:total])
                del buf[:total]
                if kind == OP_NOTIFY and plen == NOTIFY.size:
                    pid, ptype, quality, vbits, ts, sts = NOTIFY.unpack(body)
                    self.on_notify(pid, {
                        "id": pid, "type": ptype,
                        "typeName": TYPE_NAME.get(ptype, str(ptype)),
                        "quality": quality,
                        "value": _decode_value(vbits, b"", ptype),
                        "ts": ts, "sourceTs": sts,
                        # reserved[1] 的快照标记（v3.11）
                        "snapshot": bool(body[7] & NOTIFY_F_SNAPSHOT),
                    })
                elif kind == OP_SNAPSHOT_DONE and plen == SNAPSHOT_DONE.size:
                    count, flags = SNAPSHOT_DONE.unpack(body)
                    if self.on_snapshot_done:
                        self.on_snapshot_done({"count": count, "flags": flags})
                # 其余（订阅确认等响应）忽略

    def stop(self) -> None:
        self._stop.set()
        if self._sock:
            try:
                self._sock.close()
            finally:
                self._sock = None


def _recv_exact_sock(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RtdbError("connection closed")
        buf += chunk
    return bytes(buf)


def probe_caps(client) -> Optional[dict]:
    """探测 v3.11 CAPS；旧服务端返回 None。"""
    try:
        return client.caps()
    except (RtdbError, OSError, AttributeError):
        return None


def probe_set_subs(client) -> bool:
    """探测服务端是否支持 SET_SUBS；旧服务端返回错误/断链时返回 False。"""
    try:
        client.set_subs([])
        return True
    except (RtdbError, OSError):
        return False


if __name__ == "__main__":
    import sys
    path = sys.argv[1] if len(sys.argv) > 1 else "/run/indurtdb/default.sock"
    with RtdbClient(path) as c:
        print("ping:", c.ping())
        pts = c.list_points()
        print(f"points: {len(pts)}")
        for p in pts[:10]:
            print("  ", p)
