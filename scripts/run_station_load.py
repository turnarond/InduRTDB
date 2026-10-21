#!/usr/bin/env python3
"""v3.10 单站规模化负载：20k 点 × 1 Hz 写 × 50 客户端 × 500 订阅。

被测目标：`rtdbd --max-points 20000 --max-subs 1024`，50 个读连接各自
`SET_SUBS` 500 点，8 个写线程按每点约 1 次/秒轮转写入；持续读 NOTIFY 并统计
每客户端帧数、写 ACK P99、health.notify_drop 与 rtdbd 进程 RSS/CPU。

纯标准库；协议客户端复用 tools/rtdb-monitor/rtdb_client.py。
用法：
  python3 scripts/run_station_load.py --socket /tmp/indurtdb_station_load.sock \
      --points 20000 --clients 50 --subs 500 --duration 600 --write-rate 20000
"""
from __future__ import annotations

import argparse
import array
import os
import random
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TOOL = os.path.join(REPO, "tools", "rtdb-monitor")
sys.path.insert(0, TOOL)
sys.path.insert(0, HERE)

from rtdb_client import NotifyListener, RtdbClient, TYPE_INT32  # noqa: E402
from check_rtdbd_health import fetch as fetch_health  # noqa: E402


@dataclass
class WriterStats:
    writes: int = 0
    errors: int = 0
    lat_ns: array.array = field(default_factory=lambda: array.array("Q"))


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


class Writer(threading.Thread):
    def __init__(self, sock: str, ids: list[int], index: int, workers: int,
                 rate: float, stop: threading.Event, deadline: float,
                 timeout: float):
        super().__init__(name=f"writer-{index}", daemon=True)
        self.sock = sock
        self.ids = ids
        self.index = index
        self.workers = workers
        self.rate = rate
        self.stop = stop
        self.deadline = deadline
        self.timeout = timeout
        self.stats = WriterStats()
        self.lat = LatencySampler()
        self.fatal: Optional[str] = None

    def run(self) -> None:
        assigned = self.ids[self.index::self.workers]
        if not assigned:
            return
        period_ns = max(1, int(1e9 / self.rate))
        next_ns = time.perf_counter_ns()
        cycle = 0
        try:
            client = RtdbClient(self.sock, timeout=self.timeout).connect()
        except Exception as e:  # noqa: BLE001
            self.fatal = f"connect: {e}"
            return
        try:
            while not self.stop.is_set() and time.monotonic() < self.deadline:
                for pid in assigned:
                    if self.stop.is_set() or time.monotonic() >= self.deadline:
                        break
                    now = time.perf_counter_ns()
                    if next_ns > now:
                        time.sleep((next_ns - now) / 1e9)
                    value = (cycle * 100_000 + pid) & 0x7FFFFFFF
                    t0 = time.perf_counter_ns()
                    try:
                        client.write_point(pid, TYPE_INT32, value)
                        self.stats.writes += 1
                        self.lat.add(time.perf_counter_ns() - t0)
                    except Exception as e:  # noqa: BLE001
                        self.stats.errors += 1
                        if self.stats.errors <= 3:
                            self.fatal = f"write {pid}: {e}"
                    next_ns += period_ns
                    # 严重落后时不要无限追赶，避免突发把队列打满。
                    if next_ns < time.perf_counter_ns() - 1_000_000_000:
                        next_ns = time.perf_counter_ns()
                cycle += 1
        finally:
            client.close()


@dataclass
class ReaderStats:
    frames: int = 0
    errors: list[str] = field(default_factory=list)
    alive: bool = False


class Reader:
    def __init__(self, sock: str, ids: list[int], index: int, timeout: float):
        self.sock = sock
        self.ids = ids
        self.index = index
        self.timeout = timeout
        self.stats = ReaderStats()
        self.listener: Optional[NotifyListener] = None
        self._lock = threading.Lock()

    def _on_notify(self, _pid: int, _info: dict) -> None:
        with self._lock:
            self.stats.frames += 1

    def start(self) -> None:
        self.listener = NotifyListener(self.sock, on_notify=self._on_notify,
                                       timeout=self.timeout)
        try:
            self.listener.start()
            self.listener.set_subs(self.ids)
        except Exception as e:  # noqa: BLE001
            self.stats.errors.append(f"start/set_subs: {e}")
        self.stats.alive = True

    def check(self) -> None:
        if self.listener and self.listener._thread is not None:
            if not self.listener._thread.is_alive():
                self.stats.errors.append("listener thread exited")

    def stop(self) -> None:
        if self.listener:
            self.listener.stop()


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


