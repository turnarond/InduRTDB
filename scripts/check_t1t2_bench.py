#!/usr/bin/env python3
"""T1 / T2 本机实测框架（方案 v2 B 项载体，CODEBUDDY.md:226 工具工程化）。

设计约束（依据 docs/03-设计文档/18-...-T1T2本机实测框架方案分析.md v2）：
  - ① local_core_shm：编译运行 tests/bench/bench.c 直连 libindurtdb.a，
    自解析表格 P99 数值（bench.c:308-324 列序：操作/迭代/P50/P99/P999/Min/Max/Mean），
    取核心操作 P99 最大值，判据文字**限定为「v3.9.0 核心库直连基线未退化的回归守卫」**，
    禁止出现「T2 达标 / T2 通过」。P99 ≤ 10.0 μs（冻结常量，doc 13:18/:78）为回归守卫。
  - ② uds_roundtrip：编译运行 tests/bench/uds_latency.c 取裸 AF_UNIX echo P99，
    仅作 T0 门槛参考、**不参与 10 μs 判据**，标注「T0 门槛参考，不参与 T2 判据」。
  - ③④ t2_real_uds_shm / vsoa_e2e 及 T2 真实判据 / T1 6 节点 sync 损耗：本机不可得，
    显式 status:"n/a" + reason + unlock:"M5" + ctest_name:null，不填占位数字、不声称 PASS。
  - 不注册 Gate_T1_SyncOverhead / Gate_T2_LocalP99 ctest 名；
    Gate_* 仅作 --json 内部标签 gate_label 出现且 ctest_name:null。
  - 数字只进 build/ 日志与 --json 输出，声明仅为回归证据（见 evidence_note）。

恒绿通道规避（评审 M2）：bench.c:511 无条件 return 0、run_bench.sh:98-106 只判 $RC，
现有「P99 全部 PASS」是打印文本非门禁；本工具判据来自**自解析的 P99 数值字段**。

用法::
    python3 scripts/check_t1t2_bench.py            # 正常模式，退出码即门禁
    python3 scripts/check_t1t2_bench.py --json     # 机器可解析四栏 + N/A
    python3 scripts/check_t1t2_bench.py --self-test  # 反证三类坏样本均被抓
    python3 scripts/check_t1t2_bench.py --root <repo>  # 指定仓库根（测试用）

退出码：0 = local_core_shm 回归守卫通过（P99 ≤ 阈值）且测量成功；非 0 = 守卫失败或测量失败。
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# ---- 冻结常量（doc 13:18/:78，禁实现者自改；上调须方案评审留痕）----
P99_THRESHOLD_US = 10.0
# ① 判据文字锁定（禁止出现「T2 达标 / T2 通过」）
LOCAL_CORE_SHM_GUARD_LABEL = "v3.9.0 核心库直连基线未退化的回归守卫"
# ② 口径标注
UDS_NOTE = "T0 门槛参考，不参与 T2 判据"
# N/A 项统一口径
NA_UNLOCK = "M5"
NA_REASON_T2 = "B 段，需真实 rtdbd 栈"
NA_REASON_VSOA = "VSOA 端到端需 ON 构建 + 真实栈"
NA_REASON_T2_REAL = "写后对端可见 p99 ≤100ms 需真实 rtdbd 栈"
NA_REASON_T1 = "本机无 6 节点编排 / sync 模式本机不可切换"

# 核心库直连 SHM 的回归守卫覆盖的核心操作（bench.c:370-375）
CORE_OPS = ["write_int32", "read_int32", "write_double", "read_double", "peek"]

# 快速模式迭代（与 run_bench.sh --quick 一致，控制门禁耗时）
BENCH_WARMUP = 5000
BENCH_ITERS = 50000
BENCH_BATCH = 20
BENCH_TP_SEC = 1
UDS_WARMUP = 5000
UDS_ITERS = 50000
UDS_MSG_SIZE = 16


def resolve_root(arg_root):
    if arg_root:
        return Path(arg_root).resolve()
    return Path(__file__).resolve().parent.parent


def compile_and_run(cc, src, out_path, cflags, defines, ldflags, timeout=600):
    """编译并运行一个 tests/bench 下的 C 基准，返回 (rc, stdout+stderr)。"""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [cc] + cflags.split() + defines.split() + [str(src)] + ldflags.split() + ["-o", str(out_path)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, f"TIMEOUT compiling/running {src}"
    except FileNotFoundError as exc:
        return 127, f"compiler not found: {exc}"
    if proc.returncode != 0:
        return proc.returncode, (proc.stdout or "") + (proc.stderr or "")
    # 运行
    try:
        run = subprocess.run([str(out_path)], capture_output=True, text=True, timeout=timeout)
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return 124, f"TIMEOUT/ERR running {out_path}"
    return run.returncode, (run.stdout or "") + (run.stderr or "")


def parse_bench_p99(text):
    """从 bench.c 输出的延迟分布表解析各操作 P99。

    行格式（bench.c:308-324）：
      | 操作 | 迭代数 | P50(us) | P99(us) | P999(us) | Min(us) | Max(us) | 吞吐(op/s) |
    数据行示例：
      | write_int32              |    50000 |    3.456 |    9.123 |    9.456 | ... |
    返回 {op: {"p50":..,"p99":..,"p999":..}}，仅保留延迟分布行（吞吐行 P50..Max 为 '-'）。
    """
    rows = {}
    for line in text.splitlines():
        s = line.strip()
        if not s.startswith("|"):
            continue
        parts = [p.strip() for p in s.strip("|").split("|")]
        if len(parts) != 8:
            continue
        name = parts[0]
        # 迭代列必须是整型（吞吐行也是整型迭代，但 P50..Max 为 '-'）
        try:
            int(parts[1])
        except ValueError:
            continue
        try:
            p50 = float(parts[2])
            p99 = float(parts[3])
            p999 = float(parts[4])
        except ValueError:
            # 吞吐行：P50..Max 为 '-'，跳过（非延迟分布行）
            continue
        rows[name] = {"p50": p50, "p99": p99, "p999": p999}
    return rows


def parse_uds_p99(text):
    """从 uds_latency.c 输出解析 P99（行：P99          %8.3f us）。"""
    m = re.search(r"P99\s+([0-9]+\.[0-9]+)\s*us", text)
    if not m:
        return None
    return float(m.group(1))


def measure(root):
    """编译并运行 bench.c + uds_latency.c，返回 (bench_rows, uds_p99, errors)。"""
    repo = root
    build_dir = repo / "build"
    bench_dir = repo / "tests/bench"
    errors = []
    cc = os.environ.get("CC", "gcc")

    # ---- ① bench.c 直连 libindurtdb.a ----
    bench_src = bench_dir / "bench.c"
    bench_out = build_dir / "bench" / "bench"
    bench_cflags = (f"-std=c11 -Wall -Wextra -Werror -O2 -I{repo / 'include'} "
                    "-DINDURTDB_NO_DEPRECATE_WARN")
    bench_defines = (f"-DBENCH_WARMUP={BENCH_WARMUP} -DBENCH_ITERATIONS={BENCH_ITERS} "
                     f"-DBENCH_BATCH_COUNT={BENCH_BATCH} -DBENCH_THROUGHPUT_SEC={BENCH_TP_SEC}")
    bench_ldflags = f"-L{build_dir} -lindurtdb -lpthread -lrt -lm"
    rc, out = compile_and_run(cc, bench_src, bench_out, bench_cflags, bench_defines, bench_ldflags)
    bench_rows = {}
    if rc != 0:
        errors.append(f"bench.c 编译/运行失败(rc={rc}): {out[-500:]}")
    else:
        bench_rows = parse_bench_p99(out)
        if not bench_rows:
            errors.append("bench.c 输出未解析到任何延迟分布行")

    # ---- ② uds_latency.c 裸 AF_UNIX echo（T0 参考，非门禁）----
    uds_src = bench_dir / "uds_latency.c"
    uds_out = build_dir / "bench" / "uds_latency"
    uds_cflags = (f"-std=gnu11 -Wall -Wextra -Werror -O2 -I{repo / 'include'} "
                  "-DINDURTDB_NO_DEPRECATE_WARN")
    uds_defines = (f"-DBENCH_WARMUP={UDS_WARMUP} -DBENCH_ITERATIONS={UDS_ITERS} "
                   f"-DBENCH_MSG_SIZE={UDS_MSG_SIZE}")
    rc2, out2 = compile_and_run(cc, uds_src, uds_out, uds_cflags, uds_defines, "")
    uds_p99 = None
    if rc2 != 0:
        errors.append(f"uds_latency.c 编译/运行失败(rc={rc2}): {out2[-500:]} (T0 参考，不阻断门禁)")
    else:
        uds_p99 = parse_uds_p99(out2)
        if uds_p99 is None:
            errors.append("uds_latency.c 输出未解析到 P99 (T0 参考，不阻断门禁)")

    return bench_rows, uds_p99, errors


def build_report(bench_rows, uds_p99, threshold=P99_THRESHOLD_US):
    """构造 --json 报告；返回 (report_dict, guard_pass)。

    guard_pass=True 当且仅当：① local_core_shm 各核心操作 P99 ≤ 阈值 且 测量成功，
    且 N/A 一致性校验通过（无 N/A 项被填成数字/PASS）。uds_roundtrip 不参与判据。
    """
    # ① local_core_shm
    core = {op: (bench_rows.get(op) or {}).get("p99") for op in CORE_OPS}
    missing = [op for op, v in core.items() if v is None]
    if missing:
        local_status = "fail"
        local_pass = False
        local_p99_max = None
    else:
        local_p99_max = max(v for v in core.values() if v is not None)
        local_pass = local_p99_max <= threshold
        local_status = "pass" if local_pass else "fail"

    report = {
        "tool": "check_t1t2_bench.py",
        "schema": 1,
        "threshold_us": threshold,
        "guard_label_local_core_shm": LOCAL_CORE_SHM_GUARD_LABEL,
        "evidence_note": ("build/ 日志与 --json 数字仅为回归证据，不构成 doc13:85 数字落库、"
                          "不作 doc13:365 交付证据、不得回填 README:31、不得置 B 段 measured"),
        "columns": {
            "local_core_shm": {
                "desc": "① 核心库直连 SHM（bench.c 直连 libindurtdb.a），取实际 P99 数值",
                "gate_label": "Gate_T2_LocalP99 (内部标签，非 ctest)",
                "ctest_name": None,
                "status": local_status,
                "p99_us": core,
                "p99_max_us": local_p99_max,
                "threshold_us": threshold,
                "verdict": LOCAL_CORE_SHM_GUARD_LABEL,
            },
            "uds_roundtrip": {
                "desc": "② 裸 AF_UNIX echo（uds_latency.c，不经 rtdbd），T0 门槛参考",
                "gate_label": None,
                "ctest_name": None,
                "status": ("measured" if uds_p99 is not None else "error"),
                "p99_us": uds_p99,
                "note": UDS_NOTE,
            },
            "t2_real_uds_shm": {
                "desc": "③ 真实 rtdbd 收包 + dispatch + Seqlock 写 SHM + 回包",
                "gate_label": None,
                "ctest_name": None,
                "status": "n/a",
                "reason": NA_REASON_T2,
                "unlock": NA_UNLOCK,
            },
            "vsoa_e2e": {
                "desc": "④ VSOA 前端端到端（需 ON 构建 + 真实栈）",
                "gate_label": None,
                "ctest_name": None,
                "status": "n/a",
                "reason": NA_REASON_VSOA,
                "unlock": NA_UNLOCK,
            },
        },
        "criteria": {
            "t2_real": {
                "desc": "T2 真实判据（写后对端可见 p99 ≤100ms）",
                "gate_label": None,
                "ctest_name": None,
                "status": "n/a",
                "reason": NA_REASON_T2_REAL,
                "unlock": NA_UNLOCK,
            },
            "t1_sync_overhead": {
                "desc": "T1 6 节点 sync TPS 损耗 ≤5%（doc 13:77，需 10 万点/1 万 ops/s/6 节点）",
                "gate_label": "Gate_T1_SyncOverhead (内部标签，非 ctest)",
                "ctest_name": None,
                "status": "n/a",
                "reason": NA_REASON_T1,
                "unlock": NA_UNLOCK,
            },
        },
    }

    # N/A 一致性校验：本应 n/a 的项不得被填成数字/PASS（防假绿回填）
    na_consistent = _validate_no_fake_na(report)
    guard_pass = bool(local_pass) and na_consistent
    report["guard_pass"] = guard_pass
    return report, guard_pass


def _na_items(report):
    """返回所有应固定为 n/a 的项（③④ 与 T2 真实判据 / T1）。"""
    items = []
    items.append(report["columns"]["t2_real_uds_shm"])
    items.append(report["columns"]["vsoa_e2e"])
    items.append(report["criteria"]["t2_real"])
    items.append(report["criteria"]["t1_sync_overhead"])
    return items


def _validate_no_fake_na(report):
    """N/A 项若被填成数字 / 标 PASS ⇒ 返回 False（工具应 FAIL）。

    这是反假绿兜底：若有人篡改源码把 N/A 项填成数字或 status=pass，
    工具必须失败而非恒绿。
    """
    for item in _na_items(report):
        if item.get("status") != "n/a":
            return False
        # status 为 n/a 时不得携带实质性测量数字
        if item.get("p99_us") is not None:
            return False
    return True


def render_human(report, errors):
    lines = []
    lines.append("=== T1/T2 本机实测框架（方案 B 载体）===")
    lines.append(f"阈值（冻结常量，doc 13:18/:78）：P99 ≤ {report['threshold_us']} μs")
    lines.append(f"① local_core_shm 判据：{LOCAL_CORE_SHM_GUARD_LABEL}")
    loc = report["columns"]["local_core_shm"]
    p99_max_str = f"{loc['p99_max_us']} μs" if loc['p99_max_us'] is not None else "N/A"
    lines.append(f"  [{'OK' if loc['status'] == 'pass' else 'FAIL'}] local_core_shm: "
                 f"status={loc['status']} p99_max={p99_max_str} (阈值 {loc['threshold_us']})")
    for op, v in (loc["p99_us"] or {}).items():
        lines.append(f"      {op}: P99={v} μs")
    uds = report["columns"]["uds_roundtrip"]
    lines.append(f"  [INFO] uds_roundtrip: status={uds['status']} p99={uds['p99_us']} μs —— {UDS_NOTE}")
    for key in ("t2_real_uds_shm", "vsoa_e2e"):
        it = report["columns"][key]
        lines.append(f"  [N/A ] {key}: {it['reason']}（unlock={it['unlock']}）")
    lines.append(f"  [N/A ] T2 真实判据：{report['criteria']['t2_real']['reason']}（unlock={NA_UNLOCK}）")
    lines.append(f"  [N/A ] T1 6 节点 sync 损耗：{report['criteria']['t1_sync_overhead']['reason']}"
                 f"（unlock={NA_UNLOCK}）")
    if errors:
        lines.append("测量告警/错误：")
        for e in errors:
            lines.append(f"  - {e}")
    lines.append(f"守卫结论：{'PASS' if report['guard_pass'] else 'FAIL'}")
    return "\n".join(lines)


def self_test():
    """反证三类坏样本均被工具抓出（评审 M2 / v2 §6 判据 2/3）。

    1) 篡改 Stage 输入值：阈值改小（≤0.001μs），本机真实 ~9μs 应判 fail。
    2) 篡改测量源：bench 产出 >10μs 样本（注入 p99=99.0），应判 fail。
    3) N/A 项被填数字 / PASS：构造被填的报告，_validate_no_fake_na 应 False。
    """
    results = []

    def check(name, ok):
        results.append((name, ok))
        print(f"  [{'OK' if ok else 'FAIL'}] self-test: {name}")

    # 1) 阈值篡改
    _, pass_small = build_report(
        {op: {"p50": 1.0, "p99": 9.5, "p999": 10.0} for op in CORE_OPS},
        uds_p99=18.2, threshold=0.001)
    check("篡改阈值≤0.001μs（真实~9μs）⇒ 守卫 FAIL", pass_small is False)

    # 2) 测量源注入 >10μs
    _, pass_big = build_report(
        {op: {"p50": 50.0, "p99": 99.0, "p999": 120.0} for op in CORE_OPS},
        uds_p99=18.2, threshold=P99_THRESHOLD_US)
    check("测量源注入 P99=99μs ⇒ 守卫 FAIL", pass_big is False)

    # 2b) 正常样本应通过（sanity：守卫不是恒红）
    # 用固定 10.0（非全局阈值），确保阈值常量被外部篡改时不影响本 sanity 自证，
    # 失败应落到正常模式的 bench_t1t2 Stage，而非 self-test 链本身。
    _, pass_normal = build_report(
        {op: {"p50": 1.0, "p99": 3.0, "p999": 5.0} for op in CORE_OPS},
        uds_p99=18.2, threshold=10.0)
    check("正常样本 P99=3μs ⇒ 守卫 PASS", pass_normal is True)

    # 3) N/A 项被填数字 / PASS ⇒ 工具 FAIL
    rep, _ = build_report(
        {op: {"p50": 1.0, "p99": 3.0, "p999": 5.0} for op in CORE_OPS},
        uds_p99=18.2)
    rep["columns"]["t2_real_uds_shm"]["status"] = "pass"
    rep["columns"]["t2_real_uds_shm"]["p99_us"] = 5.0
    check("N/A 项(t2_real_uds_shm)被填数字/PASS ⇒ 校验 FAIL",
          _validate_no_fake_na(rep) is False)

    rep2, _ = build_report(
        {op: {"p50": 1.0, "p99": 3.0, "p999": 5.0} for op in CORE_OPS},
        uds_p99=18.2)
    rep2["criteria"]["t1_sync_overhead"]["status"] = "pass"
    check("N/A 项(t1_sync_overhead)被标 PASS ⇒ 校验 FAIL",
          _validate_no_fake_na(rep2) is False)

    ok = all(ok for _, ok in results)
    if not ok:
        print("FAIL: self-test 未通过（守卫失活）", file=sys.stderr)
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true", help="输出机器可解析四栏 + N/A")
    ap.add_argument("--self-test", action="store_true", help="反证三类坏样本均被抓")
    ap.add_argument("--root", default=None, help="仓库根目录（测试/CI 用，默认脚本上级）")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    root = resolve_root(args.root)
    bench_rows, uds_p99, errors = measure(root)
    report, guard_pass = build_report(bench_rows, uds_p99)

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(render_human(report, errors))

    # 测量失败（bench.c 无法编译/运行）也视为守卫失败，退出非 0
    if errors and not bench_rows:
        return 1
    return 0 if guard_pass else 1


if __name__ == "__main__":
    sys.exit(main())
