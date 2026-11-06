#!/usr/bin/env python3
"""检查 change card 必填槽位与证据；缺项 FAIL。"""
import argparse
import json
import re
import sys
from pathlib import Path

REQUIRED = [
    "问题与影响",
    "设计决策",
    "验收判据",
    "测试证据",
    "文档影响",
    "评审记录",
    "发布/部署影响",
]
PLACEHOLDER = re.compile(
    r"待独立评审|待评审|待补|待实现|待回填|TBD|TODO|FIXME", re.I)
SLOT_ONLY = re.compile(r"^<[^<>]+>$")
OPTION_LIST = re.compile(
    r"PASS\s*/\s*FAIL|ai-independent\s*\|\s*human|浅\s*\|\s*中\s*\|\s*深|浅\s*\|\s*免|通过\s*\|\s*有条件通过\s*\|\s*返工", re.I)
VERDICT = re.compile(r"^[*\s]*(通过|有条件通过|返工)")
REJECT_VERDICT = re.compile(r"未通过|不通过")
EMPTY_VALUES = {"无", "n/a", "none", "-", "—"}


def is_concrete(value):
    v = value.strip().strip("`").strip()
    if not v or v.lower() in EMPTY_VALUES:
        return False
    return not (PLACEHOLDER.search(v) or OPTION_LIST.search(v) or SLOT_ONLY.match(v))


def sections(text):
    out, cur = {}, None
    for line in text.splitlines():
        if line.startswith("## "):
            cur = line[3:].strip()
            out[cur] = []
        elif cur is not None:
            out[cur].append(line)
    return {k: "\n".join(v).strip() for k, v in out.items()}


def meta(text, key):
    pat = re.compile(r"^\s*-\s*" + re.escape(key) + r"\s*:\s*(.*)$", re.I)
    for line in text.splitlines():
        m = pat.match(line)
        if m:
            return m.group(1).strip()
    return ""