def main() -> int:
    ap = argparse.ArgumentParser(description="InduRTDB v3.10 单站规模化负载")
    ap.add_argument("--socket", required=True, help="rtdbd UDS socket")
    ap.add_argument("--points", type=int, default=20000, help="配置点位数（默认 20000）")
    ap.add_argument("--clients", type=int, default=50, help="读客户端数（默认 50）")
    ap.add_argument("--subs", type=int, default=500, help="每客户端订阅数（默认 500）")
    ap.add_argument("--duration", type=float, default=600.0, help="持续秒数（默认 600）")
    ap.add_argument("--write-rate", type=int, default=20000, help="总写速率（默认 20000/s）")
    ap.add_argument("--workers", type=int, default=8, help="写线程数（默认 8）")
    ap.add_argument("--timeout", type=float, default=5.0, help="UDS 客户端超时秒")
    ap.add_argument("--pid", type=int, help="rtdbd PID（用于 RSS/CPU）")
    args = ap.parse_args()

    if args.clients <= 0 or args.subs <= 0 or args.duration <= 0 or args.write_rate <= 0:
        print("clients/subs/duration/write-rate 必须 > 0", file=sys.stderr)
        return 2
    if args.points <= 0 or args.subs > args.points:
        print("points 必须 > 0 且 subs <= points", file=sys.stderr)
        return 2

    with RtdbClient(args.socket, timeout=args.timeout) as c:
        listed = c.list_points()
    ids = [p["id"] for p in listed[:args.points]]
    if len(ids) < args.points:
        print(f"rtdbd 只有 {len(ids)} 个点位，期望 {args.points}", file=sys.stderr)
        return 2

    print(f"station-load: points={len(ids)} clients={args.clients} "
          f"subs/client={args.subs} write-rate={args.write_rate}/s duration={args.duration}s")

    before = _proc_stat(args.pid)
    health_before = fetch_health(args.socket, args.timeout)

    # 50 个读客户端：窗口尽量错开，覆盖全库；50×500 超过 20k 时允许重叠。
    stride = max(1, len(ids) // args.clients)
    readers: list[Reader] = []
    for i in range(args.clients):
        start = (i * stride) % len(ids)
        sub_ids = [ids[(start + j) % len(ids)] for j in range(args.subs)]
        r = Reader(args.socket, sub_ids, i, args.timeout)
        r.start()
        readers.append(r)

    # 给快照留出进入队列/被消费的时间，快照本身不计入写速率。
    time.sleep(0.5)

    stop = threading.Event()
    deadline = time.monotonic() + args.duration
    per_worker = args.write_rate / args.workers
    writers = [Writer(args.socket, ids, i, args.workers, per_worker,
                      stop, deadline, args.timeout) for i in range(args.workers)]
    t_start = time.monotonic()
    for w in writers:
        w.start()
    for w in writers:
        w.join(timeout=args.duration + 30)
    stop.set()
    elapsed = max(1e-9, time.monotonic() - t_start)

    # 先取 health，再停 reader；避免关闭连接时的 outq 丢弃污染被测窗口。
    health_after = fetch_health(args.socket, args.timeout)
    for r in readers:
        r.check()
    for r in readers:
        r.stop()
    after = _proc_stat(args.pid)

    total_writes = sum(w.stats.writes for w in writers)
    errors = sum(w.stats.errors for w in writers)
    p99 = max((w.lat.p99() for w in writers), default=0.0)
    reader_errors = [e for r in readers for e in r.stats.errors]
    frames = [r.stats.frames for r in readers]
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
    if errors:
        failures.append(f"writer errors={errors}")
    if reader_errors:
        failures.append(f"reader errors={len(reader_errors)}: {reader_errors[:3]}")
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
