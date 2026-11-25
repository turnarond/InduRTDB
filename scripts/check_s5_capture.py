#!/usr/bin/env python3
"""S5 取证 JSON 主机侧核验（v3.12）。

规则（见 docs/08-验证记录/v3.12-S5-capture-schema.md）：
  1. JSON 结构合法：mode/node_id/fields；每项字段含 value/_source/reason。
  2. _source ∈ {on-site, logic-only, unavailable}；unavailable 必须 value=null 且 reason 非空。
  3. P10 三字段 + P11 machine_restart_pull_full 必须为 unavailable（产品语义不足）。
  4. 台账回填仅接受 on-site；本脚本核验 JSON 标注，不声称硬防手改（手改由签名链约束）。

用法: check_s5_capture.py <json...>   |   check_s5_capture.py --self-test
"""
import json
import sys

FORBIDDEN_ON_SITE = {
    "nodes_converged", "terminal_hash", "dict_max_snapshot_match",
    "machine_restart_pull_full", "deny_non_loopback", "join_triggers_full",
    "rbe_gate_hits", "rbe_gate_misses", "snapshot_done_caps_only",
    "subs_restored", "epoch_before", "epoch_after",
    "partition_duration_s", "stale_epoch_rejected",
}
VALID_SRC = {"on-site", "logic-only", "unavailable"}


def check_capture(obj):
    errs = []
    if not isinstance(obj, dict):
        return ["top-level must be object"]
    if obj.get("mode") not in ("f2", "f3a", "f4"):
        errs.append("mode must be f2|f3a|f4")
    if not obj.get("node_id"):
        errs.append("node_id missing")
    fields = obj.get("fields")
    if not isinstance(fields, dict) or not fields:
        errs.append("fields must be non-empty object")
        return errs
    for key, ent in fields.items():
        if not isinstance(ent, dict):
            errs.append(f"{key}: entry must be object")
            continue
        src = ent.get("_source")
        if src not in VALID_SRC:
            errs.append(f"{key}: bad _source {src!r}")
            continue
        if src == "unavailable":
            if "value" not in ent or ent["value"] is not None:
                errs.append(f"{key}: unavailable must have value=null")
            if not ent.get("reason"):
                errs.append(f"{key}: unavailable missing reason")
        if key in FORBIDDEN_ON_SITE and src != "unavailable":
            errs.append(f"{key}: must be unavailable (product semantics insufficient)")
    return errs


def check_files(paths):
    total = 0
    for p in paths:
        try:
            with open(p, "r", encoding="utf-8") as f:
                obj = json.load(f)
        except (OSError, ValueError) as e:
            print(f"FAIL {p}: {e}")
            total += 1
            continue
        errs = check_capture(obj)
        if errs:
            print(f"FAIL {p}:")
            for e in errs:
                print(f"  - {e}")
            total += 1
        else:
            print(f"OK   {p} (mode={obj['mode']}, fields={len(obj['fields'])})")
    return total


def self_test():
    good = {
        "mode": "f2", "node_id": "192.168.30.220",
        "fields": {
            "epoch": {"value": 42, "_source": "on-site", "reason": ""},
            "terminal_hash": {"value": None, "_source": "unavailable",
                              "reason": "产品语义不足（S3不落shm）"},
        },
    }
    bad_unavailable_mapped = {
        "mode": "f2", "node_id": "x",
        "fields": {"terminal_hash": {"value": "fake", "_source": "on-site", "reason": ""}},
    }
    bad_logic_only_mapped = {
        "mode": "f2", "node_id": "x",
        "fields": {"nodes_converged": {"value": 6, "_source": "logic-only", "reason": "logic"}},
    }
    bad_null_no_reason = {
        "mode": "f3a", "node_id": "x",
        "fields": {"stale_epoch_rejected": {"value": None,
                                            "_source": "unavailable", "reason": ""}},
    }
    bad_non_loopback = {
        "mode": "f2", "node_id": "x",
        "fields": {"deny_non_loopback": {"value": 1, "_source": "on-site", "reason": ""}},
    }
    bad_f3a = {
        "mode": "f3a", "node_id": "x",
        "fields": {"partition_duration_s": {"value": 6, "_source": "on-site", "reason": ""}},
    }
    checks = [
        ("good sample", check_capture(good), True),
        ("unavailable mapped on-site", check_capture(bad_unavailable_mapped), False),
        ("logic-only mapped to P10", check_capture(bad_logic_only_mapped), False),
        ("null without reason", check_capture(bad_null_no_reason), False),
        ("deny_non_loopback mapped on-site", check_capture(bad_non_loopback), False),
        ("F3a duration mapped on-site", check_capture(bad_f3a), False),
    ]
    ok = True
    for name, errs, want_pass in checks:
        got_pass = not errs
        status = "OK" if got_pass == want_pass else "FAIL"
        if status == "FAIL":
            ok = False
        print(f"[{status}] {name}: {'pass' if got_pass else 'reject'} "
              f"(expected {'pass' if want_pass else 'reject'})")
        if errs:
            for e in errs:
                print(f"       - {e}")
    return 0 if ok else 1


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        return self_test()
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    return 1 if check_files(sys.argv[1:]) else 0


if __name__ == "__main__":
    sys.exit(main())
