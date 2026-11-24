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
import subprocess
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


_CTEST_LINE_RE = re.compile(r"^\s*Test\s+#\d+:\s+(\S+)\s*$")
_CTEST_TOTAL_RE = re.compile(r"^Total Tests:\s*(\d+)\s*$", re.M)
_CTEST_PROJECT_RE = re.compile(r"^Test project\s+(.+?)\s*$", re.M)
_IR_BUILD_DIRS_RE = re.compile(r"^# IR_BUILD_DIRS:\s*(.+?)\s*$", re.M)


def parse_ctest_snapshot(text):
    """解析 ctest -N 快照。返回 (names, build_dirs, error)。
    只认 `Test #N: name` 行（doc19 §5.5），结构化校验首行/Total/条目数，
    防手改快照（追加 Test 行或改文本）伪造。"""
    if not isinstance(text, str) or not text:
        return set(), [], "empty snapshot"
    names = set()
    for line in text.splitlines():
        m = _CTEST_LINE_RE.match(line)
        if m:
            names.add(m.group(1))
    total_m = _CTEST_TOTAL_RE.search(text)
    if not total_m:
        return names, [], "missing 'Total Tests: N'"
    total = int(total_m.group(1))
    # 条目数必须与 Total 一致（防手工追加/删除 Test 行）。
    n_test_lines = sum(1 for line in text.splitlines() if _CTEST_LINE_RE.match(line))
    if n_test_lines != total:
        return names, [], f"Test line count {n_test_lines} != Total Tests {total}"
    if not _CTEST_PROJECT_RE.search(text):
        return names, [], "missing 'Test project' header"
    dirs_m = _IR_BUILD_DIRS_RE.search(text)
    build_dirs = dirs_m.group(1).split() if dirs_m else []
    return names, build_dirs, None


def live_ctest_names(build_dir):
    """现场执行 ctest -N，返回真实注册名集合；失败返回 (None, err)。
    self-test 专用注入：环境变量 RTDBD_SELFTEST_CTEST_OUT 指向的文本代替真实
    ctest -N 输出——用于验证门禁逻辑（手改/删名/只跑OFF 被拒），不替代真实现场校验。"""
    import os
    stub = os.environ.get("RTDBD_SELFTEST_CTEST_OUT")
    if stub:
        try:
            text = Path(stub).read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            return None, f"selftest ctest stub unreadable: {exc}"
        names, _, err = parse_ctest_snapshot(text)
        if err:
            return None, f"selftest ctest stub unparsable: {err}"
        return names, None
    bd = Path(build_dir)
    if not bd.is_dir():
        return None, f"build dir missing: {bd}"
    try:
        out = subprocess.run(["ctest", "-N"], cwd=str(bd), capture_output=True,
                             text=True, timeout=60, check=False)
    except (OSError, subprocess.SubprocessError) as exc:
        return None, f"ctest -N failed: {exc}"
    if out.returncode != 0:
        return None, f"ctest -N exit {out.returncode}: {out.stderr[:200]}"
    names, _, err = parse_ctest_snapshot(out.stdout)
    if err:
        return None, f"ctest -N unparsable: {err}"
    return names, None


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

    # doc19 §5.5：ir_snapshot 从文本子串匹配升级为「现场 ctest -N 逐行/集合比对」，
    # 堵「手改快照/名字写进任意文本即过」的假绿（H-2 纵深）。
    required = seg.get("required_irs") or []
    snaps = seg.get("ir_snapshots")          # v2 新增：列表优先（OFF+ON 并集）
    snap_single = seg.get("ir_snapshot")     # v1 兼容：单文件
    if isinstance(snaps, list) and snaps:
        snap_paths = snaps
    elif snap_single:
        snap_paths = [snap_single]
    else:
        snap_paths = []
    if not isinstance(required, list) or not required:
        findings.append({"level": "FAIL", "field": "conditions.b_segment.required_irs",
                         "message": "required_irs must be a non-empty list"})
    if not snap_paths:
        findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                         "message": "ctest -N snapshot missing (ir_snapshot/ir_snapshots)"})
    live_union = set()
    snap_union = set()
    for raw in snap_paths:
        if not isinstance(raw, str) or not raw:
            findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                             "message": f"invalid snapshot path: {raw!r}"})
            continue
        p = _resolve(raw, root)
        if not p.is_file():
            findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                             "message": f"ctest -N snapshot missing: {p}"})
            continue
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                             "message": f"unreadable: {exc}"})
            continue
        snap_names, build_dirs, perr = parse_ctest_snapshot(text)
        if perr:
            findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                             "message": f"snapshot structure invalid ({p}): {perr}"})
            continue
        snap_union |= snap_names
        if not build_dirs:
            pm = _CTEST_PROJECT_RE.search(text)
            build_dirs = [pm.group(1)] if pm else []
        for bd in build_dirs:
            live, lerr = live_ctest_names(bd)
            if live is None:
                findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                                 "message": f"live ctest -N verification failed ({bd}): {lerr}"})
            else:
                live_union |= live
    # 并集精确比对（doc19 §5.5）：每个 build 的现场 ctest -N 集合是快照的子集，
    # 全部 build 的现场并集必须与快照并集精确相等（OFF+ON 并集语义）。
    if snap_paths and live_union:
        if snap_union != live_union:
            only_snap = sorted(snap_union - live_union)
            only_live = sorted(live_union - snap_union)
            findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                             "message": f"snapshot/live ctest union mismatch: snapshot_only={only_snap} live_only={only_live}"})
    if required and snap_paths:
        # required 必须是现场真实注册名集合的子集（精确名比对，非文本包含）。
        # 注：现场集合取 live_union（若现场校验失败则用 snap_union 仅作诊断，仍 FAIL 已记）。
        verify_set = live_union if live_union else snap_union
        missing = [name for name in required if name not in verify_set]
        if missing:
            findings.append({"level": "FAIL", "field": "conditions.b_segment.ir_snapshot",
                             "message": f"IR/Gate tests not registered in ctest: {missing}"})

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
