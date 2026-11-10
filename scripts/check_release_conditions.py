#!/usr/bin/env python3
"""B 段数字落库校验器（13 号 §11.2，复评硬条件 2/3）。

校验 ``docs/08-验证记录/v3.12-B段数字落库.json``：7 项（P3/P6–P11）
必须全部 ``status == "measured"``，且每项 ``fields`` 非空、``evidence`` 与
``verified_by`` 指向真实文件、``verified_at`` 为合法日期。任一不满足即 FAIL。

``release_gate.py --conditions`` 会在「有条件通过」结论下调用本模块同样的
``check()``，使 B 段成为发布门的可证伪机械出口，而不是「靠人不调用」。

用法::

    python3 scripts/check_release_conditions.py
    python3 scripts/check_release_conditions.py --json
    python3 scripts/check_release_conditions.py --self-test
    python3 scripts/check_release_conditions.py --ledger <path> --root <dir>

退出码：0 = 7 项全部 measured 且证据齐备；1 = 有未完成项；2 = 用法/读取错误。
"""
import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LEDGER = ROOT / "docs" / "08-验证记录" / "v3.12-B段数字落库.json"
EXPECTED_IDS = ("P3", "P6", "P7", "P8", "P9", "P10", "P11")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def _resolve(raw, root):
    p = Path(raw)
    return p if p.is_absolute() else Path(root) / p


def check(ledger=None, root=None):
    """返回结构化结果；供 release_gate.py 直接调用（避免重复实现判据）。"""
    root = root or ROOT
    ledger = ledger or DEFAULT_LEDGER
    p = _resolve(ledger, root)
    findings = []
    if not p.is_file():
        return {"ok": False, "ledger": str(p), "items": [], "findings": [
            {"level": "FAIL", "field": "ledger", "message": f"not found: {p}"}]}
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (ValueError, OSError) as exc:
        return {"ok": False, "ledger": str(p), "items": [], "findings": [
            {"level": "FAIL", "field": "ledger", "message": f"invalid JSON: {exc}"}]}
    if not isinstance(data, dict) or data.get("schema") != 1:
        findings.append({"level": "FAIL", "field": "schema", "message": "schema must be 1"})

    items = data.get("items") if isinstance(data, dict) else None
    if not isinstance(items, list) or not items:
        findings.append({"level": "FAIL", "field": "items", "message": "missing or empty items"})
        items = []

    seen = set()
    for idx, item in enumerate(items):
        if not isinstance(item, dict):
            findings.append({"level": "FAIL", "field": f"items[{idx}]", "message": "not an object"})
            continue
        iid = item.get("id", f"#{idx}")
        seen.add(iid)
        if item.get("status") != "measured":
            findings.append({"level": "FAIL", "field": f"items[{iid}].status",
                             "message": f"{iid} status={item.get('status')!r}, expected 'measured'"})
            continue
        fields = item.get("fields")
        if not isinstance(fields, dict) or not fields:
            findings.append({"level": "FAIL", "field": f"items[{iid}].fields",
                             "message": "measured item must have non-empty fields"})
        for key in ("evidence", "verified_by"):
            raw = item.get(key)
            if not raw or not _resolve(raw, root).is_file():
                findings.append({"level": "FAIL", "field": f"items[{iid}].{key}",
                                 "message": f"missing or non-existent: {raw}"})
        if not DATE_RE.match(str(item.get("verified_at") or "")):
            findings.append({"level": "FAIL", "field": f"items[{iid}].verified_at",
                             "message": "must be YYYY-MM-DD"})
        if not item.get("ctest_case"):
            findings.append({"level": "FAIL", "field": f"items[{iid}].ctest_case",
                             "message": "missing ctest case name"})

    missing = [i for i in EXPECTED_IDS if i not in seen]
    if missing:
        findings.append({"level": "FAIL", "field": "items", "message": f"missing B-segment items: {missing}"})
    return {"ok": not findings, "ledger": str(p), "items": sorted(seen), "findings": findings}


def self_test():
    import datetime
    cases = []
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        ev = root / "evidence.md"
        ev.write_text("measured evidence\n", encoding="utf-8")
        ledger = root / "ledger.json"

        def write(items, status="pending"):
            ledger.write_text(json.dumps({"schema": 1, "status": status, "items": items},
                                         ensure_ascii=False), encoding="utf-8")

        write([{"id": i, "status": "pending", "ctest_case": f"C_{i}", "fields": {},
                "evidence": "", "verified_by": "", "verified_at": ""} for i in EXPECTED_IDS])
        cases.append(("全 pending 台账应 FAIL", check(ledger, root)["ok"] is False))

        write([{"id": i, "status": "measured", "ctest_case": f"C_{i}",
                "fields": {"metric": 1}, "evidence": str(ev), "verified_by": str(ev),
                "verified_at": datetime.date.today().isoformat()} for i in EXPECTED_IDS])
        cases.append(("全 measured 且证据齐备应 PASS", check(ledger, root)["ok"] is True))

        bad = [{"id": i, "status": "measured", "ctest_case": f"C_{i}",
                "fields": {"metric": 1}, "evidence": str(ev), "verified_by": str(ev),
                "verified_at": "2026-11-07"} for i in EXPECTED_IDS]
        bad[3]["fields"] = {}
        write(bad)
        cases.append(("fields 为空应 FAIL", check(ledger, root)["ok"] is False))

        bad[3]["fields"] = {"metric": 1}
        bad[3]["verified_by"] = "/definitely/not/a/file"
        write(bad)
        cases.append(("verified_by 指向不存在文件应 FAIL", check(ledger, root)["ok"] is False))

        write(bad[:-1])
        cases.append(("缺一项应 FAIL", check(ledger, root)["ok"] is False))

    for name, ok in cases:
        print(f"  [{'OK' if ok else 'FAIL'}] {name}")
    if all(ok for _, ok in cases):
        print(f"OK: 自证通过 {len(cases)} 项")
        return 0
    print("FAIL: 自证未通过（判据被改坏？）", file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser(description="B-segment numbers ledger guard")
    ap.add_argument("--ledger")
    ap.add_argument("--root")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    result = check(args.ledger, args.root)
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        for f in result["findings"]:
            print(f"  [FAIL] {f['field']}: {f['message']}")
        if result["ok"]:
            print(f"OK: B 段 7 项数字均已落库（{result['ledger']}）")
        else:
            print(f"FAIL: {len(result['findings'])} 项未完成（B 段数字落库或证据缺失）")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
