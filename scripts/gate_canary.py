#!/usr/bin/env python3
"""门禁金丝雀（机制 B：抗假绿）——证明本仓守卫「真的会红」。

做法：在临时目录里造**已知坏样本**，调用各守卫，断言其退出码非 0；
若坏样本仍返回 0，说明守卫失活，本脚本 FAIL。

覆盖：版本号守卫、架构分层守卫、API 计数守卫、仓库洁净度守卫。

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


CASES = [
    ("版本号守卫", case_version),
    ("架构分层守卫", case_arch),
    ("API 计数守卫", case_api),
    ("仓库洁净度守卫", case_hygiene),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    results = []
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for name, fn in CASES:
            try:
                caught, sample, out = fn(tmp)
            except Exception as exc:  # 守卫崩溃也应视为失活
                caught, sample, out = False, str(exc), ""
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
