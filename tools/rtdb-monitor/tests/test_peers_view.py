#!/usr/bin/env python3
"""v3.12 M3.2 ⑥：集群成员可视化端到端 + 前端源码契约测试。

覆盖：
  - Monitor.PeersEndpoint     GET /api/peers 返回真实成员表 + 预留字段说明
  - Monitor.PeersTerminalCmd  /api/cmd 的 peers set / peers get / help
  - Monitor.PeersViewRenders  前端源码契约（菜单 + 组件 + API 接线 + 字段标注）

策略同 test_partitions.py：启真实 rtdbd + FastAPI TestClient（不 mock 数据库）。
用法：python3 tests/test_peers_view.py
"""
import os
import re
import sys
import signal
import shutil
import subprocess
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(TOOL))
RTDBD = os.path.join(REPO, "build", "rtdbd", "rtdbd")


def _read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def case_peers_endpoint(client) -> None:
    """GET /api/peers：真实成员表 + 预留字段诚实标注。"""
    r = client.get("/api/peers")
    assert r.status_code == 200, f"/api/peers -> {r.status_code}: {r.text[:120]}"
    data = r.json()
    assert "peers" in data, "响应缺 peers 字段"
    assert "notes" in data, "响应缺 notes（预留字段说明）"
    assert "预留" in data["notes"] or "reserved" in data["notes"].lower(), \
        "notes 未标注 boot_id/applied_offset/lag 为协议预留字段"
    assert isinstance(data["peers"], list)

    # 经终端设置 2 个成员，再读端点：必须是真实 rtdbd 状态
    r2 = client.post("/api/cmd", json={"cmd": "peers set 10.40.0.1 10.40.0.2"})
    assert r2.status_code == 200
    out2 = r2.json().get("output", [])
    assert not any(str(x).startswith("ERR:") for x in out2), f"peers set 失败: {out2}"

    r3 = client.get("/api/peers")
    assert r3.status_code == 200
    peers = r3.json()["peers"]
    assert len(peers) == 2, f"expected 2 peers, got {peers}"
    assert peers[0]["addr"] == "10.40.0.1"
    for key in ("node_id", "state", "boot_id", "applied_offset", "lag"):
        assert key in peers[0], f"成员缺字段 {key}"
    print("OK Monitor.PeersEndpoint (真实成员表 + 预留字段说明)")


def case_peers_terminal_cmd(client) -> None:
    """/api/cmd 的 peers get 与 help 接线。"""
    r = client.post("/api/cmd", json={"cmd": "peers get"})
    assert r.status_code == 200
    out = r.json().get("output", [])
    assert any("10.40.0.1" in str(x) for x in out), f"peers get 输出未含成员: {out}"
    h = client.post("/api/cmd", json={"cmd": "help"}).json().get("output", [])
    assert any("peers" in str(x) for x in h), "help 未列出 peers 命令"
    print("OK Monitor.PeersTerminalCmd (peers get + help)")


def case_channels_probe(client) -> None:
    """Monitor.ChannelsProbe：channels 必须真实探测端口文件，不得臆测。"""
    saved = os.environ.pop("RTDBD_VSOA_PORT_FILE", None)
    try:
        d = client.get("/api/peers").json()
        assert d["channels"]["vsoa_enabled"] is False, "未提供端口文件却报告 VSOA 已启用"
        assert d["channels"]["source"] == "unset"

        fd, path = tempfile.mkstemp(prefix="vsoa_port_")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write("43210\n")
        os.environ["RTDBD_VSOA_PORT_FILE"] = path
        try:
            d2 = client.get("/api/peers").json()
            assert d2["channels"]["vsoa_enabled"] is True
            assert d2["channels"]["vsoa_port"] == 43210
        finally:
            os.environ.pop("RTDBD_VSOA_PORT_FILE", None)
            os.unlink(path)
        print("OK Monitor.ChannelsProbe (端口文件真实探测)")
    finally:
        if saved is not None:
            os.environ["RTDBD_VSOA_PORT_FILE"] = saved


def case_peers_view_renders() -> None:
    """前端源码契约（照 test_partitions.py 的 case_view_renders）。"""
    app_vue = _read(os.path.join(TOOL, "web", "src", "App.vue"))
    api_js = _read(os.path.join(TOOL, "web", "src", "api.js"))
    view_path = os.path.join(TOOL, "web", "src", "components", "PeersView.vue")
    assert os.path.isfile(view_path), "缺 PeersView.vue"
    view = _read(view_path)
    app_py = _read(os.path.join(TOOL, "app.py"))

    assert re.search(r'index="peers"', app_vue), "App.vue 缺集群成员菜单项"
    assert "<PeersView" in app_vue, "App.vue 未渲染 PeersView"
    assert re.search(r"getPeers\s*[:=]", api_js), "api.js 缺 getPeers 接线"
    assert re.search(r"/api/peers", app_py), "app.py 缺 /api/peers 端点"
    for field in ("addr", "node_id", "state", "boot_id", "applied_offset", "lag"):
        assert field in view, f"PeersView.vue 未呈现字段 {field}"
    assert "预留" in view or "reserved" in view.lower(), \
        "PeersView.vue 未标注预留字段（不得假装有同步状态）"
    assert "--sync-self-addr" in view, "PeersView.vue 未提示本节点地址（拒绝自地址）语义"
    assert "vsoa_enabled" in view, "PeersView.vue 未展示 VSOA 通道状态"
    assert "_vsoa_channel_info" in app_py, "app.py 未真实探测 VSOA 端口文件"
    assert "/sync/peers" in app_py, "app.py 未标注 VSOA 管控入口"
    assert "sync_admin" in view, "PeersView.vue 未标注管控角色约束"
    print("OK Monitor.PeersViewRenders (菜单 + 组件 + API + 字段标注)")


def main() -> None:
    assert os.path.isfile(RTDBD), f"rtdbd 未构建: {RTDBD}"
    tmp = tempfile.mkdtemp(prefix="peers_view_")
    sock = os.path.join(tmp, "rtdbd.sock")
    cfg = os.path.join(tmp, "cfg.yaml")
    pol = os.path.join(tmp, "policy.txt")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("points: []\n")
    with open(pol, "w", encoding="utf-8") as f:
        f.write(f"{os.getuid()}:*:0:63\n")

    proc = subprocess.Popen(
        [RTDBD, "--instance", "peers_view", "--socket", sock,
         "--config", cfg, "--policy", pol, "--max-points", "64"])
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
            case_peers_endpoint(client)
            case_peers_terminal_cmd(client)
            case_channels_probe(client)
        case_peers_view_renders()
        print("PEERS VIEW OK: endpoint + terminal + view contract")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
