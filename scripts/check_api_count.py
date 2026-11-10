#!/usr/bin/env python3
"""InduRTDB 公开 API 计数门禁（防止文档与头文件漂移）。

解析公共头文件 ``include/indurtdb/indurtdb.h``，统计：
  - v2 句柄函数（``indurtdb_h_*``）
  - v1 全局函数（``indurtdb_*`` 且非 ``indurtdb_h_*``）
  - 二者之和

若与文档声明（``docs/05-SDK手册/03-C-API参考手册.md`` 首段）不一致则退出非 0，
强制维护者在「改 API / 改文档」时同步双方，避免静默漂移。

判定规则：
  - 函数声明 = 标识符 ``indurtdb_h?_[a-z_]+`` 后紧跟 ``(``（忽略空白）。
  - 排除指针 typedef（如 ``indurtdb_callback_t`` 后接 ``*``/``)`` 不匹配）与宏。

用法：
    python3 scripts/check_api_count.py
    python3 scripts/check_api_count.py --header path/to/indurtdb.h
    python3 scripts/check_api_count.py --json
    python3 scripts/check_api_count.py --self-test

v3.12 D8 计划新增公开 API（复评第二签硬条件 1）：
    方案已冻结 ``indurtdb_h_wait_sync()``，但**实现尚未开工**（§6 禁令）。
    若机械地把 EXPECTED_V2 改成 56，头文件实际仍 55，门禁会全程假红——
    用常量造假不是闭环。故登记 PLANNED_V2_APIS 台账：
      * 当前基准保持与头文件一致的 55 / 117；
      * 目标基准 56 / 118 由台账显式暴露（``--json`` 可机械核验）；
      * 一旦该 API 真出现在头文件，本守卫要求 EXPECTED_* 已同步，否则 FAIL，
        防止实现期「加了 API 却忘改基准」静默漂移。
"""
import argparse
import json
import re
import sys
from pathlib import Path

# 期望值：与 docs/05-SDK手册/03-C-API参考手册.md 首段一致。
# v3.4 记为 47/43，v3.7 主题A/B 新增 API 后未同步 —— 该门禁是 CI 的第一步，
# 失配会让整条流水线在 10 秒内直接失败，v3.7 开发期因此全程为红。改动 API 时务必同步此三处：
#   本脚本 EXPECTED_* · SDK 手册首段计数 · 本注释的版本标注
EXPECTED_V1 = 62   # v1 全局函数（v3.7 主题A/B 新增后为 62，此前漏更新致 CI 长期为红）
EXPECTED_V2 = 55   # v2 句柄函数（v3.9 T4 名称域 _in×4 + set_partition 后为 55；此前 50）
EXPECTED_TOTAL = EXPECTED_V1 + EXPECTED_V2  # 117

# v3.12 D8 冻结新增的公开 API（实现期落地，届时须同步 EXPECTED_* 与 SDK 手册首段）。
PLANNED_V2_APIS = ("indurtdb_h_wait_sync",)
TARGET_V2 = EXPECTED_V2 + len(PLANNED_V2_APIS)      # 56
TARGET_TOTAL = EXPECTED_TOTAL + len(PLANNED_V2_APIS)  # 118

# 函数名允许含数字（如 indurtdb_h_write_int32 / indurtdb_read_uint32）。
HEADER_RE = re.compile(r"\bindurtdb_[A-Za-z0-9_]+\s*\(")
V2_RE = re.compile(r"\bindurtdb_h_[A-Za-z0-9_]+\s*\(")


def count_functions(text: str) -> tuple[int, int, int]:
    # 按「函数名」去重：头文件注释中可能出现同名提及（如文档示例），
    # 仅统计不同函数名个数，避免注释提及导致计数虚高。
    all_fns = set(HEADER_RE.findall(text))
    v2 = set(V2_RE.findall(text))
    v1 = len(all_fns) - len(v2)
    return v1, len(v2), len(all_fns)


