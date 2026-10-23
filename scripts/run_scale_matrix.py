#!/usr/bin/env python3
"""v3.11 规模矩阵：2k / 20k / 200k / 500k / 1M 点位 × 1/5/10 Hz。

每档启动独立 rtdbd，复用 `run_station_load.py` 的写/订阅负载模型（10 个写进程），
额外用 BATCH_READ 与逐点 GET 对照，输出 JSON/CSV 与每档原始日志。脚本不会伪造
结果：负载未达标只记录失败原因，矩阵继续跑下一档。

用法：
  python3 scripts/run_scale_matrix.py --out /tmp/v311-matrix --runs 1 --quick
  python3 scripts/run_scale_matrix.py --points 20000 --hz 1 --runs 3 --duration 600
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
TOOL = REPO / "tools" / "rtdb-monitor"
sys.path.insert(0, str(TOOL))
sys.path.insert(0, str(HERE))

from rtdb_client import RtdbClient, TYPE_INT32, probe_caps  # noqa: E402
from check_rtdbd_health import fetch as fetch_health  # noqa: E402
from run_station_load import _proc_stat  # noqa: E402

DEFAULT_POINTS = (2_000, 20_000, 200_000, 500_000, 1_000_000)
DEFAULT_HZ = (1, 5, 10)


def _write_config(path: Path, points: int) -> None:
    with path.open("w", encoding="utf-8") as f:
        f.write("points:\n")
        for pid in range(points):
            f.write(
                f'  - id: {pid}\n    name: "Scale_{pid}"\n'
                f"    type: int32\n    access: 3\n"
            )


def _write_policy(path: Path, points: int) -> None:
    path.write_text(f"{os.getuid()}:*:0:{points - 1}\n", encoding="utf-8")


def _build(build_dir: Path, no_build: bool) -> Path:
    rtdbd = build_dir / "rtdbd" / "rtdbd"
    if no_build:
        if not rtdbd.is_file():
            raise SystemExit(f"缺少 {rtdbd}（去掉 --no-build 先构建）")
        return rtdbd
    if not (build_dir / "CMakeCache.txt").is_file():
        subprocess.run(
            ["cmake", "-S", str(REPO), "-B", str(build_dir),
             "-DCMAKE_BUILD_TYPE=Release"],
            check=True,
        )
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", "rtdbd",
         "-j", str(os.cpu_count() or 1)],
        check=True,
    )
    if not rtdbd.is_file():
        raise SystemExit(f"构建后仍缺少 {rtdbd}")
    return rtdbd


def _start_rtdbd(rtdbd: Path, work: Path, points: int) -> tuple[subprocess.Popen, Path, Path, Path]:
    sock = work / "rtdbd.sock"
    cfg = work / "points.yaml"
    pol = work / "policy.conf"
    log = work / "rtdbd.log"
    stats = work / "stats.txt"
    _write_config(cfg, points)
    _write_policy(pol, points)
    instance = f"scale_{os.getpid()}_{points}"
    shm = Path("/dev/shm") / f"indurtdb_{instance}"
    try:
        shm.unlink()
    except FileNotFoundError:
        pass
    cmd = [
        str(rtdbd), "--instance", instance, "--socket", str(sock),
        "--config", str(cfg), "--policy", str(pol),
        "--max-points", str(points), "--max-clients", "128",
        "--initial-clients", "32", "--max-subs", "10000",
        "--initial-subs", "1024", "--max-batch", "1024",
        "--stats-file", str(stats), "--timeout-ms", "0",
    ]
    logf = log.open("wb")
    try:
        proc = subprocess.Popen(cmd, stdout=logf, stderr=subprocess.STDOUT)
    finally:
        logf.close()
    for _ in range(300):
        if sock.is_socket():
            return proc, sock, log, stats
        if proc.poll() is not None:
            raise RuntimeError(f"rtdbd 启动失败 rc={proc.returncode}: {log.read_text(errors='replace')[:2000]}")
        time.sleep(0.1)
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
    try:
        shm.unlink()
    except FileNotFoundError:
        pass
    raise RuntimeError("rtdbd socket 等待超时")


def _dump_stats(pid: int, path: Path, timeout: float = 2.0) -> dict:
    """SIGUSR1 写 stats 文件；读取 key=value，失败返回空字典。"""
    try:
        os.kill(pid, signal.SIGUSR1)
    except ProcessLookupError:
        return {}
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            text = path.read_text(encoding="utf-8")
            if "n_batch_read_reqs" in text:
                out = {}
                for line in text.splitlines():
                    if "=" in line and not line.startswith("#"):
                        k, _, v = line.partition("=")
                        try:
                            out[k.strip()] = int(v.strip())
                        except ValueError:
                            pass
                return out
        except OSError:
            pass
        time.sleep(0.05)
    return {}


def _stop_rtdbd(proc: subprocess.Popen, instance: str) -> None:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    try:
        (Path("/dev/shm") / f"indurtdb_{instance}").unlink()
    except FileNotFoundError:
        pass


def _profile(points: int, fanout: bool) -> tuple[int, int, str]:
    """返回 (clients, subs/client, profile_name)。"""
    if fanout:
        return min(128, 100), min(10_000, points), "fanout_100x10k"
    target = max(1, int(points * 0.35))
    clients = max(1, min(128, (target + 9_999) // 10_000))
    subs = max(1, min(10_000, target // clients, points))
    return clients, subs, "deployment_0.35x"


def _batch_benchmark(sock: Path, ids: list[int], timeout: float) -> dict:
    result = {
        "batch_read_ops_s": 0.0,
        "batch_write_ops_s": 0.0,
        "get_ops_s": 0.0,
        "batch_speedup": 0.0,
        "batch_size": 0,
        "batch_caps": False,
        "batch_error": None,
    }
    try:
        with RtdbClient(str(sock), timeout=timeout) as c:
            caps = probe_caps(c)
            result["batch_caps"] = caps is not None
            if caps is None or not (caps.get("flags", 0) & 1):
                result["batch_error"] = "CAPS 未协商或未声明 BATCH_READ"
                return result
            batch_size = max(1, min(int(c.max_batch), 1024, len(ids)))
            sample = ids[: min(len(ids), 10_000)]
            result["batch_size"] = batch_size
            chunks = [sample[i:i + batch_size] for i in range(0, len(sample), batch_size)]
            t0 = time.perf_counter()
            for chunk in chunks:
                c.batch_read(chunk)
            dt = max(1e-9, time.perf_counter() - t0)
            result["batch_read_ops_s"] = len(sample) / dt

            serial = sample[: min(200, len(sample))]
            t0 = time.perf_counter()
            for pid in serial:
                c.get_point(pid)
            dt = max(1e-9, time.perf_counter() - t0)
            result["get_ops_s"] = len(serial) / dt
            if result["get_ops_s"] > 0:
                result["batch_speedup"] = result["batch_read_ops_s"] / result["get_ops_s"]

            # 批量写对照：与读使用同一批 id，计数写入 n_batch_write_reqs。
            writes = [(pid, TYPE_INT32, 1) for pid in sample]
            t0 = time.perf_counter()
            for i in range(0, len(writes), batch_size):
                c.batch_write(writes[i:i + batch_size])
            dt = max(1e-9, time.perf_counter() - t0)
            result["batch_write_ops_s"] = len(writes) / dt
    except Exception as e:  # noqa: BLE001
        result["batch_error"] = f"{type(e).__name__}: {e}"
    return result


def _run_once(rtdbd: Path, points: int, hz: int, duration: float,
              timeout: float, fanout: bool, log_path: Path) -> dict:
    clients, subs, profile = _profile(points, fanout)
    work = Path(tempfile.mkdtemp(prefix=f"indurtdb-scale-{points}-"))
    instance = f"scale_{os.getpid()}_{points}"
    rec: dict = {
        "points": points,
        "hz": hz,
        "write_rate": points * hz,
        "profile": profile,
        "clients": clients,
        "subs_per_client": subs,
        "duration_s": duration,
        "ok": False,
        "error": None,
    }
    proc: Optional[subprocess.Popen] = None
    logf = None
    try:
        proc, sock, rtdbd_log, stats_path = _start_rtdbd(rtdbd, work, points)
        logf = log_path.open("w", encoding="utf-8")
        logf.write(f"# rtdbd log: {rtdbd_log}\n")
        health_before = fetch_health(str(sock), timeout)
        cpu_before = _proc_stat(proc.pid)

        cmd = [
            sys.executable, str(HERE / "run_station_load.py"),
            "--socket", str(sock), "--pid", str(proc.pid),
            "--points", str(points), "--clients", str(clients),
            "--subs", str(subs), "--duration", str(duration),
            "--write-rate", str(points * hz), "--workers", "10",
            "--timeout", str(timeout),
        ]
        t0 = time.perf_counter()
        cp = subprocess.run(cmd, text=True, capture_output=True, timeout=duration + 180)
        rec["elapsed_s"] = time.perf_counter() - t0
        rec["load_rc"] = cp.returncode
        logf.write("## command\n" + " ".join(cmd) + "\n")
        logf.write("## stdout\n" + cp.stdout + "\n## stderr\n" + cp.stderr + "\n")
        rec["ok"] = cp.returncode == 0

        # 先跑批量读对照，再 dump stats，确保 n_batch_read_reqs 计入本档。
        rec.update(_batch_benchmark(sock, list(range(points)), timeout))
        health_after = fetch_health(str(sock), timeout) if proc.poll() is None else health_before
        cpu_after = _proc_stat(proc.pid)
        stats_after = _dump_stats(proc.pid, stats_path) if proc.poll() is None else {}
        for key in (
            "n_writes", "n_notifies", "notify_drop", "notify_send_fail",
            "conn_alloc_fail", "subs_alloc_fail", "n_batch_read_reqs",
            "n_batch_write_reqs",
        ):
            if key in stats_after:
                rec[key] = max(0, int(stats_after[key]))
            else:
                rec[key] = max(0, int(health_after.get(key, 0)) - int(health_before.get(key, 0)))
        if cpu_before and cpu_after:
            rec["cpu_s"] = max(0.0, cpu_after["cpu_s"] - cpu_before["cpu_s"])
            rec["rss_kb"] = cpu_after["rss_kb"]
        rec["n_conns"] = health_after.get("n_conns", 0)
        if cp.returncode != 0:
            rec["error"] = (cp.stderr or cp.stdout).strip().splitlines()[-1:] or ["load failed"]
    except Exception as e:  # noqa: BLE001
        rec["error"] = f"{type(e).__name__}: {e}"
    finally:
        if logf is not None:
            logf.close()
        if proc is not None:
            _stop_rtdbd(proc, instance)
        shutil.rmtree(work, ignore_errors=True)
    return rec


def _median(values: list[float]) -> float:
    return float(statistics.median(values)) if values else 0.0


def _summarize(records: list[dict]) -> list[dict]:
    groups: dict[tuple, list[dict]] = {}
    for r in records:
        groups.setdefault((r["points"], r["hz"], r["profile"]), []).append(r)
    out = []
    keys = (
        "elapsed_s", "n_writes", "n_notifies", "notify_drop", "notify_send_fail",
        "conn_alloc_fail", "subs_alloc_fail", "n_batch_read_reqs",
        "n_batch_write_reqs", "cpu_s", "rss_kb", "n_conns",
        "batch_read_ops_s", "batch_write_ops_s", "get_ops_s", "batch_speedup",
    )
    for (points, hz, profile), rows in sorted(groups.items()):
        rec = {
            "points": points, "hz": hz, "profile": profile,
            "runs": len(rows), "ok_runs": sum(1 for r in rows if r["ok"]),
            "clients": rows[0]["clients"],
            "subs_per_client": rows[0]["subs_per_client"],
        }
        for k in keys:
            rec[k] = _median([float(r.get(k, 0) or 0) for r in rows])
        out.append(rec)
    return out


def _write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    fields = list(rows[0].keys())
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)


def main() -> int:
    ap = argparse.ArgumentParser(description="InduRTDB v3.11 规模矩阵")
    ap.add_argument("--points", type=int, nargs="+", default=list(DEFAULT_POINTS))
    ap.add_argument("--hz", type=int, nargs="+", default=list(DEFAULT_HZ))
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--duration", type=float, default=None,
                    help="统一持续秒数；缺省 20k=600s，其余=60s")
    ap.add_argument("--duration-20k", type=float, default=600.0)
    ap.add_argument("--quick", action="store_true",
                    help="短跑冒烟：runs=1，所有档 duration=5s")
    ap.add_argument("--fanout", action="store_true",
                    help="使用 100 连接 × 10k 全量扇出 profile")
    ap.add_argument("--out", default="/tmp/indurtdb-scale-matrix")
    ap.add_argument("--build-dir", default=str(REPO / "build"))
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    if args.runs <= 0 or any(p < 2 for p in args.points) or any(h <= 0 for h in args.hz):
        print("points>=2、hz>0、runs>0", file=sys.stderr)
        return 2
    if args.quick:
        args.runs = 1
        args.duration = 5.0

    out = Path(args.out)
    (out / "logs").mkdir(parents=True, exist_ok=True)
    rtdbd = _build(Path(args.build_dir), args.no_build)

    records: list[dict] = []
    env = {
        "cpu_model": _cpu_model(),
        "cpu_count": os.cpu_count(),
        "mem_kb": _mem_kb(),
        "kernel": platform.release(),
        "machine": platform.machine(),
        "taskset": os.environ.get("TASKSET", ""),
        "rtdbd": str(rtdbd),
    }
    for points in args.points:
        for hz in args.hz:
            duration = args.duration if args.duration is not None else (
                args.duration_20k if points == 20_000 else 60.0
            )
            for run in range(args.runs):
                log = out / "logs" / f"{points}_{hz}hz_{run + 1}.log"
                print(f"== {points:,} pts @ {hz} Hz run {run + 1}/{args.runs} "
                      f"({duration:g}s) ==", flush=True)
                rec = _run_once(rtdbd, points, hz, duration, args.timeout,
                                args.fanout, log)
                rec["run"] = run + 1
                records.append(rec)
                print(json.dumps({k: rec.get(k) for k in (
                    "ok", "load_rc", "elapsed_s", "n_writes", "notify_drop",
                    "notify_send_fail", "batch_speedup", "error")},
                    ensure_ascii=False), flush=True)

    summary = _summarize(records)
    payload = {"env": env, "runs": records, "summary": summary}
    (out / "matrix.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    _write_csv(out / "matrix.csv", summary)
    print(f"\n写出 {out / 'matrix.json'} 和 {out / 'matrix.csv'}")
    failed = sum(1 for r in records if not r["ok"])
    return 0 if failed == 0 else 1


def _cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="ascii").splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def _mem_kb() -> int:
    try:
        for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
            if line.startswith("MemTotal:"):
                return int(line.split()[1])
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
