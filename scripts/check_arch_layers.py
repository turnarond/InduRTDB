#!/usr/bin/env python3
"""架构分层方向守卫（机制 C：抗架构腐朽，把「单向依赖」变成可验证判据）。

InduRTDB 的分层（由外向内，箭头为允许的依赖方向）::

    公共契约 include/            （只允许依赖自身）
        ↑
    外观层   src/api             → core / internal / 公共契约
    服务层   rtdbd/ client/      → 公共契约（不得摸 core 内部）
        ↑
    核心层   src/core            → internal / osal / 公共契约
        ↑
    共享布局 src/internal        → 公共契约（ABI 结构体定义）
        ↑
    平台层   src/osal            → 仅系统头文件

判据：源文件 ``#include`` 到「比自己更内层以外」的层 = 反向依赖 = FAIL；
同层/向内层引用合法。

用法::

    python3 scripts/check_arch_layers.py              # 默认严格：有违规退 1
    python3 scripts/check_arch_layers.py --warn-only  # 只报告不退非 0（校准期用）
    python3 scripts/check_arch_layers.py --json
    python3 scripts/check_arch_layers.py --self-test  # 判据自证（抗假绿）
"""
import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
SOURCE_EXT = (".c", ".h")

# ring 名 -> 目录前缀（相对仓库根）；顺序无关，按最长前缀命中。
RINGS = {
    "public": ["include/"],
    "api": ["src/api/"],
    "server": ["rtdbd/", "client/"],
    "core": ["src/core/"],
    "internal": ["src/internal/"],
    "osal": ["src/osal/"],
}

# 头文件 include 前缀 -> ring（构建系统把各目录加入 include path，故用前缀判层）。
HEADER_PREFIX_RING = {
    "indurtdb/": "public",
    "core/": "core",
    "internal/": "internal",
    "osal/": "osal",
    "rtdbd/": "server",
    "irtcli/": "server",
    "api/": "api",
}

# 合法依赖（from ring -> 允许的 to ring 集合）。未列出的边一律违规。
ALLOWED = {
    "public": set(),
    "api": {"core", "internal", "public"},
    "server": {"public"},
    "core": {"internal", "osal", "public"},
    "internal": {"public"},
    "osal": set(),
}

SKIP_DIR_PARTS = {"build", "node_modules", "__pycache__", ".git", "tools", "tests", "examples", "demo", "templates"}


def ring_of_path(rel):
    best, best_len = None, -1
    for ring, prefixes in RINGS.items():
        for prefix in prefixes:
            if rel.startswith(prefix) and len(prefix) > best_len:
                best, best_len = ring, len(prefix)
    return best


def ring_of_include(inc, from_ring):
    inc = inc.strip()
    for prefix, ring in HEADER_PREFIX_RING.items():
        if inc.startswith(prefix):
            return ring
    # 裸文件名：视为同目录（同 ring）引用，不参与跨层判定。
    if "/" not in inc:
        return from_ring
    # 其他带路径的（如 "core/xxx.h" 已覆盖）：无法归类则忽略，避免误判。
    return None


def scan(root):
    root = Path(root)
    violations = []
    scanned = 0
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_EXT:
            continue
        rel = path.relative_to(root).as_posix()
        if any(part in SKIP_DIR_PARTS for part in rel.split("/")):
            continue
        from_ring = ring_of_path(rel)
        if not from_ring:
            continue
        scanned += 1
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            to_ring = ring_of_include(m.group(1), from_ring)
            if not to_ring or to_ring == from_ring:
                continue
            if to_ring not in ALLOWED.get(from_ring, set()):
                violations.append({
                    "level": "FAIL",
                    "file": rel,
                    "line": lineno,
                    "include": m.group(1),
                    "from": from_ring,
                    "to": to_ring,
                    "message": f"{from_ring} 不得依赖 {to_ring}",
                })
    return {"ok": not violations, "scanned": scanned, "violations": violations}


def self_test():
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "src/core").mkdir(parents=True)
        (root / "src/osal").mkdir(parents=True)
        (root / "rtdbd").mkdir(parents=True)
        (root / "include/indurtdb").mkdir(parents=True)

        (root / "src/core/good.c").write_text(
            '#include "internal/irt_types.h"\n#include <osal/irt_osal.h>\n'
            '#include <indurtdb/indurtdb.h>\n#include "core/irt_shm.h"\n', encoding="utf-8")
        (root / "src/osal/good.c").write_text('#include <string.h>\n', encoding="utf-8")
        (root / "rtdbd/good.c").write_text('#include <indurtdb/indurtdb.h>\n', encoding="utf-8")

        ok_result = scan(root)

        (root / "src/osal/bad.c").write_text('#include "core/irt_shm.h"\n', encoding="utf-8")
        (root / "rtdbd/bad.c").write_text('#include "core/irt_config.h"\n', encoding="utf-8")
        (root / "include/indurtdb/bad.h").write_text('#include "core/irt_shm.h"\n', encoding="utf-8")
        bad_result = scan(root)

    cases = [
        ("合法依赖应 PASS", ok_result["ok"] is True),
        ("osal→core 应被抓出", bad_result["ok"] is False),
        ("违规边数应为 3", len(bad_result["violations"]) == 3),
    ]
    for name, passed in cases:
        print(f"  [{'OK' if passed else 'FAIL'}] {name}")
    if all(ok for _, ok in cases):
        print(f"OK: 自证通过 {len(cases)} 项")
        return 0
    print("FAIL: 自证未通过（判据表本身被改坏？）", file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--warn-only", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    result = scan(args.root)
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        for v in result["violations"]:
            print(f"  [FAIL] {v['file']}:{v['line']} include {v['include']} —— {v['message']}")
        if result["ok"]:
            print(f"OK: 分层方向合规（扫描 {result['scanned']} 个源文件）")
        elif args.warn_only:
            print(f"WARN: {len(result['violations'])} 处反向依赖（--warn-only 不阻断）")
        else:
            print(f"FAIL: {len(result['violations'])} 处反向依赖")
    if args.warn_only or result["ok"]:
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
