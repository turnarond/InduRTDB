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
"""
import argparse
import re
import sys
from pathlib import Path

# 期望值：与 docs/05-SDK手册/03-C-API参考手册.md 首段一致（v3.4.0）。
EXPECTED_V1 = 47   # v1 全局函数
EXPECTED_V2 = 43   # v2 句柄函数（indurtdb_h_*）
EXPECTED_TOTAL = EXPECTED_V1 + EXPECTED_V2  # 90

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


def main() -> int:
    parser = argparse.ArgumentParser(description="InduRTDB public API count guard")
    default_header = Path(__file__).resolve().parent.parent / "include" / "indurtdb" / "indurtdb.h"
    parser.add_argument("--header", default=str(default_header), help="path to indurtdb.h")
    args = parser.parse_args()

    header = Path(args.header)
    if not header.is_file():
        print(f"[check_api_count] ERROR: header not found: {header}", file=sys.stderr)
        return 2

    text = header.read_text(encoding="utf-8")
    v1, v2, total = count_functions(text)

    ok = (v1 == EXPECTED_V1) and (v2 == EXPECTED_V2) and (total == EXPECTED_TOTAL)
    print(f"[check_api_count] v1 全局函数 = {v1} (期望 {EXPECTED_V1})")
    print(f"[check_api_count] v2 句柄函数 = {v2} (期望 {EXPECTED_V2})")
    print(f"[check_api_count] 公开 API 合计 = {total} (期望 {EXPECTED_TOTAL})")

    if ok:
        print("[check_api_count] OK: 计数与文档声明一致")
        return 0

    print(
        "[check_api_count] FAIL: 计数与文档声明不一致。\n"
        "  若确有新增/移除 API：请同步修改本脚本 EXPECTED_* 与\n"
        "  docs/05-SDK手册/03-C-API参考手册.md 首段的计数说明。",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