def check(path):
    findings = []
    p = Path(path)
    if not p.is_file():
        return [{"level": "FAIL", "field": "file", "message": f"not found: {path}"}]
    text = p.read_text(encoding="utf-8", errors="replace")
    sec = sections(text)

    for name in REQUIRED:
        if name not in sec or not sec[name].strip():
            findings.append({"level": "FAIL", "field": name, "message": "missing or empty section"})

    tdd = meta(text, "TDD 深度")
    if not re.search(r"tdd:(浅|中|深|免)", tdd) or "|" in tdd.split("（")[0]:
        findings.append({"level": "FAIL", "field": "TDD 深度",
                         "message": "must declare one of tdd:浅/中/深/免"})

    test = sec.get("测试证据", "")
    test_cmd = meta(test, "命令")
    test_result = meta(test, "结果")
    exempt = meta(test, "豁免")
    if test:
        if not test_cmd and not re.search(r"`[^`]+`", test) and not exempt:
            findings.append({"level": "FAIL", "field": "测试证据",
                             "message": "needs a command or explicit 豁免:"})
        result_lines = [ln for ln in test.splitlines()
                        if re.search(r"(PASS|FAIL|exit|passed|ok|通过)", ln, re.I)
                        and not PLACEHOLDER.search(ln) and not OPTION_LIST.search(ln)
                        and not SLOT_ONLY.match(ln.strip().lstrip("- ").strip())]
        if not result_lines and not exempt:
            findings.append({"level": "FAIL", "field": "测试证据",
                             "message": "needs a result or explicit 豁免:"})
        for field, value in (("命令", test_cmd), ("结果", test_result), ("豁免", exempt)):
            if value and not is_concrete(value):
                findings.append({"level": "FAIL", "field": "测试证据",
                                 "message": f"{field} must be concrete, not 无/placeholder/option list"})
        if test_result and not re.search(r"(PASS|FAIL|exit|passed|ok|通过)", test_result, re.I):
            findings.append({"level": "FAIL", "field": "测试证据",
                             "message": "结果 must contain PASS/FAIL/exit/result"})
        if exempt and not re.search(r"[:：]\s*\S", exempt):
            findings.append({"level": "FAIL", "field": "测试证据",
                             "message": "豁免 needs a reason and follow-up"})

    review = sec.get("评审记录", "")
    # Blockquote notes are template guidance, not review fields.
    review_scan = "\n".join(ln for ln in review.splitlines()
                            if not ln.lstrip().startswith(">"))
    if (PLACEHOLDER.search(review_scan) or OPTION_LIST.search(review_scan)
            or any(SLOT_ONLY.match(ln.strip()) for ln in review_scan.splitlines())):
        findings.append({"level": "FAIL", "field": "评审记录",
                         "message": "review placeholder/option list found"})
    values = {}
    for key in ("reviewer_type", "验证了什么", "风险点", "结论"):
        value = meta(review, key)
        values[key] = value
        if not value:
            findings.append({"level": "FAIL", "field": "评审记录", "message": f"missing {key}"})

    if values["验证了什么"] and not is_concrete(values["验证了什么"]):
        findings.append({"level": "FAIL", "field": "评审记录",
                         "message": "验证了什么 must be concrete"})
    reviewer = values["reviewer_type"]
    if reviewer:
        if re.search(r"author|self|same-context", reviewer, re.I):
            findings.append({"level": "FAIL", "field": "评审记录",
                             "message": "author/self review is not a second signature"})
        elif not re.match(r"^(ai-independent|human)\b", reviewer, re.I):
            findings.append({"level": "FAIL", "field": "评审记录",
                             "message": "reviewer_type must be ai-independent or human"})
        elif not is_concrete(reviewer) or "|" in reviewer:
            findings.append({"level": "FAIL", "field": "评审记录",
                             "message": "reviewer_type must be a single concrete value"})
    conclusion = values["结论"]
    if conclusion:
        if not is_concrete(conclusion) or "|" in conclusion:
            findings.append({"level": "FAIL", "field": "评审记录",
                             "message": "结论 must be a single concrete value"})
        elif REJECT_VERDICT.search(conclusion):
            findings.append({"level": "FAIL", "field": "评审记录",
                             "message": "结论 is a rejection; use 返工"})
        elif not VERDICT.match(conclusion):
            findings.append({"level": "FAIL", "field": "评审记录",
                             "message": "结论 must start with 通过/有条件通过/返工"})
        elif conclusion.lstrip("* \t").startswith("有条件通过") and not re.match(
                r"^[*\s]*有条件通过\s*[（(：:。]", conclusion):
            findings.append({"level": "FAIL", "field": "评审记录",
                             "message": "有条件通过 must list its conditions"})
        elif re.search(r"条件[:：]\s*(无|n/?a|none)", conclusion, re.I):
            findings.append({"level": "FAIL", "field": "评审记录",
                             "message": "有条件通过 needs real conditions, not 无"})
    if values["风险点"] and not re.search(r"[^\s无]", values["风险点"]):
        findings.append({"level": "FAIL", "field": "评审记录",
                         "message": "风险点 must name a risk or explain 无"})
    if values["风险点"] and (PLACEHOLDER.search(values["风险点"])
                             or OPTION_LIST.search(values["风险点"])
                             or SLOT_ONLY.match(values["风险点"])):
        findings.append({"level": "FAIL", "field": "评审记录",
                         "message": "风险点 must be concrete"})
    return findings


def conclusion_value(path):
    """Return the card verdict text for release_gate.py."""
    p = Path(path)
    if not p.is_file():
        return ""
    text = p.read_text(encoding="utf-8", errors="replace")
    return meta(sections(text).get("评审记录", ""), "结论")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("card", nargs="?")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_checkers import main as run_self_tests
        return run_self_tests()
    if not args.card:
        ap.error("card is required")
    findings = check(args.card)
    result = {"ok": not findings, "file": args.card, "findings": findings}
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        print(("OK: " if result["ok"] else "FAIL: ") + args.card)
        for f in findings:
            print(f"  [{f['level']}] {f['field']}: {f['message']}")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
