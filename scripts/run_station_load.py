#!/usr/bin/env python3
"""v3.10 单站规模化负载：20k 点 × 1 Hz 写 × 50 客户端 × 500 订阅。

被测目标：`rtdbd --max-points 20000 --max-subs 1024`，50 个读连接各自
`SET_SUBS` 500 点，8 个写进程按每点约 1 次/秒轮转写入；持续读 NOTIFY 并统计
每客户端帧数、写 ACK P99、health.notify_drop 与 rtdbd 进程 RSS/CPU。

写端与读端均使用独立进程：同一 Python 进程内的 8 写线程 + 50 读线程会被
GIL 串行化，测得的是负载生成器上限而非 rtdbd 上限；独立进程各自持有连接，
才能把 20k/s 写负载真正压到服务端。

纯标准库；协议客户端复用 tools/rtdb-monitor/rtdb_client.py。
用法：
  python3 scripts/run_station_load.py --socket /tmp/indurtdb_station_load.sock \
      --points 20000 --clients 50 --subs 500 --duration 600 --write-rate 20000
"""
from __future__ import annotations

import argparse
import array
import multiprocessing as mp
import os
import queue
import random
import statistics
import sys
import time
from typing import Optional

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TOOL = os.path.join(REPO, "tools", "rtdb-monitor")
sys.path.insert(0, TOOL)
sys.path.insert(0, HERE)

from rtdb_client import NotifyListener, RtdbClient, TYPE_INT32  # noqa: E402
from check_rtdbd_health import fetch as fetch_health  # noqa: E402


class LatencySampler:
    """保留全部样本直到上限，之后水库采样，避免 10 分钟撑爆内存。"""

    def __init__(self, cap: int = 200_000):
        self.cap = cap
        self.buf: array.array = array.array("Q")
        self.n = 0

    def add(self, value: int) -> None:
        self.n += 1
        if len(self.buf) < self.cap:
            self.buf.append(value)
        else:
            j = random.randrange(self.n)
            if j < self.cap:
                self.buf[j] = value

    def p99(self) -> float:
        if not self.buf:
            return 0.0
        vals = sorted(self.buf)
        idx = min(len(vals) - 1, int(len(vals) * 0.99))
        return vals[idx] / 1000.0


def _writer_worker(sock: str, ids: list[int], index: int, workers: int,
                   rate: float, duration: float, timeout: float,
                   start: mp.synchronize.Event,
                   ready_q: mp.Queue, result_q: mp.Queue) -> None:
    assigned = ids[index::workers]
    result = {
        "kind": "writer",
        "index": index,
        "writes": 0,
        "errors": 0,
        "fatal": None,
        "p99_us": 0.0,
    }
    if not assigned:
        ready_q.put((index, True))
        result_q.put(result)
        return

    try:
        client = RtdbClient(sock, timeout=timeout).connect()
    except Exception as e:  # noqa: BLE001
        result["fatal"] = f"connect: {e}"
        ready_q.put((index, False))
        result_q.put(result)
        return

    ready_q.put((index, True))
    if not start.wait(timeout=60):
        result["fatal"] = "start timeout"
        client.close()
        result_q.put(result)
        return

    lat = LatencySampler()
    period_ns = max(1, int(1e9 / rate))
    next_ns = time.perf_counter_ns()
    deadline = time.monotonic() + duration
    cycle = 0
    try:
        while time.monotonic() < deadline:
            for pid in assigned:
                if time.monotonic() >= deadline:
                    break
                now = time.perf_counter_ns()
                if next_ns > now:
                    time.sleep((next_ns - now) / 1e9)
                value = (cycle * 100_000 + pid) & 0x7FFFFFFF
                t0 = time.perf_counter_ns()
                try:
                    client.write_point(pid, TYPE_INT32, value)
                    result["writes"] += 1
                    lat.add(time.perf_counter_ns() - t0)
                except Exception as e:  # noqa: BLE001
                    result["errors"] += 1
                    if result["errors"] <= 3:
                        result["fatal"] = f"write {pid}: {e}"
                next_ns += period_ns
                if next_ns < time.perf_counter_ns() - 1_000_000_000:
                    next_ns = time.perf_counter_ns()
            cycle += 1
    finally:
        client.close()
        result["p99_us"] = lat.p99()
        result_q.put(result)


