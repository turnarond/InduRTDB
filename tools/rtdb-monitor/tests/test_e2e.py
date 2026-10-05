#!/usr/bin/env python3
"""rtdb-monitor 端到端测试：启真实 rtdbd + FastAPI TestClient（含 WebSocket）。

不依赖外部网络；uvicorn 以 TestClient 进程内方式驱动 app，
但 app 通过真实 UDS 连到真实 rtdbd，覆盖 REST 与 WS 实时推送全链路。
用法：python3 tests/test_e2e.py
"""
import os
import sys
import signal
import shutil
import subprocess
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)                       # tools/rtdb-monitor
REPO = os.path.dirname(os.path.dirname(TOOL))       # 仓库根
RTDBD = os.path.join(REPO, "build", "rtdbd", "rtdbd")

CFG_YAML = """\
points:
  - name: AHU_01.Supply_Temp
    id: 10
    type: int32
    access: read_write
  - name: Pump_Start_CMD
    id: 11
    type: bool
    access: read_write
"""


def ensure_built() -> None:
    if os.path.exists(RTDBD):
        return
    print("[e2e] building rtdbd ...")
    subprocess.run(["cmake", "-S", REPO, "-B", os.path.join(REPO, "build"),
                    "-DCMAKE_BUILD_TYPE", "Release"], check=True)
    subprocess.run(["cmake", "--build", os.path.join(REPO, "build"),
                    "-j", str(os.cpu_count() or 1)], check=True)


def main() -> None:
    ensure_built()
    tmp = tempfile.mkdtemp()
    sock = os.path.join(tmp, "rtdbd.sock")
    cfg = os.path.join(tmp, "cfg.yaml")
    pol = os.path.join(tmp, "policy.txt")
    with open(cfg, "w") as f:
        f.write(CFG_YAML)
    with open(pol, "w") as f:
        f.write(f"{os.getuid()}:0:63\n")  # 放行当前 uid（deny by default）

    proc = subprocess.Popen(
        [RTDBD, "--instance", "e2e", "--sock", sock,
         "--config", cfg, "--policy", pol, "--max-points", "64"]
    )
    try:
        for _ in range(50):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        assert os.path.exists(sock), "rtdbd socket not ready"

        os.environ["RTDBD_SOCK"] = sock
        sys.path.insert(0, TOOL)
        from fastapi.testclient import TestClient
        import app as appmod

        with TestClient(appmod.app) as client:
            # ---- REST：枚举 ----
            r = client.get("/api/points")
            assert r.status_code == 200, r.text
            pts = r.json()
            assert len(pts) == 2, pts
            names = {p["name"]: p["id"] for p in pts}
            assert names.get("AHU_01.Supply_Temp") == 10

            # ---- REST：按 id 读当前值 ----
            r = client.get("/api/points/10")
            assert r.status_code == 200, r.text
            p10 = r.json()
            assert p10["type"] == 1, p10          # int32
            assert p10["value"] == 0, p10         # 初始值

            # ---- REST：设置值 ----
            r = client.post("/api/points/10/set", json={"value": 77})
            assert r.status_code == 200, r.text
            r = client.get("/api/points/10")
            assert r.json()["value"] == 77, r.json()

            # ---- REST：元数据 ----
            r = client.get("/api/meta/10")
            assert r.status_code == 200, r.text
            assert "eurMin" in r.json()

            # ---- WebSocket：清单 + 实时 update ----
            with client.websocket_connect("/ws") as ws:
                msg = ws.receive_json()
                assert msg["op"] == "list" and len(msg["points"]) == 2, msg

                # 触发一次写，期待经 NOTIFY 收到 update
                client.post("/api/points/10/set", json={"value": 123})
                update = None
                for _ in range(100):
                    m = ws.receive_json()
                    if m.get("op") == "update":
                        update = m
                        break
                assert update is not None, "no update received over WS"
                assert update["id"] == 10, update
                assert update["pointId"] == "AHU_01.Supply_Temp", update  # 点名对齐 HmiPointValueDto
                assert update["value"] == 123, update

        print("E2E OK: REST(list/get/set/meta) + WebSocket(list/update) all passed")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
