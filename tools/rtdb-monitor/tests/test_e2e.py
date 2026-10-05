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
  - name: Line_Status
    id: 12
    type: string
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
        [RTDBD, "--instance", "e2e", "--socket", sock,
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
            assert len(pts) == 3, pts
            names = {p["name"]: p["id"] for p in pts}
            assert names.get("AHU_01.Supply_Temp") == 10
            assert names.get("Line_Status") == 12

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

            # ---- REST：字符串点位 GET（value_str 路径）----
            r = client.get("/api/points/12")
            assert r.status_code == 200, r.text
            assert isinstance(r.json()["value"], str), r.json()  # 字符串点位值为 str

            # ---- REST：元数据 ----
            r = client.get("/api/meta/10")
            assert r.status_code == 200, r.text
            assert "eurMin" in r.json()

            # ---- WebSocket：清单 + 实时 update ----
            with client.websocket_connect("/ws") as ws:
                msg = ws.receive_json()
                assert msg["op"] == "list" and len(msg["points"]) == 3, msg

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
                assert update["value"] == "123", update                   # value 字符串化对齐
                assert update["quality"] == "0", update                  # quality 字符串化对齐

            # ---- v3.6 点位 CRUD（REST）----
            r = client.post("/api/points", json={
                "id": 20, "name": "Test.New_Point", "type": "int32", "access": 3})
            assert r.status_code == 200, r.text
            assert any(p["id"] == 20 for p in client.get("/api/points").json())

            client.post("/api/points/20/set", json={"value": 55})
            assert client.get("/api/points/20").json()["value"] == 55

            r = client.post("/api/points/20/rename", json={"name": "Test.Renamed"})
            assert r.status_code == 200, r.text
            names = [p["name"] for p in client.get("/api/points").json()]
            assert "Test.Renamed" in names and "Test.New_Point" not in names, names

            r = client.delete("/api/points/20")
            assert r.status_code == 200, r.text
            assert not any(p["id"] == 20 for p in client.get("/api/points").json())

            # ---- v3.6 命令终端 ----
            def cmd(c):
                rr = client.post("/api/cmd", json={"cmd": c})
                assert rr.status_code == 200, rr.text
                return rr.json()["output"]

            assert cmd("ping") == ["PONG"]
            assert cmd("create 21 Term.CmdPoint int32") == ["OK"]
            assert cmd("set Term.CmdPoint 7") == ["OK"]
            assert cmd("get Term.CmdPoint")[0].startswith("7")
            assert cmd("find Term.CmdPoint") == ["21"]
            assert cmd("del Term.CmdPoint") == ["OK"]
            assert cmd("bogus")[0].startswith("未知命令")

        print("E2E OK: REST(list/get/set/meta/CRUD/cmd) + WebSocket(list/update) all passed")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
