"""泄漏检测：驱动 harness 反复启停，检测 fd 是否增长。"""
from __future__ import annotations

import re
import subprocess
import sys


def check_fd_leak(harness: str, instance_id: str,
                  cycles: int = 20) -> "tuple[int, int, str]":
    cmd = [harness, "--leak", "--id", instance_id, "--cycles", str(cycles)]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except FileNotFoundError:
        return 127, 0, f"harness 未找到: {harness}"
    out = (p.stdout or "") + (p.stderr or "")
    m0 = re.search(r"LEAK_INIT_FD\s+(\d+)", out)
    m1 = re.search(r"LEAK_FINAL_FD\s+(\d+)", out)
    init_fd = int(m0.group(1)) if m0 else -1
    final_fd = int(m1.group(1)) if m1 else -1
    delta = final_fd - init_fd if (m0 and m1) else -1
    return p.returncode, delta, out


def main_leak(args) -> int:
    rc, delta, out = check_fd_leak(args.harness, args.id, args.cycles)
    sys.stdout.write(out)
    if rc != 0:
        return rc
    if delta < 0:
        return 2
    if delta > 0:
        print(f"[leak] 检测到 fd 增长: {delta}", file=sys.stderr)
        return 3
    print("[leak] 未发现 fd 泄漏")
    return 0
