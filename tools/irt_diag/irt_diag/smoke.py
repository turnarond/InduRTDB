"""冒烟：驱动 C harness 完成「起服务 → 写 → 读 → 订阅 → 停」全流程。"""
from __future__ import annotations

import subprocess
import sys


def run_smoke(harness: str, instance_id: str,
              points: int = 64, subs: int = 4) -> "tuple[int, str]":
    cmd = [harness, "--smoke", "--id", instance_id,
           "--points", str(points), "--subs", str(subs)]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except FileNotFoundError:
        return 127, f"harness 未找到: {harness}"
    out = (p.stdout or "") + (p.stderr or "")
    return p.returncode, out


def main_smoke(args) -> int:
    rc, out = run_smoke(args.harness, args.id, args.points, args.subs)
    sys.stdout.write(out)
    return rc