def evaluate(text):
    """返回 (result, findings)：含 D8 计划 API 的落地一致性校验。"""
    v1, v2, total = count_functions(text)
    findings = []
    if (v1, v2, total) != (EXPECTED_V1, EXPECTED_V2, EXPECTED_TOTAL):
        findings.append("计数与文档声明不一致；若确有新增/移除 API，"
                        "请同步 EXPECTED_* 与 docs/05-SDK手册/03-C-API参考手册.md 首段")

    landed = [name for name in PLANNED_V2_APIS
              if re.search(r"\b" + re.escape(name) + r"\s*\(", text)]
    pending = [name for name in PLANNED_V2_APIS if name not in landed]
    # 计划 API 已落地但基准仍是旧数 → 实现期漏同步，必须 FAIL。
    if landed and EXPECTED_V2 != v2:
        findings.append(f"D8 计划 API {landed} 已落地，但 EXPECTED_V2={EXPECTED_V2} "
                        f"与实际 {v2} 不符，须同步基准与 SDK 手册")

    result = {
        "ok": not findings,
        "v1": v1, "v2": v2, "total": total,
        "expected_v1": EXPECTED_V1, "expected_v2": EXPECTED_V2, "expected_total": EXPECTED_TOTAL,
        "planned_v2_apis": list(PLANNED_V2_APIS),
        "planned_landed": landed, "planned_pending": pending,
        "target_v2": TARGET_V2, "target_total": TARGET_TOTAL,
        "findings": findings,
    }
    return result


def self_test():
    import tempfile
    cases = []
    with tempfile.TemporaryDirectory() as td:
        header = Path(td) / "indurtdb.h"
        # 造一个与当前基准一致、且计划 API 未落地的最小头文件
        fns = [f"int indurtdb_h_f{i}(void);\n" for i in range(EXPECTED_V2)]
        fns += [f"int indurtdb_g{i}(void);\n" for i in range(EXPECTED_V1)]
        header.write_text("".join(fns), encoding="utf-8")
        res = evaluate(header.read_text(encoding="utf-8"))
        cases.append(("未落地时基准一致应 PASS", res["ok"] is True))
        cases.append(("台账应暴露 56/118", res["target_v2"] == 56 and res["target_total"] == 118))
        cases.append(("未落地 API 应列为 pending", "indurtdb_h_wait_sync" in res["planned_pending"]))

        # 计划 API 落地但基准未同步 → 必须 FAIL（防实现期静默漂移）
        header.write_text("".join(fns) + "int indurtdb_h_wait_sync(void);\n", encoding="utf-8")
        bad = evaluate(header.read_text(encoding="utf-8"))
        cases.append(("计划 API 落地未同步基准应 FAIL", bad["ok"] is False))

    for name, ok in cases:
        print(f"  [{'OK' if ok else 'FAIL'}] {name}")
    if all(ok for _, ok in cases):
        print(f"OK: 自证通过 {len(cases)} 项")
        return 0
    print("FAIL: 自证未通过（计划台账被改坏？）", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="InduRTDB public API count guard")
    default_header = Path(__file__).resolve().parent.parent / "include" / "indurtdb" / "indurtdb.h"
    parser.add_argument("--header", default=str(default_header), help="path to indurtdb.h")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    header = Path(args.header)
    if not header.is_file():
        print(f"[check_api_count] ERROR: header not found: {header}", file=sys.stderr)
        return 2

    result = evaluate(header.read_text(encoding="utf-8"))
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        print(f"[check_api_count] v1 全局函数 = {result['v1']} (期望 {EXPECTED_V1})")
        print(f"[check_api_count] v2 句柄函数 = {result['v2']} (期望 {EXPECTED_V2})")
        print(f"[check_api_count] 公开 API 合计 = {result['total']} (期望 {EXPECTED_TOTAL})")
        print(f"[check_api_count] D8 计划新增 {PLANNED_V2_APIS}；"
              f"实现后目标基准 {TARGET_V2}/{TARGET_TOTAL}（当前未落地：{result['planned_pending']}）")
        if result["ok"]:
            print("[check_api_count] OK: 计数与文档声明一致")
        else:
            for f in result["findings"]:
                print(f"[check_api_count] FAIL: {f}", file=sys.stderr)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
