#!/usr/bin/env python3
"""版本号四处一致性守卫（工程规则 5：版本号不得单点漂移）。

口径（与 CODEBUDDY.md 一致）：
  ``VERSION`` / ``CMakeLists.txt`` 的 ``project(VERSION)`` /
  ``include/indurtdb/indurtdb.h`` 版本宏 / ``CHANGELOG.md`` 对应版本条目。

README、docs 允许出现历史版本号，只做存在性检查（不含即 WARN，不 FAIL）。

用法::

    python3 scripts/check_version_sync.py
    python3 scripts/check_version_sync.py --json
    python3 scripts/check_version_sync.py --self-test   # 判据自证，抗假绿

退出码：0 = 一致；1 = 有 FAIL；2 = 用法错误。
"""
import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SEMVER = r"\d+\.\d+\.\d+"


def _read(path):
    return Path(path).read_text(encoding="utf-8", errors="replace")


def check(root):
    root = Path(root)
    findings = []
    values = {}

    version_file = root / "VERSION"
    if not version_file.is_file():
        findings.append({"level": "FAIL", "field": "VERSION", "message": "文件不存在"})
        version = ""
    else:
        version = _read(version_file).strip().splitlines()[0].strip() if _read(version_file).strip() else ""
        if not re.fullmatch(SEMVER, version):
            findings.append({"level": "FAIL", "field": "VERSION",
                             "message": f"不是 x.y.z 形式：{version!r}"})
    values["VERSION"] = version

    cmake = root / "CMakeLists.txt"
    cmake_version = ""
    if not cmake.is_file():
        findings.append({"level": "FAIL", "field": "CMakeLists.txt", "message": "文件不存在"})
    else:
        m = re.search(r"project\s*\([^)]*?VERSION\s+(" + SEMVER + r")", _read(cmake), re.S)
        cmake_version = m.group(1) if m else ""
        if not cmake_version:
            findings.append({"level": "FAIL", "field": "CMakeLists.txt",
                             "message": "未找到 project(... VERSION x.y.z)"})
    values["CMakeLists.txt"] = cmake_version

    header = root / "include/indurtdb/indurtdb.h"
    header_version = ""
    if not header.is_file():
        findings.append({"level": "FAIL", "field": "indurtdb.h", "message": "文件不存在"})
    else:
        text = _read(header)
        m = re.search(r'INDURTDB_VERSION_STRING\s+"(' + SEMVER + r')"', text)
        header_version = m.group(1) if m else ""
        parts = {}
        for name in ("MAJOR", "MINOR", "PATCH"):
            mm = re.search(r"#define\s+INDURTDB_VERSION_" + name + r"\s+(\d+)", text)
            parts[name] = mm.group(1) if mm else ""
        composed = ".".join(parts[k] for k in ("MAJOR", "MINOR", "PATCH"))
        if not header_version:
            findings.append({"level": "FAIL", "field": "indurtdb.h",
                             "message": "未找到 INDURTDB_VERSION_STRING"})
        elif composed != header_version:
            findings.append({"level": "FAIL", "field": "indurtdb.h",
                             "message": f"_STRING={header_version} 与 MAJOR/MINOR/PATCH={composed} 不一致"})
    values["indurtdb.h"] = header_version

    for field, value in (("CMakeLists.txt", cmake_version), ("indurtdb.h", header_version)):
        if value and version and value != version:
            findings.append({"level": "FAIL", "field": field,
                             "message": f"{value} != VERSION {version}"})

    changelog = root / "CHANGELOG.md"
    if not changelog.is_file():
        findings.append({"level": "FAIL", "field": "CHANGELOG.md", "message": "文件不存在"})
    elif version:
        entries = re.findall(r"^##\s*\[(" + SEMVER + r")\]", _read(changelog), re.M)
        values["CHANGELOG.md"] = entries[0] if entries else ""
        if version not in entries:
            findings.append({"level": "FAIL", "field": "CHANGELOG.md",
                             "message": f"缺少 [{version}] 条目（最新条目：{entries[0] if entries else '无'}）"})
        elif entries and entries[0] != version:
            findings.append({"level": "WARN", "field": "CHANGELOG.md",
                             "message": f"最新条目 {entries[0]} 领先 VERSION {version}（开发中可接受，发布前须对齐）"})

    readme = root / "README.md"
    if readme.is_file() and version:
        if f"v{version}" not in _read(readme) and version not in _read(readme):
            findings.append({"level": "WARN", "field": "README.md",
                             "message": f"未出现版本号 {version}"})

    ok = not any(f["level"] == "FAIL" for f in findings)
    return {"ok": ok, "version": version, "where": values, "findings": findings}


