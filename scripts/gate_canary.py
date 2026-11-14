#!/usr/bin/env python3
"""门禁金丝雀（机制 B：抗假绿）——证明本仓守卫「真的会红」。

做法：在临时目录里造**已知坏样本**，调用各守卫，断言其退出码非 0；
若坏样本仍返回 0，说明守卫失活，本脚本 FAIL。

覆盖：版本号守卫、架构分层守卫、API 计数守卫、仓库洁净度守卫、
发布门「有条件通过」条件闭环守卫、dispatch 符号边界守卫。

用法::

    python3 scripts/gate_canary.py
    python3 scripts/gate_canary.py --json

退出码：0 = 全部守卫可证伪；1 = 有守卫对坏样本仍绿（假绿）。
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def run(cmd, cwd=None):
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


# 守卫「脚本不存在 / 崩溃」也会返回非 0，若不排除就会被误判为「坏样本被抓出」——
# 这是最隐蔽的假绿。崩溃特征黑名单（第三签 H-2：注释不得宣称「任何」，
# 黑名单之外还可能漏，故配合下方「非空输出 + 无崩溃标记」的双条件）。
CRASH_MARKERS = (
    "can't open file", "No such file or directory", "Traceback (most recent call last)",
    "ModuleNotFoundError", "ImportError", "SyntaxError", "FileNotFoundError",
    "Segmentation fault", "core dumped", "Killed", "Aborted", "Bus error",
    "Permission denied", "command not found",
)


def guard_alive(out):
    """守卫存活 = 有输出、无已知崩溃标记。

    仅靠黑名单无法穷尽所有崩溃（第三签实测：自定义 exit 2 且无标记的输出仍会
    被判存活），因此本函数只负责「已知崩溃 + 空输出」这两类，不宣称完备。
    完整可证伪性还依赖各 case 自己的坏样本确实命中判据，见 CASES 与 self-test。
    """
    text = out or ""
    if not text.strip():
        return False
    return not any(marker in text for marker in CRASH_MARKERS)


def case_version(tmp):
    root = tmp / "version"
    (root / "include/indurtdb").mkdir(parents=True)
    (root / "VERSION").write_text("9.9.9\n", encoding="utf-8")
    (root / "CMakeLists.txt").write_text("project(X VERSION 9.9.9 LANGUAGES C)\n", encoding="utf-8")
    (root / "include/indurtdb/indurtdb.h").write_text(
        "#define INDURTDB_VERSION_MAJOR 9\n#define INDURTDB_VERSION_MINOR 9\n"
        "#define INDURTDB_VERSION_PATCH 9\n"
        '#define INDURTDB_VERSION_STRING "9.9.9"\n', encoding="utf-8")
    (root / "CHANGELOG.md").write_text("## [9.9.8]\n", encoding="utf-8")
    code, out = run([sys.executable, str(HERE / "check_version_sync.py"), "--root", str(root)])
    return code != 0, "CHANGELOG 缺条目", out


def case_arch(tmp):
    root = tmp / "arch"
    (root / "src/osal").mkdir(parents=True)
    (root / "src/core").mkdir(parents=True)
    (root / "src/osal/leak.c").write_text('#include "core/irt_shm.h"\n', encoding="utf-8")
    code, out = run([sys.executable, str(HERE / "check_arch_layers.py"), "--root", str(root)])
    return code != 0, "osal 反向依赖 core", out


def case_arch_vsoa(tmp):
    """坏样本：核心层引入 libvsoa 头 —— VSOA 只链到 rtdbd 必须被分层守卫抓出。"""
    root = tmp / "arch_vsoa"
    (root / "src/core").mkdir(parents=True)
    (root / "src/core/leak.c").write_text('#include <libvsoa/vsoa_client.h>\n', encoding="utf-8")
    code, out = run([sys.executable, str(HERE / "check_arch_layers.py"), "--root", str(root)])
    return code != 0, "core 层引入 libvsoa 头", out


def case_api(tmp):
    header = (REPO / "include/indurtdb/indurtdb.h")
    if not header.is_file():
        return False, "API 计数", "缺少公共头文件，无法造样本"
    bad = tmp / "api_indurtdb.h"
    bad.write_text(header.read_text(encoding="utf-8") + "\nint indurtdb_canary_probe(int x);\n",
                   encoding="utf-8")
    code, out = run([sys.executable, str(HERE / "check_api_count.py"), "--header", str(bad)])
    return code != 0, "头文件多出一个 API", out


def case_hygiene(tmp):
    root = tmp / "hygiene"
    (root / ".superpowers/sdd").mkdir(parents=True)
    (root / ".superpowers/sdd/task.md").write_text("x\n", encoding="utf-8")
    (root / ".gitignore").write_text(".superpowers/\n", encoding="utf-8")
    git = ["git", "-c", "user.email=canary@local", "-c", "user.name=canary"]
    for cmd in (["init", "-q"], ["add", "-f", "."], ["commit", "-q", "-m", "canary"]):
        code, out = run(git + cmd, cwd=root)
        if code != 0:
            return False, "仓库洁净度", f"git {cmd[0]} 失败：{out}"
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(tmp))
    proc = subprocess.run(["bash", str(HERE / "check_repo_hygiene.sh"), "--root", str(root)],
                          cwd=root, capture_output=True, text=True, env=env)
    return proc.returncode != 0, "被跟踪的中间目录", proc.stdout + proc.stderr


def case_conditional_gate(tmp):
    """坏样本：结论「有条件通过」但条件台账未闭环 —— 发布门必须 FAIL。"""
    root = tmp / "conditional"
    root.mkdir()
    artifact = root / "app"
    artifact.write_bytes(b"NEW BUILD: GET /version")
    card = root / "card.md"
    card.write_text("""# Change Card: demo

