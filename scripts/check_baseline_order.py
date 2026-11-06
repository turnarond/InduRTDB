#!/usr/bin/env python3
"""校验“基线先于实现”：冻结后必须出现实现文件，且基线文件时间不晚于首个实现。"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

DEFAULT_EXCLUDES = {
    ".git", ".hg", ".svn", "node_modules", "__pycache__", ".venv", "venv",
    ".artifacts", ".probe", "dist", "build", "bin", "obj",
}
IMPLEMENTATION_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".vue", ".svelte",
    ".css", ".scss", ".sass", ".less", ".html", ".htm",
    ".py", ".sh", ".bash", ".rs", ".go", ".java", ".kt", ".swift", ".m", ".mm",
    ".json", ".yaml", ".yml", ".toml", ".sql", ".cmake", ".mk",
}


def file_md5(path):
    return hashlib.md5(Path(path).read_bytes()).hexdigest()


def iter_sources(sources, excludes):
    for raw in sources:
        root = Path(raw)
        if not root.exists():
            continue
        if root.is_file():
            yield root
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames
                           if d not in excludes and not d.startswith("build")]
            for name in filenames:
                p = Path(dirpath) / name
                if p.is_file():
                    yield p


def check(baseline, sources, excludes=None):
    findings = []
    b = Path(baseline)
    if not b.is_file():
        return {"ok": False, "baseline": str(b),
                "findings": [{"level": "FAIL", "field": "baseline",
                              "message": f"not found: {baseline}"}]}
    try:
        data = json.loads(b.read_text(encoding="utf-8"))
    except Exception as exc:
        return {"ok": False, "baseline": str(b),
                "findings": [{"level": "FAIL", "field": "baseline",
                              "message": f"invalid json: {exc}"}]}
    if not isinstance(data, dict):
        return {"ok": False, "baseline": str(b),
                "findings": [{"level": "FAIL", "field": "baseline",
                              "message": "baseline must be a JSON object"}]}
    captured_ns = data.get("capturedAtNs")
    artifacts = data.get("artifacts") or []
    if not isinstance(artifacts, list):
        return {"ok": False, "baseline": str(b),
                "findings": [{"level": "FAIL", "field": "artifacts",
                              "message": "artifacts must be a list"}]}
    if not isinstance(captured_ns, int):
        findings.append({"level": "FAIL", "field": "capturedAtNs",
                         "message": "missing or invalid"})
    if not artifacts:
        findings.append({"level": "FAIL", "field": "artifacts",
                         "message": "baseline contains no artifacts"})

    artifact_mtime = 0
    for a in artifacts:
        if not isinstance(a, dict):
            findings.append({"level": "FAIL", "field": "artifact",
                             "message": "artifact entry must be an object"})
            continue
        p = Path(a.get("path", ""))
        if not p.is_absolute():
            p = b.parent / p
        if not p.is_file():
            findings.append({"level": "FAIL", "field": "artifact",
                             "message": f"missing artifact: {p}"})
            continue
        artifact_mtime = max(artifact_mtime, int(a.get("mtimeNs") or 0))
        if a.get("md5") and file_md5(p) != a["md5"]:
            findings.append({"level": "WARN", "field": "artifact",
                             "message": f"artifact changed after freeze: {p}"})

    exclude_set = set(DEFAULT_EXCLUDES)
    exclude_set.update(excludes or [])
    files = list(iter_sources(sources, exclude_set))
    base_resolved = b.resolve()
    changed = []
    if isinstance(captured_ns, int):
        for p in files:
            if p.resolve() == base_resolved:
                continue
            if p.suffix.lower() not in IMPLEMENTATION_SUFFIXES:
                continue
            try:
                mt = p.stat().st_mtime_ns
            except OSError:
                continue
            if mt >= captured_ns:
                changed.append((p, mt))
    if not changed:
        findings.append({"level": "FAIL", "field": "source",
                         "message": "no implementation file was written after the baseline"})
        earliest = None
    else:
        earliest, earliest_mtime = min(changed, key=lambda item: item[1])
        if artifact_mtime > earliest_mtime:
            findings.append({"level": "FAIL", "field": "order",
                             "message": "baseline artifact is newer than the first implementation file"})

    ok = not any(f["level"] == "FAIL" for f in findings)
    return {
        "ok": ok,
        "baseline": str(b),
        "capturedAt": data.get("capturedAt"),
        "capturedAtNs": captured_ns,
        "artifactMtimeNs": artifact_mtime,
        "earliestSource": str(earliest) if earliest else None,
        "earliestSourceMtimeNs": earliest_mtime if earliest else None,
        "findings": findings,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=False)
    ap.add_argument("--source", action="append", nargs="+", default=[])
    ap.add_argument("--exclude", action="append", nargs="+", default=[])
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_checkers import main as run_self_tests
        return run_self_tests()
    if not args.baseline:
        ap.error("--baseline is required")
    if not args.source:
        ap.error("--source is required")
    sources = [x for group in args.source for x in group]
    excludes = [x for group in args.exclude for x in group]
    result = check(args.baseline, sources, excludes)
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        print(("OK: " if result["ok"] else "FAIL: ") + args.baseline)
        for f in result["findings"]:
            print(f"  [{f['level']}] {f['field']}: {f['message']}")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
