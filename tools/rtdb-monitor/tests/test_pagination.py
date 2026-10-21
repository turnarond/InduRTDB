#!/usr/bin/env python3
"""v3.10 监控后端服务端分页 + 每 WS 独立订阅端到端测试。

启真实 rtdbd + FastAPI TestClient：
  - /api/points 分页形状、q 过滤、分区过滤、无参旧数组
  - 两个 WS 各自 set_subs 不同页，互不串点
  - 500 点 set_subs 快照在 5s 内收齐
用法：python3 tests/test_pagination.py
"""
import contextlib
import io
import os
import queue
import signal
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(TOOL))
RTDBD = os.path.join(REPO, "build", "rtdbd", "rtdbd")

CFG_YAML = """\
points:
  - name: AHU_01.Supply_Temp
    id: 10
    type: int32
    access: 3
  - name: Pump_Start_CMD
    id: 11
    type: int32
    access: 3
  - name: Line_Status
    id: 12
    type: string
    access: 3
"""


def ensure_built() -> None:
    if os.path.exists(RTDBD):
        return
    subprocess.run(["cmake", "-S", REPO, "-B", os.path.join(REPO, "build"),
                    "-DCMAKE_BUILD_TYPE", "Release"], check=True)
    subprocess.run(["cmake", "--build", os.path.join(REPO, "build"),
                    "-j", str(os.cpu_count() or 1)], check=True)


def ws_recv_json(ws, timeout: float = 5.0):
    q: queue.Queue = queue.Queue()

    def _rx() -> None:
        try:
            q.put(("ok", ws.receive_json()))
        except Exception as e:  # noqa: BLE001
            q.put(("err", e))

    threading.Thread(target=_rx, daemon=True).start()
    kind, val = q.get(timeout=timeout)
    if kind == "err":
        raise val
    return val


def ws_expect_no_update(ws, timeout: float = 0.6) -> None:
    try:
        msg = ws_recv_json(ws, timeout)
    except (AssertionError, queue.Empty):
        return
    assert msg.get("op") != "update", f"unexpected cross-page update: {msg}"


def case_points_page_shape(client) -> None:
    r = client.get("/api/points?offset=0&limit=10")
    assert r.status_code == 200, r.text
    body = r.json()
    assert set(body) >= {"points", "total", "offset", "limit"}, body
    assert body["offset"] == 0 and body["limit"] == 10
    assert body["total"] == 503, body["total"]
    assert len(body["points"]) <= 10
    ids = [p["id"] for p in body["points"]]
    assert ids == sorted(ids), ids


def case_points_filter_q(client) -> None:
    r = client.get("/api/points?q=temp&offset=0&limit=50")
    assert r.status_code == 200, r.text
    pts = r.json()["points"]
    assert pts and all("temp" in p["name"].lower() for p in pts), pts


def case_points_partition_with_page(client) -> None:
    r = client.get("/api/points?partition=0&offset=0&limit=5")
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["total"] == 503, body
    assert len(body["points"]) == 5
    assert all(p.get("partition") == 0 for p in body["points"]), body
    # 无 offset/limit、仅 partition → 仍是旧数组
    r2 = client.get("/api/points?partition=0")
    assert r2.status_code == 200 and isinstance(r2.json(), list), r2.text


def case_points_legacy_full_list(client) -> None:
    r = client.get("/api/points")
    assert r.status_code == 200, r.text
    pts = r.json()
    assert isinstance(pts, list) and len(pts) == 503, len(pts)


def case_two_ws_isolated(client) -> None:
    with client.websocket_connect("/ws") as ws1, client.websocket_connect("/ws") as ws2:
        assert ws_recv_json(ws1)["op"] == "list"
        assert ws_recv_json(ws2)["op"] == "list"

        ws1.send_json({"op": "set_subs", "ids": [10]})
        ws2.send_json({"op": "set_subs", "ids": [11]})

        # 各自等待 subscribed + 快照 10/11
        seen1 = set()
        seen2 = set()
        for _ in range(10):
            m = ws_recv_json(ws1)
            if m.get("op") == "update" and m.get("id") == 10:
                seen1.add(10)
                break
        for _ in range(10):
            m = ws_recv_json(ws2)
            if m.get("op") == "update" and m.get("id") == 11:
                seen2.add(11)
                break
        assert 10 in seen1 and 11 in seen2, (seen1, seen2)

        client.post("/api/points/10/set", json={"value": 101})
        client.post("/api/points/11/set", json={"value": 202})

        got1 = None
        for _ in range(10):
            m = ws_recv_json(ws1)
            if m.get("op") == "update" and m.get("id") == 10:
                got1 = m
                break
        got2 = None
        for _ in range(10):
            m = ws_recv_json(ws2)
            if m.get("op") == "update" and m.get("id") == 11:
                got2 = m
                break
        assert got1 is not None and got1["value"] == "101", got1
        assert got2 is not None and got2["value"] == "202", got2

        # 串页检查：ws1 不应收到点 11，ws2 不应收到点 10
        ws_expect_no_update(ws1)
        ws_expect_no_update(ws2)


def case_500_snapshot(client) -> None:
    ids = list(range(100, 600))
    with client.websocket_connect("/ws") as ws:
        assert ws_recv_json(ws)["op"] == "list"
        ws.send_json({"op": "set_subs", "ids": ids})
        got = set()
        deadline = time.time() + 5.0
        while len(got) < 500 and time.time() < deadline:
            m = ws_recv_json(ws, timeout=max(0.1, deadline - time.time()))
            if m.get("op") == "update":
                got.add(m["id"])
        assert len(got) == 500, f"only {len(got)} snapshots"


def main() -> None:
    ensure_built()
    tmp = tempfile.mkdtemp()
    sock = os.path.join(tmp, "rtdbd.sock")
    cfg = os.path.join(tmp, "cfg.yaml")
    pol = os.path.join(tmp, "policy.txt")
    with open(cfg, "w") as f:
        f.write(CFG_YAML)
    with open(pol, "w") as f:
        f.write(f"{os.getuid()}:*:0:1023\n")

    proc = subprocess.Popen(
        [RTDBD, "--instance", "pagination", "--socket", sock,
         "--config", cfg, "--policy", pol,
         "--max-points", "1024", "--max-subs", "1024"]
    )
    try:
        for _ in range(50):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        assert os.path.exists(sock), "rtdbd socket not ready"

        os.environ["RTDBD_SOCK"] = sock
        sys.path.insert(0, TOOL)
        from rtdb_client import RtdbClient
        from fastapi.testclient import TestClient
        import app as appmod

        with RtdbClient(sock) as c, contextlib.redirect_stdout(io.StringIO()):
            for pid in range(100, 600):
                c.create_point(pid, f"Scale_{pid}", "int32", 3)
                c.write_point(pid, 1, pid)

        with TestClient(appmod.app) as client:
            case_points_page_shape(client)
            case_points_filter_q(client)
            case_points_partition_with_page(client)
            case_points_legacy_full_list(client)
            case_two_ws_isolated(client)
            case_500_snapshot(client)

        print("PAGINATION OK: page shape/filter/partition/legacy + WS isolation + 500 snapshots")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