def _reader_worker(sock: str, ids: list[int], index: int, duration: float,
                   timeout: float, start: mp.synchronize.Event,
                   ready_q: mp.Queue, result_q: mp.Queue) -> None:
    frames = 0
    errors: list[str] = []

    def on_notify(_pid: int, _info: dict) -> None:
        nonlocal frames
        frames += 1

    listener = NotifyListener(sock, on_notify=on_notify, timeout=timeout)
    try:
        listener.start()
        listener.set_subs(ids)
    except Exception as e:  # noqa: BLE001
        errors.append(f"start/set_subs: {e}")
        ready_q.put((index, False))
        listener.stop()
        result_q.put({"kind": "reader", "index": index, "frames": frames,
                      "errors": errors})
        return

    ready_q.put((index, True))
    try:
        if not start.wait(timeout=60):
            errors.append("start timeout")
        else:
            time.sleep(duration + 1.0)
            if listener._thread is not None and not listener._thread.is_alive():
                errors.append("listener thread exited")
    finally:
        listener.stop()
        result_q.put({"kind": "reader", "index": index, "frames": frames,
                      "errors": errors})


def _proc_stat(pid: Optional[int]) -> Optional[dict]:
    if not pid:
        return None
    try:
        with open(f"/proc/{pid}/stat", encoding="ascii") as f:
            raw = f.read().split()
        # comm 可能含空格，字段使用从 ')' 之后重新切分更稳；这里 stat 的 comm 不含空格时够用。
        if len(raw) < 24:
            return None
        ticks = os.sysconf("SC_CLK_TCK")
        page = os.sysconf("SC_PAGE_SIZE")
        return {
            "cpu_s": (int(raw[13]) + int(raw[14])) / ticks,
            "rss_kb": int(raw[23]) * page // 1024,
        }
    except (OSError, ValueError):
        return None


