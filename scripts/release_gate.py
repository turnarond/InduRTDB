#!/usr/bin/env python3
"""发布门：产物必须存在且含当前功能标记；可选 md5 校验。

结论分三类：approve（通过）/ conditional（有条件通过）/ reject（返工）。
「有条件通过」不再等价无条件放行：必须提供 ``--conditions`` 指向机器可读的
条件闭环台账，且台账中每条硬条件已闭环、B 段数字已落库、IR1–IR7 已注册、
全量门禁无 [SKIP] canary，任一不满足即 FAIL（复评第二签硬条件 3）。
"""
import argparse
import datetime
import hashlib
import json
import re
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
    # 必须先判 conditional：「有条件通过」也含「通过」，落到 approve 即为假绿。
    if clean.startswith("有条件通过"):
        return "conditional"
    if clean.startswith("通过"):
        return "approve"
    return None


def _resolve(raw, root):
    p = Path(raw)
    return p if p.is_absolute() else Path(root) / p


def check_conditions(ledger, root):
    """校验条件闭环台账：硬条件全闭环 + B 段数字落库 + IR 注册 + 全量门禁。"""
    findings = []
    p = _resolve(ledger, root)
    if not p.is_file():
        return [{"level": "FAIL", "field": "conditions",
                 "message": f"conditions ledger not found: {p}"}]
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (ValueError, OSError) as exc:
        return [{"level": "FAIL", "field": "conditions",
                 "message": f"invalid conditions ledger: {exc}"}]
    if not isinstance(data, dict):
        return [{"level": "FAIL", "field": "conditions",
                 "message": "conditions ledger must be a JSON object"}]

    hcs = data.get("hard_conditions")
    if not isinstance(hcs, list) or not hcs:
        findings.append({"level": "FAIL", "field": "conditions.hard_conditions",
                         "message": "missing or empty hard_conditions"})
    else:
        for idx, hc in enumerate(hcs):
            if not isinstance(hc, dict) or hc.get("closed") is not True:
                findings.append({"level": "FAIL", "field": "conditions.hard_conditions",
                                 "message": f"hard condition #{idx + 1} ({hc.get('id', '?') if isinstance(hc, dict) else '?'}) not closed"})
                continue
            cid = hc.get("id", "?")
            ev = hc.get("evidence")
            if not ev or not _resolve(ev, root).is_file():
                findings.append({"level": "FAIL", "field": "conditions.hard_conditions",
                                 "message": f"hard condition {cid} evidence missing: {ev}"})
            # §11.3：closed 只能由独立复验签字，实现者不得自置。
            # 没有 verified_by / verified_at，或证据文件不存在，一律视为自审自批。
            vb = hc.get("verified_by")
            vb_path = _resolve(vb, root) if vb else None
            if not vb or not vb_path.is_file():
                findings.append({"level": "FAIL", "field": "conditions.hard_conditions",
                                 "message": f"hard condition {cid} closed without independent re-review evidence: {vb}"})
            else:
                # 第三签 H-1：文件存在不够，内容必须是合规的独立复验证据
                # （ai-independent / independent / 三要素 / 非自审），否则实现者
                # 可用 /etc/hostname 或随手写的 md 自置 closed。
                from check_review_evidence import check as check_review
                for f in check_review(str(vb_path), require_second=True):
                    findings.append({"level": "FAIL", "field": "conditions.hard_conditions",
                                     "message": f"hard condition {cid} verified_by invalid: {f['field']}: {f['message']}"})
                # 第四签 H-2 纵深：require_second 仅强制 reviewer_context=independent，
                # 不拒绝 reviewer_type=human，会留「自撰 human 伪证据自审自批」口子。
                # 闭环证据的签字方必须是独立上下文（ai-independent），机械收窄。
                mt = re.search(r"reviewer_type:\s*(\S+)", vb_path.read_text(encoding="utf-8", errors="ignore"))
                if not mt or mt.group(1) != "ai-independent":
                    findings.append({"level": "FAIL", "field": "conditions.hard_conditions",
                                     "message": f"hard condition {cid} verified_by must be signed by ai-independent, got {mt.group(1) if mt else 'missing'}"})
                mc = re.search(r"reviewer_context:\s*(\S+)", vb_path.read_text(encoding="utf-8", errors="ignore"))
                if not mc or mc.group(1) != "independent":
                    findings.append({"level": "FAIL", "field": "conditions.hard_conditions",
                                     "message": f"hard condition {cid} verified_by must be independent context, got {mc.group(1) if mc else 'missing'}"})
            raw_date = str(hc.get("verified_at") or "")
            try:
                if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", raw_date):
                    raise ValueError("format")
                datetime.date.fromisoformat(raw_date)
            except ValueError:
                findings.append({"level": "FAIL", "field": "conditions.hard_conditions",
                                 "message": f"hard condition {cid} verified_at must be a real YYYY-MM-DD date: {raw_date}"})

    seg = data.get("b_segment")
    if not isinstance(seg, dict):
        findings.append({"level": "FAIL", "field": "conditions.b_segment",
                         "message": "missing b_segment"})
        return findings

    numbers = seg.get("numbers_ledger")
    if not numbers or not _resolve(numbers, root).is_file():
        findings.append({"level": "FAIL", "field": "conditions.b_segment.numbers_ledger",
                         "message": "B-segment numbers ledger missing or empty"})
    else:
        # 复用同一判据（13 号 §11.2）：7 项须全部 measured 且证据齐备，
        # 不能只判「文件存在」——否则 pending 台账会被误放行。
        from check_release_conditions import check as check_numbers
        res = check_numbers(numbers, root)
        for f in res["findings"]:
            findings.append(dict(f, field=f"conditions.b_segment.{f['field']}"))

    snap = seg.get("ir_snapshot")
    required = seg.get("required_irs") or []
    if not snap or not _resolve(snap, root).is_file():
        findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                         "message": "ctest -N snapshot missing"})
    elif not isinstance(required, list) or not required:
        findings.append({"level": "FAIL", "field": "conditions.b_segment.required_irs",
                         "message": "required_irs must be a non-empty list"})
    else:
        try:
            text = _resolve(snap, root).read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            text = ""
            findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                             "message": f"unreadable: {exc}"})
        missing = [name for name in required if name not in text]
        if missing:
            findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                             "message": f"IR tests not registered in ctest: {missing}"})

    log = seg.get("full_gate_log")
    if not log or not _resolve(log, root).is_file():
        findings.append({"level": "FAIL", "field": "conditions.b_segment.full_gate_log",
                         "message": "full gate log missing"})
    else:
        try:
            text = _resolve(log, root).read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            text = ""
            findings.append({"level": "FAIL", "field": "conditions.b_segment.full_gate_log",
                             "message": f"unreadable: {exc}"})
        if "[SKIP] canary" in text:
            findings.append({"level": "FAIL", "field": "conditions.b_segment.full_gate_log",
                             "message": "gate log contains [SKIP] canary (--quick is forbidden)"})
        if "ALL GREEN" not in text:
            findings.append({"level": "FAIL", "field": "conditions.b_segment.full_gate_log",
                             "message": "gate log has no ALL GREEN"})
    return findings


def check_process(change_card=None, review_evidence=None, conditions=None, root=None):
    findings = []
    classes = []
    here = Path(__file__).resolve().parent
    root = root or here.parent
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
    if "conditional" in [c for _, c in classes]:
        if not conditions:
            findings.append({"level": "FAIL", "field": "conditions",
                             "message": "conditional verdict requires --conditions ledger"})
        else:
            findings += check_conditions(conditions, root)
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
    ap.add_argument("--conditions", help="条件闭环台账 JSON（conditional 结论必填）")
    ap.add_argument("--root", help="台账内相对路径的解析根（默认仓库根）")
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
    findings += check_process(args.change_card, args.review_evidence, args.conditions, args.root)
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
