#!/usr/bin/env python3
"""VSOA SDK 探测门禁（M1.3a / v3.12 G2 平台化）：缺项即失败，不得静默降级。

依据 doc 13 §95：构建加 -DINDURTDB_ENABLE_VSOA=ON + VSOA_SDK_ROOT，
干净树默认 OFF 仍可构建。开启时 SDK 头/库缺失或版本不符必须配置期硬失败，
否则退化成「以为接了 VSOA、实际没接」的假绿。

v3.12 G2 变更：
  - --lib-subdir / --nm 参数化，支持 SylixOS 交叉库(lib/sylixos/aarch64)+交叉 nm；
    Linux 默认值不变(lib/linux/x86_64 + 宿主 nm)，零回归。
  - pool_config 检测由子串匹配改为**精确符号匹配**：SylixOS 库导出
    vsoa_mem_pool_config/vsoa_server_pool_config/vsoa_client_pool_config，
    子串匹配会误判为禁用符号而 FATAL（G2 评审 P0-3）。
  - 符号集扩展为 adapter + e2e harness 实际调用全集（server 11 + client 8），
    两平台库均导出；REQUIRED_LIBS 补 parser/json。
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REQUIRED_HEADERS = ("vsoa_server.h", "vsoa_client.h", "vsoa_version.h")
REQUIRED_LIBS = ("libvsoa-server.so", "libvsoa-client.so",
                 "libvsoa-parser.so", "libvsoa-json.so")
DEFAULT_LIB_SUBDIR = "lib/linux/x86_64"
# adapter + e2e harness 实际调用全集（G2 评审 P1-1）
REQUIRED_SYMBOLS = (
    # server（rtdbd/src/vsoa/vsoa_adapter.c）
    "vsoa_server_create", "vsoa_server_on_cli", "vsoa_server_add_listener",
    "vsoa_server_close", "vsoa_server_start", "vsoa_server_cli_address",
    "vsoa_server_cli_datagram", "vsoa_server_cli_reply",
    "vsoa_server_cli_set_authed", "vsoa_server_fds", "vsoa_server_input_fds",
    # h-C 设备侧代理（对外 passwd 连接级准入，d-C）
    "vsoa_server_passwd",
    # client（e2e harness：写入方 + 订阅者）
    "vsoa_client_create", "vsoa_client_connect", "vsoa_client_call",
    "vsoa_client_on_datagram", "vsoa_client_subscribe",
    "vsoa_client_fds", "vsoa_client_input_fds", "vsoa_client_close",
)
POOL_CONFIG_SYMBOL = "pool_config"


def probe(root, lib_subdir, nm, expect_version, check_symbols):
    findings = []
    inc = root / "include" / "libvsoa"
    libdir = root / lib_subdir
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
                out = subprocess.run([nm, "-D", str(so)], capture_output=True,
                                     text=True, timeout=60).stdout
            except (OSError, subprocess.SubprocessError) as exc:
                findings.append("%s -D 执行失败: %s (%s)" % (nm, so, exc))
                continue
            for sym in REQUIRED_SYMBOLS:
                if re.search(r"\s[TW]\s+%s$" % re.escape(sym), out, re.M):
                    symbols[sym] = True
            # G2 评审 P0-3：精确符号匹配，避免 vsoa_*_pool_config 子串误判
            if re.search(r"\s[TW]\s+%s$" % re.escape(POOL_CONFIG_SYMBOL), out, re.M):
                pool_config_exported = True
        for sym in REQUIRED_SYMBOLS:
            if not symbols.get(sym):
                findings.append("动态库未导出符号: %s" % sym)
        if pool_config_exported:
            findings.append("动态库导出了 pool_config：v3.12 口径仍为不得依赖")
    return {"ok": not findings, "root": str(root), "lib_subdir": lib_subdir,
            "version": version, "expected_version": expect_version,
            "symbols_checked": check_symbols, "symbols": symbols,
            "symbols_required": list(REQUIRED_SYMBOLS),
            "pool_config_exported": pool_config_exported, "findings": findings}


def main():
    ap = argparse.ArgumentParser(description="VSOA SDK 探测门禁")
    ap.add_argument("--root", required=True, help="VSOA_SDK_ROOT（libvsoa 树根）")
    ap.add_argument("--expect-version", default="1.9.1", help="期望的 VSOA_LIB_VERSION")
    ap.add_argument("--lib-subdir", default=DEFAULT_LIB_SUBDIR,
                    help="库相对路径（默认 lib/linux/x86_64；SylixOS 传 lib/sylixos/aarch64）")
    ap.add_argument("--nm", default="nm", help="nm 工具（SylixOS 传交叉 nm，如 ${CMAKE_NM}）")
    ap.add_argument("--check-symbols", action="store_true", help="追加 nm -D 符号级探测")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    args = ap.parse_args()
    result = probe(Path(args.root), args.lib_subdir, args.nm,
                   args.expect_version, args.check_symbols)
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
