#!/usr/bin/env python3
"""发布门：产物必须存在且含当前功能标记；可选 md5 校验。"""
import argparse
import hashlib
import json
import sys
from pathlib import Path


def file_contains(path, needle, chunk_size=1 << 20):
    path = Path(path)
    if not path.is_file() or not needle:
        return False
    raw = needle.encode() if isinstance(needle, str) else needle
    overlap = max(0, len(raw) - 1)
    tail = b""
    with path.open("rb") as f:
        while True:
            buf = f.read(chunk_size)
            if not buf:
                return False
            window = tail + buf
            if raw in window:
                return True
            tail = window[-overlap:] if overlap else b""


def verdict_class(text):
    clean = (text or "").lstrip("* \t")
    if clean.startswith("返工") or "未通过" in clean or "不通过" in clean:
        return "reject"
    if clean.startswith("通过") or clean.startswith("有条件通过"):
        return "approve"
    return None


def check_process(change_card=None, review_evidence=None):
    findings = []
    classes = []
    here = Path(__file__).resolve().parent
    if str(here) not in sys.path:
        sys.path.insert(0, str(here))
    if change_card:
        from check_change_card import check as check_card
        from check_change_card import conclusion_value as card_conclusion
        if not review_evidence:
            findings.append({"level": "FAIL", "field": "review",
                             "message": "change card requires --review-evidence"})
        for f in check_card(change_card):
            findings.append(dict(f, field=f"change_card.{f['field']}"))
        card_class = verdict_class(card_conclusion(change_card))
        if card_class == "reject":
            findings.append({"level": "FAIL", "field": "change_card.conclusion",
                             "message": "change card was rejected (返工)"})
        if card_class:
            classes.append(("change_card", card_class))
    if review_evidence:
        from check_review_evidence import check as check_review
        from check_review_evidence import conclusion_value
        for f in check_review(review_evidence, require_second=True):
            findings.append(dict(f, field=f"review.{f['field']}"))
        review_class = verdict_class(conclusion_value(review_evidence))
        if review_class == "reject":
            findings.append({"level": "FAIL", "field": "review.conclusion",
                             "message": "review rejected the change (返工/不通过)"})
        if review_class:
            classes.append(("review", review_class))
    if len(classes) == 2 and classes[0][1] != classes[1][1]:
        findings.append({"level": "FAIL", "field": "conclusion",
                         "message": f"card/review verdict mismatch: {classes}"})
    return findings


def check(artifact, markers, expected_md5=None, absent_markers=()):
    p = Path(artifact)
    findings = []
    if not p.is_file():
        return [{"level": "FAIL", "field": "artifact", "message": f"not found: {artifact}"}]
    if not markers and not expected_md5:
        findings.append({"level": "FAIL", "field": "identity",
                         "message": "provide at least one --marker or --md5"})
    for marker in markers:
        if not marker:
            findings.append({"level": "FAIL", "field": "marker",
                             "message": "empty marker is not allowed"})
        elif not file_contains(p, marker):
            findings.append({"level": "FAIL", "field": "marker",
                             "message": f"missing marker: {marker}"})
    for marker in absent_markers:
        if not marker:
            findings.append({"level": "FAIL", "field": "absent_marker",
                             "message": "empty absent-marker is not allowed"})
        elif file_contains(p, marker):
            findings.append({"level": "FAIL", "field": "absent_marker",
                             "message": f"old-build marker present: {marker}"})
    actual_md5 = hashlib.md5(p.read_bytes()).hexdigest()
    expected_md5 = expected_md5.lower() if expected_md5 else None
    if expected_md5 and actual_md5.lower() != expected_md5:
        findings.append({"level": "FAIL", "field": "md5",
                         "message": f"md5 {actual_md5} != expected {expected_md5}"})
    return findings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("artifact", nargs="?")
    ap.add_argument("--marker", action="append", default=[])
    ap.add_argument("--absent-marker", action="append", default=[],
                    help="marker that must NOT appear (e.g. old build text)")
    ap.add_argument("--md5")
    ap.add_argument("--change-card")
    ap.add_argument("--review-evidence")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_checkers import main as run_self_tests
        return run_self_tests()
    if not args.artifact:
        ap.error("artifact is required")
    findings = check(args.artifact, args.marker, args.md5, args.absent_marker)
    findings += check_process(args.change_card, args.review_evidence)
    result = {"ok": not findings, "artifact": args.artifact,
              "md5": hashlib.md5(Path(args.artifact).read_bytes()).hexdigest()
              if Path(args.artifact).is_file() else None,
              "findings": findings}
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        print(("OK: " if result["ok"] else "FAIL: ") + args.artifact)
        for f in findings:
            print(f"  [{f['level']}] {f['field']}: {f['message']}")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
