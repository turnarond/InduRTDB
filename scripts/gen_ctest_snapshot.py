#!/usr/bin/env python3
"""h-C 生产发布阶段一：生成 OFF+ON ctest -N 并集快照（doc19 §5.5）。

现场对两个构建目录执行 `ctest -N`，解析真实注册名、结构化校验后合并去重，
输出带元数据（来源 build 目录 + 时间戳）的快照文件，供 release_gate.py 现场复核。
绝不手工拼快照——手工快照会被 release_gate.py 的结构化校验与现场比对拒绝。

用法：
  python3 scripts/gen_ctest_snapshot.py --builds build build-vsoa --out snapshot.txt
  python3 scripts/gen_ctest_snapshot.py --self-test
"""
import argparse
import datetime
import re
import subprocess
import sys
import tempfile
from pathlib import Path

_CTEST_LINE_RE = re.compile(r"^\s*Test\s+#\d+:\s+(\S+)\s*$")
_CTEST_TOTAL_RE = re.compile(r"^Total Tests:\s*(\d+)\s*$", re.M)
_CTEST_PROJECT_RE = re.compile(r"^Test project\s+(.+?)\s*$", re.M)


def parse_ctest(text):
    names, n_lines = [], 0
    for line in (text or "").splitlines():
        m = _CTEST_LINE_RE.match(line)
        if m:
            names.append(m.group(1))
            n_lines += 1
    tm = _CTEST_TOTAL_RE.search(text)
    if not tm or int(tm.group(1)) != n_lines:
        raise ValueError(f"Total Tests mismatch: total={tm.group(1) if tm else 'missing'} lines={n_lines}")
    if not _CTEST_PROJECT_RE.search(text):
        raise ValueError("missing 'Test project' header")
    return names


def live_ctest(build_dir):
    bd = Path(build_dir)
    if not bd.is_dir():
        raise ValueError(f"build dir missing: {bd}")
    out = subprocess.run(["ctest", "-N"], cwd=str(bd), capture_output=True,
                         text=True, timeout=60, check=False)
    if out.returncode != 0:
        raise ValueError(f"ctest -N exit {out.returncode}: {out.stderr[:200]}")
    return parse_ctest(out.stdout), out.stdout


def union_snapshot(builds):
    all_names = set()
    raws = []
    for b in builds:
        names, raw = live_ctest(b)
        all_names |= set(names)
        raws.append((b, raw))
    lines = ["Test project /live-union"]
    for i, name in enumerate(sorted(all_names), 1):
        lines.append(f"  Test  #{i}: {name}")
    lines.append(f"Total Tests: {len(all_names)}")
    lines.append("# IR_BUILD_DIRS: " + " ".join(builds))
    lines.append("# GENERATED_AT: " + datetime.date.today().isoformat())
    return "\n".join(lines) + "\n", all_names


def self_test():
    """验证并集逻辑与结构化校验；用注入的 fake ctest 输出代替真实 ctest（不碰现场）。"""
    global live_ctest
    saved = live_ctest
    outputs = {
        "off": "Test project /fake-off\n  Test  #1: IR1\n  Test  #2: IR2\nTotal Tests: 2\n",
        "on": "Test project /fake-on\n  Test  #1: IR2\n  Test  #2: IR3_ONONLY\nTotal Tests: 2\n",
    }
    def fake_live(build_dir):
        key = Path(build_dir).name
        return parse_ctest(outputs[key]), outputs[key]
    live_ctest = fake_live
    try:
        text, names = union_snapshot(["off", "on"])
    finally:
        live_ctest = saved
    ok = names == {"IR1", "IR2", "IR3_ONONLY"} and "Total Tests: 3" in text
    print(("PASS" if ok else "FAIL") + ": gen_ctest_snapshot self-test")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--builds", nargs="+", help="build dirs (OFF and ON)")
    ap.add_argument("--out", help="output snapshot file")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.builds or not args.out:
        ap.error("--builds and --out required (or --self-test)")
    text, names = union_snapshot(args.builds)
    Path(args.out).write_text(text, encoding="utf-8")
    print(f"generated {args.out}: {len(names)} tests from {len(args.builds)} builds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