def self_test():
    """判据自证：一致样本必须 PASS，单点漂移必须 FAIL。"""
    cases = []

    def make(root, version, cmake, header, changelog_entries):
        (root / "include/indurtdb").mkdir(parents=True, exist_ok=True)
        (root / "VERSION").write_text(version + "\n", encoding="utf-8")
        (root / "CMakeLists.txt").write_text(
            f"project(InduRTDB VERSION {cmake} LANGUAGES C)\n", encoding="utf-8")
        major, minor, patch = header.split(".")
        (root / "include/indurtdb/indurtdb.h").write_text(
            f'#define INDURTDB_VERSION_MAJOR {major}\n'
            f'#define INDURTDB_VERSION_MINOR {minor}\n'
            f'#define INDURTDB_VERSION_PATCH {patch}\n'
            f'#define INDURTDB_VERSION_STRING "{header}"\n', encoding="utf-8")
        (root / "CHANGELOG.md").write_text(
            "".join(f"## [{v}]\n" for v in changelog_entries), encoding="utf-8")
        (root / "README.md").write_text(f"当前 v{version}\n", encoding="utf-8")

    with tempfile.TemporaryDirectory() as td:
        good = Path(td) / "good"
        good.mkdir()
        make(good, "1.2.3", "1.2.3", "1.2.3", ["1.2.3"])
        cases.append(("一致样本应 PASS", check(good)["ok"] is True))

        bad_cmake = Path(td) / "bad_cmake"
        bad_cmake.mkdir()
        make(bad_cmake, "1.2.3", "1.2.4", "1.2.3", ["1.2.3"])
        cases.append(("CMake 漂移应 FAIL", check(bad_cmake)["ok"] is False))

        bad_header = Path(td) / "bad_header"
        bad_header.mkdir()
        make(bad_header, "1.2.3", "1.2.3", "1.2.3", ["1.2.3"])
        h = bad_header / "include/indurtdb/indurtdb.h"
        h.write_text(h.read_text(encoding="utf-8").replace("_PATCH 3", "_PATCH 9"),
                     encoding="utf-8")
        cases.append(("头文件宏自相矛盾应 FAIL", check(bad_header)["ok"] is False))

        bad_log = Path(td) / "bad_log"
        bad_log.mkdir()
        make(bad_log, "1.2.3", "1.2.3", "1.2.3", ["1.2.2"])
        cases.append(("CHANGELOG 缺条目应 FAIL", check(bad_log)["ok"] is False))

    failed = [name for name, ok in cases if not ok]
    for name, ok in cases:
        print(f"  [{'OK' if ok else 'FAIL'}] {name}")
    if failed:
        print(f"FAIL: 自证未通过 {len(failed)} 项", file=sys.stderr)
        return 1
    print(f"OK: 自证通过 {len(cases)} 项")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    result = check(args.root)
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        print(f"版本基线：{result['version']}")
        for field, value in result["where"].items():
            print(f"  - {field}: {value or '未取到'}")
        for f in result["findings"]:
            print(f"  [{f['level']}] {f['field']}: {f['message']}")
        print("OK: 版本号一致" if result["ok"] else "FAIL: 版本号不一致")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
