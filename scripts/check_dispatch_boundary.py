#!/usr/bin/env python3
"""dispatch 层传输无关性守卫（符号级约束，机制 D）。

背景：v3.12 §8.3-3 要求抽出 ``rtdbd_dispatch()``，使业务语义与传输解耦。
`check_arch_layers.py` 只检查 include 的**分层边**，抓不到「dispatch 层源码里
直接写 fd / socket / VSOA 符号」这类符号级泄漏。本守卫补上这一层：

* 扫描 dispatch 层源码（默认 ``rtdbd/src/*dispatch*``、``src/api/*dispatch*``）；
* 命中传输相关符号（fd/socket/poll/recv/send/vsoa_*/cli_reply/...）即 FAIL；
* 空扫描（实现前）返回 PASS 并显式报告 0 文件，规则生效性由 ``--self-test`` 与
  ``gate_canary.py`` 的坏样本自证，避免「恒绿 = 没门禁」。

用法::

    python3 scripts/check_dispatch_boundary.py
    python3 scripts/check_dispatch_boundary.py --json
    python3 scripts/check_dispatch_boundary.py --self-test
    python3 scripts/check_dispatch_boundary.py --root <repo>

退出码：0 = 合规（含空扫描）；1 = 发现符号泄漏；2 = 用法/读取错误。
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 相对仓库根的 dispatch 层目录/文件名模式。实现期新增的 dispatch 模块须落在这里。
DISPATCH_GLOBS = (
    "rtdbd/src/*dispatch*",
    "rtdbd/src/dispatch/*",
    "rtdbd/include/*dispatch*",
    "src/api/*dispatch*",
)

# 传输层符号：dispatch 层不得直接出现。按「词边界/前缀」匹配，避免误伤业务命名。
FORBIDDEN = (
    (re.compile(r"\bvsoa_[A-Za-z0-9_]+"), "VSOA SDK 符号"),
    (re.compile(r"\bVSOA_[A-Z0-9_]+"), "VSOA 宏/常量"),
    (re.compile(r"\b(?:vsoa_server_t|vsoa_client_t|vsoa_cli_id_t|vsoa_payload_t)\b"), "VSOA 类型"),
    (re.compile(r"\b(?:cli_reply|cli_datagram|cli_quick_datagram|cli_close|cli_address)\b"), "VSOA 定向 API"),
    (re.compile(r"\b(?:socket|sockaddr|socklen_t|send|recv|sendto|recvfrom|accept|connect|bind|listen)\s*\("), "socket 调用"),
    (re.compile(r"\b(?:fd_set|FD_SET|FD_ZERO|FD_ISSET|pollfd|poll|pselect|select|epoll_wait|epoll_create1)\s*[\(;\s]"), "I/O 复用/fd_set"),
    (re.compile(r"\b(?:rtdbd_conn_t|outq_push|conn_snapshot_pump|SO_PEERCRED)\b"), "UDS 连接级状态"),
    (re.compile(r"(?<![A-Za-z0-9_])fd(?![A-Za-z0-9_])"), "裸 fd 标识"),
)

# 允许出现的行：注释里解释约束时可能提到这些词，避免把文档性注释误判为实现泄漏。
COMMENT_RE = re.compile(r"^\s*(?://|/\*|\*|#)")


def _collect(root):
    paths = set()
    for pattern in DISPATCH_GLOBS:
        for p in sorted(Path(root).glob(pattern)):
            if p.is_file() and p.suffix in (".c", ".h", ".cc", ".cpp"):
                paths.add(p)
    return sorted(paths)


def scan(root):
    violations = []
    files = _collect(root)
    for path in files:
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            violations.append({"level": "FAIL", "file": str(path), "line": 0,
                               "symbol": "", "message": f"读取失败: {exc}"})
            continue
        for lineno, line in enumerate(lines, 1):
            if COMMENT_RE.match(line):
                continue
            for regex, label in FORBIDDEN:
                m = regex.search(line)
                if m:
                    violations.append({
                        "level": "FAIL", "file": path.as_posix(), "line": lineno,
                        "symbol": m.group(0).strip(), "message": f"dispatch 层不得出现 {label}",
                    })
                    break
    return {"ok": not violations, "scanned": len(files),
            "files": [p.as_posix() for p in files], "violations": violations}


def self_test():
    import tempfile
    cases = []
    with tempfile.TemporaryDirectory() as td:
        root = Path(td) / "rtdbd/src/dispatch"
        root.mkdir(parents=True)

        good = root / "write.c"
        good.write_text(
            "/* 只允许纯业务语义 */\n"
            "#include <internal/irt_types.h>\n"
            "int dispatch_apply(irt_point_t *pt, const sync_rec_t *rec) {\n"
            "    return irt_pm_write(pt, rec->value);\n}\n", encoding="utf-8")
        cases.append(("纯业务 dispatch 应 PASS", scan(td)["ok"] is True))

        bad = root / "bad.c"
        bad.write_text("#include <vsoa_server.h>\n"
                       "static int dispatch_write(int fd, vsoa_server_t *srv) {\n"
                       "    return vsoa_server_cli_reply(srv, fd, 0, 0, 0, NULL);\n}\n",
                       encoding="utf-8")
        res = scan(td)
        cases.append(("VSOA/fd 符号应被抓出", res["ok"] is False))
        cases.append(("应报告具体符号与行号",
                      any("VSOA" in v["message"] or "fd" in v["symbol"] for v in res["violations"])))
        bad.unlink()

        # 空扫描不得伪装成「规则已验证」：由本用例与 canary 共同保证。
        good.unlink()
        empty = scan(td)
        cases.append(("空扫描应 PASS 且 scanned=0", empty["ok"] is True and empty["scanned"] == 0))

        # 注释中的解释性文字不得误伤。
        root2 = Path(td) / "rtdbd/src/dispatch2.c"
        root2.write_text("/* dispatch 层禁止 vsoa_ 符号与裸 fd */\n"
                         "int dispatch_ok(void) { return 0; }\n", encoding="utf-8")
        cases.append(("注释提及不误伤", scan(td)["ok"] is True))

    for name, ok in cases:
        print(f"  [{'OK' if ok else 'FAIL'}] {name}")
    if all(ok for _, ok in cases):
        print(f"OK: 自证通过 {len(cases)} 项")
        return 0
    print("FAIL: 自证未通过（符号表被改坏？）", file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser(description="dispatch layer transport-independence guard")
    ap.add_argument("--root", default=str(ROOT))
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
            print(f"  [FAIL] {v['file']}:{v['line']} `{v['symbol']}` —— {v['message']}")
        if result["ok"]:
            note = "（空扫描：规则生效性由 --self-test 与 gate_canary 自证）" if result["scanned"] == 0 else ""
            print(f"OK: dispatch 层传输无关（扫描 {result['scanned']} 个文件）{note}")
        else:
            print(f"FAIL: {len(result['violations'])} 处传输符号泄漏")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
