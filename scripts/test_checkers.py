#!/usr/bin/env python3
"""判据自检：change card / review evidence / release gate（先红后绿）。"""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

GOOD_CARD = """# Change Card: demo

- 日期: 2026-10-23
- 负责人: dev
- TDD 深度: tdd:浅（理由：单文件小改，风险低）

## 问题与影响

设备服务缺少版本端点，运维无法确认运行版本。

## 设计决策

在现有 handler 增加 /version，返回常量；不引入新依赖。

## 验收判据

GET /version 返回 1.0.0；/health 行为不变。

## 测试证据

- 命令: `./build/device_service --self-test`
- 结果: PASS（/version -> 1.0.0）
- 永久测试: tests/test_version.cpp

## 文档影响

更新 README 的端点表。

## 评审记录

- reviewer_type: ai-independent
- 验证了什么: 跑了 self-test 与 /health 回归
- 风险点: 版本常量未与构建元数据绑定
- 结论: 通过

## 发布/部署影响

- 无：纯服务端端点，随下次常规发布。
"""

BAD_CARD_MISSING_TEST = GOOD_CARD.replace("## 测试证据", "## 证据")

BAD_CARD_NO_TDD = GOOD_CARD.replace("tdd:浅（理由：单文件小改，风险低）", "浅")

BAD_CARD_PLACEHOLDER = GOOD_CARD.replace(
    "- reviewer_type: ai-independent\n"
    "- 验证了什么: 跑了 self-test 与 /health 回归\n"
    "- 风险点: 版本常量未与构建元数据绑定\n"
    "- 结论: 通过",
    "- reviewer_type: ai-independent（待独立评审）\n"
    "- 验证了什么: 待独立评审\n"
    "- 风险点: 待独立评审\n"
    "- 结论: 待独立评审")

BAD_CARD_CONCLUSION_HIDDEN = GOOD_CARD.replace(
    "- 验证了什么: 跑了 self-test 与 /health 回归",
    "- 验证了什么: 跑了 self-test 与 /health 回归，全部通过").replace(
    "- 结论: 通过", "- 结论: 待定")

BAD_CARD_SELF_REVIEW = GOOD_CARD.replace(
    "- reviewer_type: ai-independent", "- reviewer_type: author")
BAD_CARD_REJECT = GOOD_CARD.replace("- 结论: 通过", "- 结论: 不通过")
BAD_CARD_CONDITIONAL_NO_CONDITION = GOOD_CARD.replace(
    "- 结论: 通过", "- 结论: 有条件通过")
BAD_CARD_REWORK = GOOD_CARD.replace(
    "- 结论: 通过", "- 结论: 返工（修复前不得发布）")
BAD_CARD_EMPTY_COMMAND = GOOD_CARD.replace(
    "- 命令: `./build/device_service --self-test`", "- 命令: 无")
BAD_CARD_EMPTY_RESULT = GOOD_CARD.replace(
    "- 结果: PASS（/version -> 1.0.0）", "- 结果: 无")
BAD_CARD_REVERSE_REVIEWER = GOOD_CARD.replace(
    "- reviewer_type: ai-independent", "- reviewer_type: human | ai-independent")
BAD_CARD_CONCLUSION_PIPE = GOOD_CARD.replace(
    "- 结论: 通过", "- 结论: 通过 | 返工")
BAD_CARD_CONDITION_NONE = GOOD_CARD.replace(
    "- 结论: 通过", "- 结论: 有条件通过（条件：无）")

# 「有条件通过」是独立第三类结论：既不是 approve 也不是 reject。
GOOD_CARD_CONDITIONAL = GOOD_CARD.replace(
    "- 结论: 通过", "- 结论: 有条件通过（硬条件 1–4 全部闭环并经复验前不得发布）")

