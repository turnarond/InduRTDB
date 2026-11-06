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