- 日期: 2026-11-07
- 负责人: dev
- TDD 深度: tdd:浅（理由：单文件小改，风险低）

## 问题与影响

测试条件闭环门禁。

## 设计决策

仅验证发布门。

## 验收判据

未闭环条件不得放行。

## 测试证据

- 命令: `python3 scripts/test_checkers.py`
- 结果: PASS（全部通过）

## 文档影响

无。

## 评审记录

- reviewer_type: ai-independent
- 验证了什么: 跑了发布门自检与回归
- 风险点: 条件未闭环时发布门可能误放行
- 结论: 有条件通过（硬条件闭环前不得发布）

## 发布/部署影响

- 无。
""", encoding="utf-8")
    review = root / "review.md"
    review.write_text("""# Review Evidence: demo

- reviewer_type: ai-independent
- reviewer_context: independent
- date: 2026-11-07

## 验证了什么

运行发布门自检与回归取证。

## 风险点

- 条件未闭环时发布门可能误放行。

## 结论与整改

- 结论: 有条件通过（硬条件闭环前不得发布）：须补条件台账。
""", encoding="utf-8")
    ledger = root / "conditions.json"
    ledger.write_text(json.dumps({
        "hard_conditions": [{"id": "H1", "closed": False, "evidence": ""}],
        "b_segment": {"numbers_ledger": "", "ir_snapshot": "",
                      "required_irs": ["IR1"], "full_gate_log": ""},
    }), encoding="utf-8")
    code, out = run([sys.executable, str(HERE / "release_gate.py"), str(artifact),
                     "--marker", "GET /version", "--change-card", str(card),
                     "--review-evidence", str(review), "--conditions", str(ledger)])
    return code != 0, "有条件通过但硬条件未闭环", out


def case_release_conditions(tmp):
    """坏样本：B 段数字落库台账全 pending —— 校验器必须 FAIL。"""
    root = tmp / "release_conditions"
    root.mkdir()
    evidence = root / "evidence.md"
    evidence.write_text("evidence\n", encoding="utf-8")
    ledger = root / "b-numbers.json"
    ledger.write_text(json.dumps({"schema": 1, "status": "pending", "items": [
        {"id": i, "status": "pending", "ctest_case": f"IR_{i}", "fields": {},
         "evidence": "", "verified_by": "", "verified_at": ""}
        for i in ("P3", "P6", "P7", "P8", "P9", "P10", "P11")]}), encoding="utf-8")
    code, out = run([sys.executable, str(HERE / "check_release_conditions.py"),
                     "--ledger", str(ledger), "--root", str(root)])
    return code != 0, "B 段 7 项全 pending", out


def case_dispatch_boundary(tmp):
    """坏样本：dispatch 层出现 VSOA/fd 符号 —— 符号级守卫必须 FAIL。"""
    root = tmp / "dispatch"
    (root / "rtdbd/src/dispatch").mkdir(parents=True)
    (root / "rtdbd/src/dispatch/write.c").write_text(
        "#include <vsoa_server.h>\n"
        "static int dispatch_write(int fd, vsoa_server_t *srv) {\n"
        "    return vsoa_server_cli_reply(srv, fd, 0, 0, 0, NULL);\n}\n", encoding="utf-8")
    code, out = run([sys.executable, str(HERE / "check_dispatch_boundary.py"),
                     "--root", str(root)])
    return code != 0, "dispatch 层出现 VSOA/fd 符号", out


def case_t1t2(tmp):
    """坏样本：篡改 check_t1t2_bench.py 的 ≤10 μs 阈值为 ≤0.001 μs
    （篡改 Stage 输入值），本机真实 ~0.3μs 也应判 FAIL；工具须返回非 0。

    覆盖 v2 §6 判据 3 / 评审 M2：新增守卫能对坏样本返回非 0，否则即假绿。
    """
    src = HERE / "check_t1t2_bench.py"
    if not src.is_file():
        return False, "T1/T2 基准守卫", "缺少 scripts/check_t1t2_bench.py"
    d = tmp / "t1t2"
    d.mkdir(parents=True, exist_ok=True)
    tampered = d / "check_t1t2_bench_tampered.py"
    text = src.read_text(encoding="utf-8")
    tampered_text = text.replace("P99_THRESHOLD_US = 10.0", "P99_THRESHOLD_US = 0.001")
    if tampered_text == text:
        return False, "阈值常量未被替换", "未找到 'P99_THRESHOLD_US = 10.0'"
    tampered.write_text(tampered_text, encoding="utf-8")
    code, out = run([sys.executable, str(tampered), "--root", str(REPO)])
    return code != 0, "篡改阈值≤0.001μs仍绿(假绿)", out


CASES = [
    ("版本号守卫", case_version),
    ("架构分层守卫", case_arch),
    ("VSOA ring 守卫", case_arch_vsoa),
    ("API 计数守卫", case_api),
    ("仓库洁净度守卫", case_hygiene),
    ("发布门条件闭环守卫", case_conditional_gate),
    ("B 段数字落库守卫", case_release_conditions),
    ("dispatch 符号边界守卫", case_dispatch_boundary),
    ("T1/T2 基准守卫", case_t1t2),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true",
                    help="校验 guard_alive 判据自身（第三签 H-2）后运行全量金丝雀")
    args = ap.parse_args()
    if args.self_test:
        # guard_alive 自身必须可证伪：空输出 / 已知崩溃特征不得判为存活。
        cases = [
            ("空输出应失活", guard_alive("") is False),
            ("段错误应失活", guard_alive("Segmentation fault (core dumped)") is False),
            ("脚本缺失应失活", guard_alive("python3: can't open file 'x.py'") is False),
            ("正常输出应存活", guard_alive("[FAIL] bad sample caught\n") is True),
        ]
        for name, ok in cases:
            print(f"  [{'OK' if ok else 'FAIL'}] guard_alive: {name}")
        if not all(ok for _, ok in cases):
            print("FAIL: guard_alive 自证未通过", file=sys.stderr)
            return 1

    results = []
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for name, fn in CASES:
            try:
                caught, sample, out = fn(tmp)
            except Exception as exc:  # 守卫崩溃也应视为失活
                caught, sample, out = False, str(exc), ""
            if caught and not guard_alive(out):
                caught, sample = False, "守卫脚本缺失或崩溃（非 0 退出码不等于可证伪）"
            results.append({"guard": name, "caught": caught, "sample": sample,
                            "evidence": out.strip().splitlines()[-1] if out.strip() else ""})
            print(f"  [{'OK' if caught else 'FAIL'}] {name}：坏样本（{sample}）"
                  f"{'被抓出' if caught else '未被抓出 → 守卫失活'}")

    ok = all(r["caught"] for r in results)
    if args.json:
        print(json.dumps({"ok": ok, "results": results}, ensure_ascii=False))
    elif ok:
        print(f"OK: {len(results)} 个守卫均可证伪")
    else:
        print("FAIL: 存在对坏样本仍返回 0 的守卫（假绿）", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
