#!/usr/bin/env python3
"""检查评审证据三要素与第二签；作者自审不算第二签。"""
import argparse
import json
import re
import sys
from pathlib import Path

PLACEHOLDER = re.compile(
    r"待独立评审|待评审|待补|待实现|待回填|TBD|TODO|FIXME", re.I)
SLOT_ONLY = re.compile(r"^<[^<>]+>$")
OPTION_LIST = re.compile(
    r"PASS\s*/\s*FAIL|ai-independent\s*\|\s*human|浅\s*\|\s*中\s*\|\s*深|浅\s*\|\s*免|通过\s*\|\s*有条件通过\s*\|\s*返工", re.I)
VERDICT = re.compile(r"^[*\s]*(通过|有条件通过|返工)")
REJECT_VERDICT = re.compile(r"未通过|不通过")


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


def check(path, require_second=False):
    p = Path(path)
    if not p.is_file():
        return [{"level": "FAIL", "field": "file", "message": f"not found: {path}"}]
    text = p.read_text(encoding="utf-8", errors="replace")
    sec = sections(text)
    findings = []

    rtype = meta(text, "reviewer_type")
    context = meta(text, "reviewer_context")
    date = meta(text, "date")
    if not rtype:
        findings.append({"level": "FAIL", "field": "reviewer_type", "message": "missing"})
    if not date or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date):
        findings.append({"level": "FAIL", "field": "date", "message": "must be YYYY-MM-DD"})
    if require_second:
        if not re.match(r"^(ai-independent|human)\b", rtype, re.I):
            findings.append({"level": "FAIL", "field": "reviewer_type",
                             "message": "second signature must be ai-independent or human"})
        if context.lower() != "independent":
            findings.append({"level": "FAIL", "field": "reviewer_context",
                             "message": "second signature must be independent context"})
    if OPTION_LIST.search(rtype) or PLACEHOLDER.search(rtype):
        findings.append({"level": "FAIL", "field": "reviewer_type",
                         "message": "must be a single concrete value"})
    for name in ("验证了什么", "风险点", "结论与整改"):
        body = sec.get(name, "")
        first = next((ln.strip() for ln in body.splitlines() if ln.strip()), "")
        if any(SLOT_ONLY.match(ln.strip()) for ln in body.splitlines()) or \
                PLACEHOLDER.search(first) or OPTION_LIST.search(first):
            findings.append({"level": "FAIL", "field": name,
                             "message": "placeholder/option list or unfilled slot"})

    for name in ("验证了什么", "风险点", "结论与整改"):
        if name not in sec or not sec[name].strip():
            findings.append({"level": "FAIL", "field": name, "message": "missing or empty"})

    risks = [ln.strip() for ln in sec.get("风险点", "").splitlines()
             if ln.strip().startswith("-") and ln.strip().lstrip("- ").strip()
             and ln.strip().lstrip("- ").strip() != "无"
             and not PLACEHOLDER.search(ln) and not SLOT_ONLY.match(ln.strip().lstrip("- ").strip())]
    if not risks:
        findings.append({"level": "FAIL", "field": "风险点",
                         "message": "at least one concrete risk is required"})

    conclusion = sec.get("结论与整改", "")
    verdict = meta(conclusion, "结论") or conclusion.strip()
    if REJECT_VERDICT.search(verdict):
        findings.append({"level": "FAIL", "field": "结论与整改",
                         "message": "conclusion is a rejection; use 返工"})
    elif not VERDICT.match(verdict):
        findings.append({"level": "FAIL", "field": "结论与整改",
                         "message": "conclusion must start with 通过/有条件通过/返工"})
    elif verdict.lstrip("* \t").startswith("有条件通过") and not re.match(
            r"^[*\s]*有条件通过\s*[（(：:。]", verdict):
        findings.append({"level": "FAIL", "field": "结论与整改",
                         "message": "有条件通过 must list its conditions"})
    return findings


def conclusion_value(path):
    """Return the review verdict text for release_gate.py."""
    p = Path(path)
    if not p.is_file():
        return ""
    text = p.read_text(encoding="utf-8", errors="replace")
    sec = sections(text)
    body = sec.get("结论与整改", "")
    return meta(body, "结论") or body.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("evidence", nargs="?")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--require-second", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_checkers import main as run_self_tests
        return run_self_tests()
    if not args.evidence:
        ap.error("evidence is required")
    findings = check(args.evidence, args.require_second)
    result = {"ok": not findings, "file": args.evidence, "findings": findings}
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        print(("OK: " if result["ok"] else "FAIL: ") + args.evidence)
        for f in findings:
            print(f"  [{f['level']}] {f['field']}: {f['message']}")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
