#!/usr/bin/env python3
"""rtdb_client 协议编解码单测（不依赖 rtdbd 进程，纯结构校验）。"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from rtdb_client import (  # noqa: E402
    REQ_HDR, RESP_HDR, WRITE_REQ, GET_REQ, GET_RESP, LIST_REQ, POINT_INFO,
    NOTIFY, FIND_REQ, FIND_RESP, META_REQ, META_PAYLOAD,
    _decode_value, _encode_value, TYPE_INT32, TYPE_INT64, TYPE_UINT32,
    TYPE_FLOAT, TYPE_DOUBLE, TYPE_BOOL,
)


def test_struct_sizes_match_c_layout():
    # 与 rtdbd/include/rtdbd/protocol.h 的 static_assert 保持一致
    assert REQ_HDR.size == 12
    assert RESP_HDR.size == 12
    assert WRITE_REQ.size == 24
    assert GET_REQ.size == 4
    assert GET_RESP.size == 64
    assert LIST_REQ.size == 8
    assert POINT_INFO.size == 72
    assert NOTIFY.size == 32
    assert FIND_REQ.size == 64
    assert FIND_RESP.size == 4
    assert META_REQ.size == 4
    assert META_PAYLOAD.size == 32


def test_req_resp_hdr_layout():
    b = REQ_HDR.pack(0x52424431, 2, 10, 4)
    assert len(b) == 12
    magic, version, opcode, plen = REQ_HDR.unpack(b)
    assert magic == 0x52424431 and version == 2 and opcode == 10 and plen == 4


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
    b = POINT_INFO.pack(11, TYPE_INT32, 3, b"Pump.Start\x00\x00\x00\x00\x00\x00\x00\x00")
    assert len(b) == 72
    pid, ptype, access, name = POINT_INFO.unpack(b)
    assert pid == 11 and access == 3 and name.rstrip(b"\x00") == b"Pump.Start"


if __name__ == "__main__":
    test_struct_sizes_match_c_layout()
    test_req_resp_hdr_layout()
    test_value_roundtrip_numeric()
    test_get_resp_roundtrip()
    test_point_info_roundtrip()
    print("ALL OK")
