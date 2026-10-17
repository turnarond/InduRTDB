#!/usr/bin/env python3
"""
rtdbd 健康检查与告警基线（U4）

回答「rtdbd 现在健康吗、该告警什么」。rtdbd 只监听 AF_UNIX，无 HTTP/metrics
端点（见 rtdbd.c 的 socket(AF_UNIX,...)），因此不能靠 Prometheus 抓取；
本脚本直接消费 UDS 协议的 RTDBD_OP_HEALTH(opcode 16) 快照，产出：
  - 人可读的表格（默认）
  - Prometheus node_exporter textfile 格式（--prometheus FILE）
  - 退出码驱动告警：0=OK 1=告警 2=无法判定

纯标准库，无第三方依赖（与 tools/irt_diag 同一约定）。

用法:
    python3 scripts/check_rtdbd_health.py --socket /run/rtdbd/hvac.sock
    python3 scripts/check_rtdbd_health.py -s /run/rtdbd/hvac.sock --prometheus /var/lib/node_exporter/rtdbd.prom
    python3 scripts/check_rtdbd_health.py -s /run/rtdbd/hvac.sock --state /var/lib/rtdbd.hc.state

cron 每分钟跑时用 --state：脚本会与上次快照求差得到速率，
这是「notify_drop 持续增长」这类告警唯一可靠的判据。

退出码:
    0  健康
    1  命中告警基线
    2  无法连接 / 响应异常（不等于健康，不要静默）
"""
import argparse
import json
import os
import socket
import struct
import sys
import time

RTDBD_MAGIC = 0x52424431
RTDBD_PROTO_VERSION = 2
RTDBD_OP_HEALTH = 16
RTDBD_ST_OK = 0

REQ_HDR = struct.Struct("<IHHI")     # magic, version, opcode, payload_len
RESP_HDR = struct.Struct("<IHHI")    # magic, version, status, payload_len
HEALTH = struct.Struct("<QQQQQQQQII")  # 8×u64 + 2×u32 = 72B

HEALTH_NAME = {0: "OK", 1: "DEGRADED", 2: "UNHEALTHY"}

FIELDS = (
    ("n_writes", "成功写入次数"),
    ("n_notifies", "通知入队次数"),
    ("decode_fail", "报文解码失败（多为客户端协议版本不匹配）"),
    ("write_rejected", "鉴权拒绝（policy deny）"),
    ("write_error", "写入返回非 0"),
    ("notify_drop", "通知丢弃（队列满 / 连接关闭）"),
    ("notify_send_fail", "出站发送失败（对端已关闭）"),
    ("uptime_ns", "进程运行时长（ns）"),
)
RATE_FIELDS = ("decode_fail", "write_rejected", "write_error",
               "notify_drop", "notify_send_fail")


class ProbeError(Exception):
    pass


def fetch(sock_path, timeout):
    """连 UDS 发一次 OP_HEALTH，返回解析后的快照字典。"""
    if not os.path.exists(sock_path):
        raise ProbeError(f"socket 不存在: {sock_path}")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(sock_path)
        s.sendall(REQ_HDR.pack(RTDBD_MAGIC, RTDBD_PROTO_VERSION,
                               RTDBD_OP_HEALTH, 0))
        head = b""
        while len(head) < RESP_HDR.size:
            chunk = s.recv(RESP_HDR.size - len(head))
            if not chunk:
                raise ProbeError("响应头不完整（对端可能已关闭）")
            head += chunk
        magic, _ver, status, plen = RESP_HDR.unpack(head)
        if magic != RTDBD_MAGIC:
            raise ProbeError(f"magic 不符: 0x{magic:08X}")
        if status != RTDBD_ST_OK:
            raise ProbeError(f"rtdbd 返回状态码 {status}")
        if plen != HEALTH.size:
            raise ProbeError(f"响应负载长度异常: {plen} != {HEALTH.size}")
        body = b""
        while len(body) < plen:
            chunk = s.recv(plen - len(body))
            if not chunk:
                raise ProbeError("响应负载不完整")
            body += chunk
    finally:
        s.close()

    vals = HEALTH.unpack(body)
    snap = dict(zip([f[0] for f in FIELDS], vals[:8]))
    snap["health"] = vals[8]
    snap["n_conns"] = vals[9]
    return snap


def load_state(path):
    if not path or not os.path.exists(path):
        return None
    try:
        with open(path, encoding="utf-8") as f:
            prev = json.load(f)
        return prev if prev.get("schema") == 1 else None
    except (OSError, ValueError):
        return None


def save_state(path, snap, sock_path):
    if not path:
        return
    rec = {"schema": 1, "ts": time.time(), "sock": sock_path,
           "snap": {k: snap[k] for k, _ in FIELDS}}
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(rec, f)
    os.replace(tmp, path)          # 原子替换，避免 cron 并发读到半个文件


