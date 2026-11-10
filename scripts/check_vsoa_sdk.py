#!/usr/bin/env python3
"""VSOA SDK 探测门禁（M1.3a）：缺项即失败，不得静默降级。

依据 doc 13 §95：构建加 -DINDURTDB_ENABLE_VSOA=ON + VSOA_SDK_ROOT，
干净树默认 OFF 仍可构建。开启时 SDK 头/库缺失或版本不符必须配置期硬失败，
否则退化成「以为接了 VSOA、实际没接」的假绿。
文件级探测可被假 SDK 根样本覆盖；符号级探测（--check-symbols）需真实 ELF。
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REQUIRED_HEADERS = ("vsoa_server.h", "vsoa_client.h", "vsoa_version.h")
REQUIRED_LIBS = ("libvsoa-server.so", "libvsoa-client.so")
LIB_SUBDIR = Path("lib") / "linux" / "x86_64"
REQUIRED_SYMBOLS = ("vsoa_server_start", "vsoa_client_connect",
                    "vsoa_client_call", "vsoa_server_cli_datagram")
POOL_CONFIG_SYMBOL = "pool_config"


def probe(root, expect_version, check_symbols):
    findings = []
    inc = root / "include" / "libvsoa"
    libdir = root / LIB_SUBDIR
    if not root.is_dir():
        findings.append("SDK 根不存在: %s" % root)
    for h in REQUIRED_HEADERS:
        if not (inc / h).is_file():
            findings.append("缺少头文件: %s" % (inc / h))
    for l in REQUIRED_LIBS:
        if not (libdir / l).is_file():
            findings.append("缺少动态库: %s" % (libdir / l))
    version = None
    vfile = inc / "vsoa_version.h"
    if vfile.is_file():
        text = vfile.read_text(encoding="utf-8", errors="ignore")
        m = re.search(r'#define\s+VSOA_LIB_VERSION\s+"([^"]+)"', text)
        if m:
            version = m.group(1)
        else:
            findings.append("vsoa_version.h 未定义 VSOA_LIB_VERSION: %s" % vfile)
        if version and expect_version and version != expect_version:
            findings.append("版本不符: 实际 %s，期望 %s" % (version, expect_version))
    symbols = {}
    pool_config_exported = False
    if check_symbols:
        for lib in REQUIRED_LIBS:
            so = libdir / lib
            if not so.is_file():
                continue
            try:
                out = subprocess.run(["nm", "-D", str(so)], capture_output=True,
                                     text=True, timeout=60).stdout
            except (OSError, subprocess.SubprocessError):
                findings.append("nm -D 执行失败: %s" % so)
                continue
            for sym in REQUIRED_SYMBOLS:
                if re.search(r"\s[TW]\s+%s$" % re.escape(sym), out, re.M):
                    symbols[sym] = True
            if POOL_CONFIG_SYMBOL in out:
                pool_config_exported = True
        for sym in REQUIRED_SYMBOLS:
            if not symbols.get(sym):
                findings.append("动态库未导出符号: %s" % sym)
        if pool_config_exported:
            findings.append("动态库导出了 pool_config：v3.12 口径仍为不得依赖")
    return {"ok": not findings, "root": str(root), "version": version,
            "expected_version": expect_version, "symbols_checked": check_symbols,
            "symbols": symbols, "pool_config_exported": pool_config_exported,
            "findings": findings}


def main():
    ap = argparse.ArgumentParser(description="VSOA SDK 探测门禁")
    ap.add_argument("--root", required=True, help="VSOA_SDK_ROOT（libvsoa 树根）")
    ap.add_argument("--expect-version", default="1.9.1", help="期望的 VSOA_LIB_VERSION")
    ap.add_argument("--check-symbols", action="store_true", help="追加 nm -D 符号级探测")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    args = ap.parse_args()
    result = probe(Path(args.root), args.expect_version, args.check_symbols)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    elif result["ok"]:
        print("OK: VSOA SDK 可用（版本 %s）" % result["version"])
    else:
        print("FAIL: VSOA SDK 不可用", file=sys.stderr)
        for f in result["findings"]:
            print("  - %s" % f, file=sys.stderr)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