def _collect_results(result_q: mp.Queue, expected: int) -> list[dict]:
    out: list[dict] = []
    deadline = time.monotonic() + 15
    while len(out) < expected:
        remain = deadline - time.monotonic()
        if remain <= 0:
            break
        try:
            out.append(result_q.get(timeout=remain))
        except queue.Empty:
            break
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="InduRTDB v3.10 单站规模化负载")
    ap.add_argument("--socket", required=True, help="rtdbd UDS socket")
    ap.add_argument("--points", type=int, default=20000, help="配置点位数（默认 20000）")
    ap.add_argument("--clients", type=int, default=50, help="读客户端数（默认 50）")
    ap.add_argument("--subs", type=int, default=500, help="每客户端订阅数（默认 500）")
    ap.add_argument("--duration", type=float, default=600.0, help="持续秒数（默认 600）")
    ap.add_argument("--write-rate", type=int, default=20000, help="总写速率（默认 20000/s）")
    ap.add_argument("--workers", type=int, default=8, help="写进程数（默认 8）")
    ap.add_argument("--timeout", type=float, default=5.0, help="UDS 客户端超时秒")
    ap.add_argument("--pid", type=int, help="rtdbd PID（用于 RSS/CPU）")
    args = ap.parse_args()

    if args.clients <= 0 or args.subs <= 0 or args.duration <= 0 or args.write_rate <= 0:
        print("clients/subs/duration/write-rate 必须 > 0", file=sys.stderr)
        return 2
    if args.points <= 0 or args.subs > args.points:
        print("points 必须 > 0 且 subs <= points", file=sys.stderr)
        return 2
    if args.workers <= 0:
        print("workers 必须 > 0", file=sys.stderr)
        return 2

    with RtdbClient(args.socket, timeout=args.timeout) as c:
        listed = c.list_points()
    ids = [p["id"] for p in listed[:args.points]]
    if len(ids) < args.points:
        print(f"rtdbd 只有 {len(ids)} 个点位，期望 {args.points}", file=sys.stderr)
        return 2

    print(f"station-load: points={len(ids)} clients={args.clients} "
          f"subs/client={args.subs} write-rate={args.write_rate}/s "
          f"workers={args.workers} duration={args.duration}s")

    before = _proc_stat(args.pid)
    health_before = fetch_health(args.socket, args.timeout)

    ctx = mp.get_context()
    start = ctx.Event()
    ready_q: mp.Queue = ctx.Queue()
    result_q: mp.Queue = ctx.Queue()

    stride = max(1, len(ids) // args.clients)
    readers = []
    for i in range(args.clients):
        first = (i * stride) % len(ids)
        sub_ids = [ids[(first + j) % len(ids)] for j in range(args.subs)]
        p = ctx.Process(target=_reader_worker,
                        args=(args.socket, sub_ids, i, args.duration,
                              args.timeout, start, ready_q, result_q))
        p.start()
        readers.append(p)

    per_worker = args.write_rate / args.workers
    writers = []
    for i in range(args.workers):
        p = ctx.Process(target=_writer_worker,
                        args=(args.socket, ids, i, args.workers, per_worker,
                              args.duration, args.timeout, start, ready_q, result_q))
        p.start()
        writers.append(p)

    ready_ok = 0
    ready_failed = 0
    ready_deadline = time.monotonic() + 60
    for _ in range(args.clients + args.workers):
        remain = ready_deadline - time.monotonic()
        if remain <= 0:
            break
        try:
            _index, ok = ready_q.get(timeout=remain)
        except queue.Empty:
            break
        if ok:
            ready_ok += 1
        else:
            ready_failed += 1

    # SET_SUBS 请求发出到服务端生效之间留出窗口，避免首批写通知落空。
    time.sleep(0.5)
    t_start = time.monotonic()
    start.set()

    for p in writers:
        p.join(timeout=args.duration + 30)
    elapsed = max(1e-9, time.monotonic() - t_start)

    health_after = fetch_health(args.socket, args.timeout)
    for p in readers:
        p.join(timeout=10)

    alive = [p for p in writers + readers if p.is_alive()]
    for p in alive:
        p.terminate()
        p.join(timeout=5)

    results = _collect_results(result_q, args.clients + args.workers)
    writer_results = [r for r in results if r["kind"] == "writer"]
    reader_results = [r for r in results if r["kind"] == "reader"]
    after = _proc_stat(args.pid)

    total_writes = sum(r["writes"] for r in writer_results)
    errors = sum(r["errors"] for r in writer_results)
    p99 = max((r["p99_us"] for r in writer_results), default=0.0)
    reader_errors = [e for r in reader_results for e in r["errors"]]
    frames = [r["frames"] for r in reader_results]
    drops = health_after["notify_drop"] - health_before["notify_drop"]
    notifies = health_after["n_notifies"] - health_before["n_notifies"]
    min_expected = int(args.subs * max(1.0, args.duration) * 0.5)

    print("\n结果")
    print("=" * 58)
    print(f"写 ACK: {total_writes:,} 次 / {elapsed:.2f}s = {total_writes / elapsed:,.0f}/s  "
          f"errors={errors}  P99={p99:.1f} μs")
    if frames:
        print(f"读帧: min={min(frames):,} median={int(statistics.median(frames)):,} "
              f"max={max(frames):,}  client_errors={len(reader_errors)}")
    print(f"health: n_writes+{health_after['n_writes'] - health_before['n_writes']:,}  "
          f"n_notifies+{notifies:,}  notify_drop+{drops:,}")
    if before and after:
        print(f"rtdbd pid {args.pid}: RSS {after['rss_kb']:,} KB  "
              f"CPU {after['cpu_s'] - before['cpu_s']:.2f}s")
    elif args.pid:
        print(f"rtdbd pid {args.pid}: /proc 不可读")

    failures: list[str] = []
    if ready_failed or ready_ok < args.clients + args.workers:
        failures.append(f"worker ready {ready_ok}/{args.clients + args.workers}"
                        f" failed={ready_failed}")
    if alive:
        failures.append(f"workers still alive after deadline: {len(alive)}")
    if errors:
        failures.append(f"writer errors={errors}")
    if reader_errors:
        failures.append(f"reader errors={len(reader_errors)}: {reader_errors[:3]}")
    if len(writer_results) != args.workers:
        failures.append(f"writer results {len(writer_results)}/{args.workers}")
    if len(reader_results) != args.clients:
        failures.append(f"reader results {len(reader_results)}/{args.clients}")
    if drops != 0:
        failures.append(f"notify_drop={drops}（必须为 0）")
    if frames and min(frames) < min_expected:
        failures.append(f"最小客户端帧数 {min(frames)} < {min_expected}")

    if failures:
        print("\nFAIL: " + "; ".join(failures), file=sys.stderr)
        return 1
    print("\nPASS: notify_drop=0；客户端无断链；帧数达到下限")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