GOOD_REVIEW = """# Review Evidence: demo

- reviewer_type: ai-independent
- reviewer_context: independent
- date: 2026-10-23

## 验证了什么

运行 38 个主机测试，读 repository/manager/controller 调用链。

## 风险点

- 并发窄窗可能返回 409 无 data。

## 结论与整改

通过；窄窗已文档化，工具按 409 处理。
"""

BAD_REVIEW_SELF = GOOD_REVIEW.replace("ai-independent", "author").replace("independent", "same-context")

BAD_REVIEW_NO_RISK = GOOD_REVIEW.replace("- 并发窄窗可能返回 409 无 data。", "无")

BAD_REVIEW_PENDING = GOOD_REVIEW.replace(
    "运行 38 个主机测试，读 repository/manager/controller 调用链。", "待补")

BAD_REVIEW_REWORK = GOOD_REVIEW.replace(
    "通过；窄窗已文档化，工具按 409 处理。", "返工：修复前不得发布。")

GOOD_REVIEW_CONDITIONAL = GOOD_REVIEW.replace(
    "通过；窄窗已文档化，工具按 409 处理。",
    "有条件通过（硬条件 1–4 闭环前不得发布）：须补命令取证与 B 段机械出口。")


def run(script, args):
    return subprocess.run([sys.executable, str(HERE / script), *args],
                          capture_output=True, text=True)


def check(cond, label):
    print(("  PASS: " if cond else "  FAIL: ") + label)
    return 0 if cond else 1


def result_ok(r):
    try:
        return json.loads(r.stdout).get("ok")
    except Exception:
        return None


