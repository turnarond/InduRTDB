#!/usr/bin/env python3
"""冻结设计/变更卡基线：记录内容哈希、时间与捕获时刻；只允许创建一次。"""
import argparse
import hashlib
import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

SCHEMA = 1


def file_meta(path, base):
    p = Path(path).resolve()
    st = p.stat()
    birth = getattr(st, "st_birthtime", None)
    try:
        rel = os.path.relpath(str(p), str(Path(base).resolve().parent))
    except ValueError:
        rel = str(p)
    return {
        "path": rel,
        "md5": hashlib.md5(p.read_bytes()).hexdigest(),
        "size": st.st_size,
        "mtimeNs": st.st_mtime_ns,
        "birthNs": int(birth * 1_000_000_000) if birth is not None else None,
    }


def freeze(files, baseline):
    b = Path(baseline)
    findings = []
    if b.exists():
        return {"ok": False, "baseline": str(b),
                "findings": [{"level": "FAIL", "field": "baseline",
                              "message": "already frozen; use a new baseline path for a new phase"}]}
    artifacts = []
    for raw in files:
        p = Path(raw)
        if not p.is_file():
            findings.append({"level": "FAIL", "field": "file",
                             "message": f"not found: {raw}"})
            continue
        artifacts.append(file_meta(p, baseline))
    if findings:
        return {"ok": False, "baseline": str(b), "findings": findings}
    if not artifacts:
        return {"ok": False, "baseline": str(b),
                "findings": [{"level": "FAIL", "field": "files",
                              "message": "at least one file is required"}]}
    captured_ns = time.time_ns()
    data = {
        "schema": SCHEMA,
        "capturedAt": datetime.fromtimestamp(captured_ns / 1e9, timezone.utc).isoformat(),
        "capturedAtNs": captured_ns,
        "artifacts": artifacts,
    }
    b.parent.mkdir(parents=True, exist_ok=True)
    b.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n",
                 encoding="utf-8")
    return {"ok": True, "baseline": str(b), "capturedAt": data["capturedAt"],
            "capturedAtNs": captured_ns,
            "artifacts": [{"path": a["path"], "md5": a["md5"]} for a in artifacts],
            "findings": []}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*")
    ap.add_argument("--baseline", required=False)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_checkers import main as run_self_tests
        return run_self_tests()
    if not args.baseline:
        ap.error("--baseline is required")
    result = freeze(args.files, args.baseline)
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        print(("OK: " if result["ok"] else "FAIL: ") + args.baseline)
        for f in result["findings"]:
            print(f"  [{f['level']}] {f['field']}: {f['message']}")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