def evaluate(snap, prev):
    """返回 [(级别, 说明, 值)]，级别 ∈ {'CRIT','WARN'}。"""
    alerts = []

    h = snap["health"]
    if h == 2:
        alerts.append(("CRIT", "段头自检失败（magic/version 不符），必须立即介入",
                       "UNHEALTHY"))
    elif h == 1:
        # rtdbd 内部已用 60s 滑动窗口判定，此处仍为 DEGRADED 意味着已持续一段时间
        alerts.append(("WARN", "健康降级（rtdbd 内含 60s 滑动窗口，持续存在）",
                       "DEGRADED"))

    if prev is None:
        return alerts

    dt = time.time() - prev.get("ts", 0)
    if dt <= 0:
        return alerts
    ps = prev.get("snap", {})

    for k in RATE_FIELDS:
        d = snap.get(k, 0) - ps.get(k, 0)
        if d <= 0:
            continue
        rate = d / dt
        if k == "decode_fail":
            alerts.append(("WARN", "解码失败速率异常 → 查客户端协议版本", f"{rate:.3f}/s"))
        elif k == "notify_drop":
            alerts.append(("CRIT", "通知丢弃持续增长 → 订阅端消费慢于生产，"
                                    "容量规划的硬信号", f"{rate:.3f}/s"))
        elif k == "notify_send_fail":
            alerts.append(("WARN", "出站发送失败 → 对端可能已断开", f"{rate:.3f}/s"))
        elif k == "write_rejected":
            alerts.append(("WARN", "鉴权拒绝速率异常 → 查 policy 是否过窄",
                           f"{rate:.3f}/s"))
        elif k == "write_error":
            alerts.append(("WARN", "写入错误速率异常", f"{rate:.3f}/s"))
    return alerts


def render_text(snap, alerts, prev):
    out = ["rtdbd 健康快照", "=" * 46]
    for k, desc in FIELDS:
        out.append(f"  {k:<18} {snap[k]:>22,}   {desc}")
    out.append(f"  {'health':<18} {HEALTH_NAME.get(snap['health'], snap['health']):>22}   RTDBD_HEALTH_*")
    out.append(f"  {'n_conns':<18} {snap['n_conns']:>22,}   当前连接数")
    if prev:
        out.append(f"  （速率基线取自 {prev.get('ts', 0):.0f} 之前的快照）")
    else:
        out.append("  （无历史快照，本次不计算速率——首次运行请预期只有绝对值判定）")
    if alerts:
        out.append("")
        out.append("告警:")
        for lvl, desc, val in alerts:
            out.append(f"  [{lvl}] {desc}  ({val})")
    else:
        out.append("")
        out.append("告警: 无")
    return "\n".join(out)


def render_prometheus(snap):
    """node_exporter textfile collector 格式。"""
    L = []
    L.append("# HELP rtdbd_health rtdbd 健康状态 (0=OK 1=DEGRADED 2=UNHEALTHY)")
    L.append("# TYPE rtdbd_health gauge")
    L.append(f"rtdbd_health {snap['health']}")
    L.append("# HELP rtdbd_uptime_seconds rtdbd 运行时长")
    L.append("# TYPE rtdbd_uptime_seconds gauge")
    L.append(f"rtdbd_uptime_seconds {snap['uptime_ns'] / 1e9:.3f}")
    L.append("# HELP rtdbd_connections rtdbd 当前连接数")
    L.append("# TYPE rtdbd_connections gauge")
    L.append(f"rtdbd_connections {snap['n_conns']}")
    for k in ("n_writes", "n_notifies", "decode_fail", "write_rejected",
              "write_error", "notify_drop", "notify_send_fail"):
        L.append(f"# TYPE rtdbd_{k}_total counter")
        L.append(f"rtdbd_{k}_total {snap[k]}")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(
        description="rtdbd 健康检查与告警基线",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-s", "--socket", required=True, help="rtdbd 的 UDS socket 路径")
    ap.add_argument("--timeout", type=float, default=3.0, help="连接与读超时秒（默认 3）")
    ap.add_argument("--state", help="历史快照文件；给出后按两次快照求速率（cron 场景必用）")
    ap.add_argument("--prometheus", metavar="FILE",
                    help="同时输出 node_exporter textfile 格式到该文件")
    ap.add_argument("-q", "--quiet", action="store_true", help="只输出退出码，不打印")
    a = ap.parse_args()

    try:
        snap = fetch(a.socket, a.timeout)
    except ProbeError as e:
        print(f"[无法判定] {e}", file=sys.stderr)
        print("提示：这不是「健康」。rtdbd 未运行 / socket 路径不对 / 段已损坏。",
              file=sys.stderr)
        return 2
    except OSError as e:
        print(f"[无法判定] 连接失败: {e}", file=sys.stderr)
        return 2

    prev = load_state(a.state)
    alerts = evaluate(snap, prev)

    if not a.quiet:
        print(render_text(snap, alerts, prev))
    if a.prometheus:
        d = os.path.dirname(a.prometheus)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(a.prometheus, "w", encoding="utf-8") as f:
            f.write(render_prometheus(snap))

    save_state(a.state, snap, a.socket)

    if any(lvl == "CRIT" for lvl, _, _ in alerts):
        return 1
    if alerts:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())