def main():
    failed = 0
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        card = d / "change.md"
        card.write_text(GOOD_CARD, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode == 0 and json.loads(r.stdout)["ok"], "good change card accepted")
        card.write_text(BAD_CARD_MISSING_TEST, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0 and not json.loads(r.stdout)["ok"], "missing test evidence rejected")
        card.write_text(BAD_CARD_NO_TDD, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "missing tdd depth rejected")
        card.write_text(BAD_CARD_PLACEHOLDER, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "placeholder review record rejected")
        card.write_text(BAD_CARD_CONCLUSION_HIDDEN, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "conclusion outside 结论 line rejected")
        card.write_text(BAD_CARD_SELF_REVIEW, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "author self-review rejected")
        card.write_text(BAD_CARD_REJECT, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "rejection verdict rejected")
        card.write_text(BAD_CARD_CONDITIONAL_NO_CONDITION, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "conditional pass without conditions rejected")
        card.write_text(BAD_CARD_EMPTY_COMMAND, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "empty test command rejected")
        card.write_text(BAD_CARD_EMPTY_RESULT, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "empty test result rejected")
        card.write_text(BAD_CARD_REVERSE_REVIEWER, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "reviewer option list rejected")
        card.write_text(BAD_CARD_CONCLUSION_PIPE, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "conclusion option list rejected")
        card.write_text(BAD_CARD_CONDITION_NONE, encoding="utf-8")
        r = run("check_change_card.py", [str(card), "--json"])
        failed += check(r.returncode != 0, "conditional pass with 条件:无 rejected")
        tpl = HERE.parent / "references" / "templates" / "change-card.md"
        if tpl.is_file():
            card.write_text(tpl.read_text(encoding="utf-8"), encoding="utf-8")
            r = run("check_change_card.py", [str(card), "--json"])
            failed += check(r.returncode != 0, "unfilled change-card template rejected")
            rtpl = HERE.parent / "references" / "templates" / "review-evidence.md"
            rtemplate = d / "review-template.md"
            rtemplate.write_text(rtpl.read_text(encoding="utf-8"), encoding="utf-8")
            r = run("check_review_evidence.py", [str(rtemplate), "--json", "--require-second"])
            failed += check(r.returncode != 0, "unfilled review-evidence template rejected")
        else:
            print("  SKIP: templates not copied into this project")

        review = d / "review.md"
        review.write_text(GOOD_REVIEW, encoding="utf-8")
        r = run("check_review_evidence.py", [str(review), "--json", "--require-second"])
        failed += check(r.returncode == 0 and json.loads(r.stdout)["ok"], "independent review accepted")
        review.write_text(BAD_REVIEW_SELF, encoding="utf-8")
        r = run("check_review_evidence.py", [str(review), "--json", "--require-second"])
        failed += check(r.returncode != 0, "author self-review rejected as second signature")
        review.write_text(BAD_REVIEW_NO_RISK, encoding="utf-8")
        r = run("check_review_evidence.py", [str(review), "--json", "--require-second"])
        failed += check(r.returncode != 0, "review without risk rejected")
        review.write_text(BAD_REVIEW_PENDING, encoding="utf-8")
        r = run("check_review_evidence.py", [str(review), "--json", "--require-second"])
        failed += check(r.returncode != 0, "pending review verification rejected")

        artifact = d / "service"
        artifact.write_bytes(b"binary with /api/sys/firmware/upgrade/current marker")
        r = run("release_gate.py", [str(artifact), "--marker", "/api/sys/firmware/upgrade/current", "--json"])
        failed += check(r.returncode == 0 and json.loads(r.stdout)["ok"], "artifact with marker accepted")
        artifact.write_bytes(b"old binary")
        r = run("release_gate.py", [str(artifact), "--marker", "/api/sys/firmware/upgrade/current", "--json"])
        failed += check(r.returncode != 0 and not json.loads(r.stdout)["ok"], "stale artifact rejected")

        artifact.write_bytes(b"OLD BUILD: no version endpoint")
        r = run("release_gate.py", [str(artifact), "--marker", "version endpoint", "--json"])
        failed += check(r.returncode == 0, "weak marker alone is a known false positive")
        r = run("release_gate.py", [str(artifact), "--marker", "version endpoint",
                                   "--absent-marker", "OLD BUILD", "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "old-build marker rejected by absent-marker")
        artifact.write_bytes(b"NEW BUILD: GET /version")
        r = run("release_gate.py", [str(artifact), "--marker", "GET /version",
                                   "--absent-marker", "OLD BUILD", "--json"])
        failed += check(r.returncode == 0 and result_ok(r) is True,
                        "new artifact accepted with absent-marker")

        card.write_text(GOOD_CARD, encoding="utf-8")
        review.write_text(GOOD_REVIEW, encoding="utf-8")
        r = run("release_gate.py", [str(artifact), "--marker", "GET /version",
                                   "--change-card", str(card),
                                   "--review-evidence", str(review), "--json"])
        failed += check(r.returncode == 0 and result_ok(r) is True,
                        "release with good card and review accepted")
        r = run("release_gate.py", [str(artifact), "--marker", "GET /version",
                                   "--change-card", str(card), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "release with card but no review rejected")
        card.write_text(BAD_CARD_PLACEHOLDER, encoding="utf-8")
        r = run("release_gate.py", [str(artifact), "--marker", "GET /version",
                                   "--change-card", str(card), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "release blocked by placeholder change card")
        card.write_text(GOOD_CARD, encoding="utf-8")
        review.write_text(BAD_REVIEW_SELF, encoding="utf-8")
        r = run("release_gate.py", [str(artifact), "--marker", "GET /version",
                                   "--review-evidence", str(review), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "release blocked by author-only review")
        r = run("release_gate.py", [str(artifact), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "release without marker/md5 rejected")
        r = run("release_gate.py", [str(artifact), "--marker", "", "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "empty marker rejected")
        card.write_text(GOOD_CARD, encoding="utf-8")
        review.write_text(BAD_REVIEW_REWORK, encoding="utf-8")
        r = run("release_gate.py", [str(artifact), "--marker", "GET /version",
                                   "--change-card", str(card),
                                   "--review-evidence", str(review), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "release blocked by rework verdict")
        card.write_text(BAD_CARD_REWORK, encoding="utf-8")
        review.write_text(GOOD_REVIEW, encoding="utf-8")
        r = run("release_gate.py", [str(artifact), "--marker", "GET /version",
                                   "--change-card", str(card), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "release blocked by card-only rework verdict")
        r = run("release_gate.py", [str(artifact), "--marker", "GET /version",
                                   "--change-card", str(card),
                                   "--review-evidence", str(review), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "release blocked by card/review verdict mismatch")
        card.write_text(GOOD_CARD, encoding="utf-8")
        review.write_text(GOOD_REVIEW, encoding="utf-8")
        artifact.write_bytes(b"NEW BUILD: GET /version")
        md5_upper = __import__("hashlib").md5(artifact.read_bytes()).hexdigest().upper()
        r = run("release_gate.py", [str(artifact), "--marker", "GET /version",
                                   "--md5", md5_upper, "--json"])
        failed += check(r.returncode == 0 and result_ok(r) is True,
                        "uppercase md5 accepted")

        # 「有条件通过」必须是独立第三类，不得等价无条件 approve（复评硬条件 3）
        sys.path.insert(0, str(HERE))
        from release_gate import verdict_class as _verdict_class
        failed += check(_verdict_class("有条件通过（硬条件未闭环）") == "conditional",
                        "conditional verdict classified separately from approve")
        card.write_text(GOOD_CARD_CONDITIONAL, encoding="utf-8")
        review.write_text(GOOD_REVIEW_CONDITIONAL, encoding="utf-8")
        r = run("release_gate.py", [str(artifact), "--marker", "GET /version",
                                   "--change-card", str(card),
                                   "--review-evidence", str(review), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "conditional verdict without closed conditions rejected")

        # 条件闭环台账：硬条件未全闭环 / B 段数字未落库 / IR 未注册 / 全量门禁有 SKIP 均须 FAIL
        ev = d / "hc-evidence.md"
        ev.write_text("hard condition evidence\n", encoding="utf-8")
        numbers = d / "b-numbers.json"
        # 全 measured 且证据齐备的合法台账（判据由 check_release_conditions.py 复用）
        numbers.write_text(json.dumps({
            "schema": 1, "status": "measured",
            "items": [{"id": i, "status": "measured", "ctest_case": f"IR_{i}",
                       "fields": {"metric": 1}, "evidence": str(ev),
                       "verified_by": str(ev), "verified_at": "2026-11-07"}
                      for i in ("P3", "P6", "P7", "P8", "P9", "P10", "P11")],
        }), encoding="utf-8")
        ir_snap = d / "ctest-snapshot.txt"
        ir_snap.write_text("Test #43: IR1_分区收敛\nTest #44: IR7_迁移\n", encoding="utf-8")
        gate_log = d / "check-all.log"
        gate_log.write_text("[OK] canary（0s）\nALL GREEN\n", encoding="utf-8")

        verified_ev = d / "independent-review.md"
        verified_ev.write_text("""# Review Evidence: independent re-review

- reviewer_type: ai-independent
- reviewer_context: independent
- date: 2026-11-07

## 验证了什么

运行发布门自检与条件闭环台账复核，逐条核验硬条件证据。

## 风险点

- 硬条件未闭环时发布门可能误放行。

## 结论与整改

- 结论: 通过（硬条件已闭环）：按台账复验方式继续。
""", encoding="utf-8")

        def ledger(hc_closed, b_numbers, irs, gate_text, verified=True):
            gate_log.write_text(gate_text, encoding="utf-8")
            ir_snap.write_text(irs, encoding="utf-8")
            hc = {"id": "H1", "closed": hc_closed, "evidence": str(ev)}
            if verified:
                hc["verified_by"] = str(verified_ev)
                hc["verified_at"] = "2026-11-07"
            obj = {
                "hard_conditions": [hc],
                "b_segment": {
                    "numbers_ledger": str(numbers) if b_numbers else "",
                    "ir_snapshot": str(ir_snap),
                    "required_irs": ["IR1", "IR7"],
                    "full_gate_log": str(gate_log),
                },
            }
            path = d / "conditions.json"
            path.write_text(json.dumps(obj), encoding="utf-8")
            return str(path)

        base = [str(artifact), "--marker", "GET /version",
                "--change-card", str(card), "--review-evidence", str(review), "--json"]
        r = run("release_gate.py", base + ["--conditions", ledger(True, True, ir_snap.read_text(), "[OK] canary（0s）\nALL GREEN\n")])
        failed += check(r.returncode == 0 and result_ok(r) is True,
                        "conditional with fully closed conditions accepted")
        r = run("release_gate.py", base + ["--conditions", ledger(False, True, ir_snap.read_text(), "ALL GREEN\n")])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "conditional with open hard condition rejected")
        # closed=true 但无独立复验证据 = 自审自批，必须被拒（§11 承诺）
        r = run("release_gate.py", base + ["--conditions",
                ledger(True, True, ir_snap.read_text(), "ALL GREEN\n", verified=False)])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "self-closed condition without independent re-review rejected")

        def ledger_with(vb, vat):
            obj = {"hard_conditions": [{"id": "H1", "closed": True, "evidence": str(ev),
                                        "verified_by": vb, "verified_at": vat}],
                   "b_segment": {"numbers_ledger": str(numbers), "ir_snapshot": str(ir_snap),
                                 "required_irs": ["IR1", "IR7"], "full_gate_log": str(gate_log)}}
            path = d / "bad-conditions.json"
            path.write_text(json.dumps(obj), encoding="utf-8")
            return str(path)

        # 存在但不是独立复验证据的文件（含 /etc/hostname 类）必须被拒（第三签 H-1）
        r = run("release_gate.py", base + ["--conditions", ledger_with("/etc/hostname", "2026-11-07")])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "non-review file as independent evidence rejected")
        fake_review = d / "fake-review.md"
        fake_review.write_text("# 随手写的说明\n\n没有评审三要素。\n", encoding="utf-8")
        r = run("release_gate.py", base + ["--conditions", ledger_with(str(fake_review), "2026-11-07")])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "existing but non-review evidence rejected")
        # 日历上不存在的日期必须被拒
        r = run("release_gate.py", base + ["--conditions", ledger_with(str(verified_ev), "9999-99-99")])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "impossible verified_at rejected")
        r = run("release_gate.py", base + ["--conditions", ledger_with("/no/such/review.md", "2026-11-07")])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "missing independent-review evidence rejected")
        # 第四签 H-2 纵深：reviewer_type=human 的伪证据即使其余格式合规也必须被拒
        # （require_second 仅强制 reviewer_context=independent，不拒绝 human，会留自审自批口子）
        human_review = d / "human-review.md"
        human_review.write_text("""# Review Evidence

- reviewer_type: human
- reviewer_context: independent
- date: 2026-11-07

## 验证了什么

冒充独立复验。

## 风险点

- 风险点占位。

## 结论与整改

- 结论: 通过（冒充独立）：就这么着。
""", encoding="utf-8")
        r = run("release_gate.py", base + ["--conditions", ledger_with(str(human_review), "2026-11-07")])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "human-type evidence not accepted as independent re-review")
        r = run("release_gate.py", base + ["--conditions", ledger(True, False, ir_snap.read_text(), "ALL GREEN\n")])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "conditional without B-segment numbers rejected")
        # 台账文件存在但仍是 pending：不得因「文件存在」就放行
        saved = numbers.read_text(encoding="utf-8")
        numbers.write_text(json.dumps({"schema": 1, "status": "pending", "items": [
            {"id": i, "status": "pending", "ctest_case": f"IR_{i}", "fields": {},
             "evidence": "", "verified_by": "", "verified_at": ""}
            for i in ("P3", "P6", "P7", "P8", "P9", "P10", "P11")]}), encoding="utf-8")
        r = run("release_gate.py", base + ["--conditions", ledger(True, True, ir_snap.read_text(), "ALL GREEN\n")])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "conditional with pending B-segment ledger rejected")
        numbers.write_text(saved, encoding="utf-8")
        r = run("release_gate.py", base + ["--conditions", ledger(True, True, "Test #1: test_c_shm\n", "ALL GREEN\n")])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "conditional with missing IR tests rejected")
        r = run("release_gate.py", base + ["--conditions", ledger(True, True, ir_snap.read_text(), "[SKIP] canary\nALL GREEN\n")])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "conditional with quick gate log rejected")

        # D8 计划新增 API 台账：方案已决定加公开 API，门禁须显式登记而非静默按旧数校验
        r = run("check_api_count.py", ["--json"])
        failed += check(r.returncode == 0 and "planned_v2_apis" in r.stdout
                        and '"target_v2": 56' in r.stdout and '"target_total": 118' in r.stdout,
                        "api count declares D8 planned API and target baseline")
        r = run("check_api_count.py", ["--self-test"])
        failed += check(r.returncode == 0, "api count planned-ledger self-test passes")

        # B 段数字落库校验器：pending 台账须 FAIL，全 measured 且字段非空才 PASS
        r = run("check_release_conditions.py", ["--ledger",
                str(HERE.parent / "docs/08-验证记录/v3.12-B段数字落库.json"), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "pending B-segment numbers ledger rejected")
        r = run("check_release_conditions.py", ["--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "B-segment checker fails on real pending ledger by default")
        r = run("check_release_conditions.py", ["--self-test"])
        failed += check(r.returncode == 0, "B-segment checker self-test passes")

        # ---- VSOA SDK 探测门禁（M1.3a）：缺任一项必须报错，不得静默降级 ----
        def make_sdk(root, headers=True, libs=True, version="1.9.1"):
            inc = root / "include" / "libvsoa"
            inc.mkdir(parents=True, exist_ok=True)
            if headers:
                (inc / "vsoa_server.h").write_text("/* fake */\n", encoding="utf-8")
                (inc / "vsoa_client.h").write_text("/* fake */\n", encoding="utf-8")
                (inc / "vsoa_version.h").write_text(
                    f'#define VSOA_LIB_VERSION    "{version}"\n', encoding="utf-8")
            libdir = root / "lib" / "linux" / "x86_64"
            libdir.mkdir(parents=True, exist_ok=True)
            if libs:
                for n in ("libvsoa-server.so", "libvsoa-client.so"):
                    (libdir / n).write_bytes(b"\x7fELF fake\n")

        sdk = d / "sdk-ok"
        make_sdk(sdk)
        r = run("check_vsoa_sdk.py", ["--root", str(sdk), "--json"])
        failed += check(r.returncode == 0 and result_ok(r) is True,
                        "complete VSOA SDK layout accepted")

        sdk_no_hdr = d / "sdk-no-hdr"
        make_sdk(sdk_no_hdr, headers=False)
        r = run("check_vsoa_sdk.py", ["--root", str(sdk_no_hdr), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "VSOA SDK missing headers rejected")

        sdk_no_lib = d / "sdk-no-lib"
        make_sdk(sdk_no_lib, libs=False)
        r = run("check_vsoa_sdk.py", ["--root", str(sdk_no_lib), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "VSOA SDK missing libraries rejected")

        sdk_bad_ver = d / "sdk-bad-ver"
        make_sdk(sdk_bad_ver, version="1.8.0")
        r = run("check_vsoa_sdk.py", ["--root", str(sdk_bad_ver), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "VSOA SDK version mismatch rejected")

        r = run("check_vsoa_sdk.py", ["--root", str(d / "no-such-sdk"), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "missing VSOA SDK root rejected")

        design = d / "design.md"
        design.write_text("design plan\n", encoding="utf-8")
        baseline = d / "design-baseline.json"
        r = run("freeze_baseline.py", ["--baseline", str(baseline), str(design), "--json"])
        failed += check(r.returncode == 0 and result_ok(r) is True,
                        "baseline freeze accepted")
        r = run("freeze_baseline.py", ["--baseline", str(baseline), str(design), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "baseline freeze refuses overwrite")
        src = d / "src"
        src.mkdir()
        (src / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        r = run("check_baseline_order.py", ["--baseline", str(baseline),
                                           "--source", str(src), "--json"])
        failed += check(r.returncode == 0 and result_ok(r) is True,
                        "baseline before implementation accepted")
        tests_dir = d / "tests"
        tests_dir.mkdir()
        (tests_dir / "test_main.cpp").write_text("// test\n", encoding="utf-8")
        r = run("check_baseline_order.py", ["--baseline", str(baseline),
                                           "--source", str(src), str(tests_dir), "--json"])
        failed += check(r.returncode == 0 and result_ok(r) is True,
                        "multiple --source values accepted")

        portable = d / "portable"
        portable.mkdir()
        (portable / "design.md").write_text("portable design\n", encoding="utf-8")
        pbase = portable / "baseline.json"
        r = run("freeze_baseline.py", ["--baseline", str(pbase),
                                      str(portable / "design.md"), "--json"])
        failed += check(r.returncode == 0, "portable baseline frozen")
        (portable / "src").mkdir()
        (portable / "src" / "main.cpp").write_text("portable code\n", encoding="utf-8")
        moved = d / "moved"
        shutil.copytree(portable, moved)
        r = run("check_baseline_order.py", ["--baseline", str(moved / "baseline.json"),
                                           "--source", str(moved / "src"), "--json"])
        failed += check(r.returncode == 0 and result_ok(r) is True,
                        "baseline paths remain valid after moving the project")

        only = d / "only"
        only.mkdir()
        (only / "design.md").write_text("only design\n", encoding="utf-8")
        obase = only / "baseline.json"
        r = run("freeze_baseline.py", ["--baseline", str(obase),
                                      str(only / "design.md"), "--json"])
        failed += check(r.returncode == 0, "only-design baseline frozen")
        r = run("check_baseline_order.py", ["--baseline", str(obase),
                                           "--source", str(only), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "baseline itself does not count as implementation")
        badbase = d / "bad-baseline.json"
        badbase.write_text("[]\n", encoding="utf-8")
        r = run("check_baseline_order.py", ["--baseline", str(badbase),
                                           "--source", str(src), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "non-object baseline rejected")

        late = d / "late"
        late.mkdir()
        late_src = late / "src"
        late_src.mkdir()
        (late_src / "main.cpp").write_text("late\n", encoding="utf-8")
        late_design = late / "design.md"
        late_design.write_text("late design\n", encoding="utf-8")
        late_baseline = late / "baseline.json"
        r = run("freeze_baseline.py", ["--baseline", str(late_baseline),
                                      str(late_design), "--json"])
        failed += check(r.returncode == 0, "late baseline frozen for negative case")
        r = run("check_baseline_order.py", ["--baseline", str(late_baseline),
                                           "--source", str(late_src), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "freeze after implementation rejected")

        design.write_text("design plan v2\n", encoding="utf-8")
        r = run("check_baseline_order.py", ["--baseline", str(baseline),
                                           "--source", str(src), "--json"])
        failed += check(r.returncode == 0 and result_ok(r) is True,
                        "design revision after freeze is a warning, not a failure")

        design.unlink()
        r = run("check_baseline_order.py", ["--baseline", str(baseline),
                                           "--source", str(src), "--json"])
        failed += check(r.returncode != 0 and result_ok(r) is False,
                        "deleted baseline artifact rejected")

    print(f"test_checkers: {'ALL PASS' if failed == 0 else str(failed) + ' FAILED'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
