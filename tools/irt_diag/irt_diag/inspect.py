"""在线段巡检：读取 /dev/shm 段 Header，输出健康状态。"""
from __future__ import annotations

import json
import os
import sys

from ._layout import HEADER_V2_SIZE, SHM_VERSION_V2, parse_v2_header

SHM_DIR = "/dev/shm"


def shm_path(instance_id: str) -> str:
    return os.path.join(SHM_DIR, f"indurtdb_{instance_id}")


def inspect_segment(instance_id: str) -> dict:
    """打开段、解析 Header、补充段大小与健康判定。"""
    path = shm_path(instance_id)
    if not os.path.exists(path):
        raise FileNotFoundError(f"段不存在: {path}")
    with open(path, "rb") as f:
        raw = f.read(HEADER_V2_SIZE if HEADER_V2_SIZE else 128)
    hdr = parse_v2_header(raw)
    st = os.fstat(os.open(path, os.O_RDONLY))
    hdr["instance_id"] = instance_id
    hdr["segment_size"] = st.st_size
    hdr["healthy"] = (
        hdr["magic_ok"]
        and hdr["version"] == SHM_VERSION_V2
        and hdr["crc_ok"]
        and hdr["owner_pid"] > 0
    )
    return hdr


def main_inspect(args) -> int:
    try:
        hdr = inspect_segment(args.id)
    except (FileNotFoundError, ValueError) as e:
        print(json.dumps({"error": str(e)}, ensure_ascii=False))
        return 2
    print(json.dumps(hdr, ensure_ascii=False, indent=2, sort_keys=True))
    return 0 if hdr["healthy"] else 1
