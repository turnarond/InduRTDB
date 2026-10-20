#!/usr/bin/env python3
"""rtdb-monitor 分区能力端到端测试：启真实 rtdbd + FastAPI TestClient。

覆盖 T7 监控台分区的三个用例：
  - Monitor.PartitionsEndpoint        GET /api/partitions 返回含 0 与 1 的列表
  - Monitor.PointsFilteredByPartition GET /api/points?partition=1 仅列分区 1；
                                      GET /api/points（无参）仍列全部（含分区 0）
  - Monitor.ViewRenders               前端源码契约（占位已移除 + 分区下拉 + API 接线）

策略文件用 uid:*:...（分区通配）——否则分区 1 的管控写被拒。
分区 0 点位来自核心配置；分区 1 点位运行期经 RtdbClient.create_point(partition=1) 建。
用法：python3 tests/test_partitions.py
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
TOOL = os.path.dirname(HERE)                       # tools/rtdb-monitor
REPO = os.path.dirname(os.path.dirname(TOOL))       # 仓库根
RTDBD = os.path.join(REPO, "build", "rtdbd", "rtdbd")

CFG_YAML = """\
points:
  - name: AHU_01.Supply_Temp
    id: 10
    type: int32
    access: 3
  - name: Pump_Start_CMD
    id: 11
    type: bool
    access: 3
"""


def ensure_built() -> None:
    if os.path.exists(RTDBD):
        return
    print("[partitions] building rtdbd ...")
    subprocess.run(["cmake", "-S", REPO, "-B", os.path.join(REPO, "build"),
                    "-DCMAKE_BUILD_TYPE", "Release"], check=True)
    subprocess.run(["cmake", "--build", os.path.join(REPO, "build"),
                    "-j", str(os.cpu_count() or 1)], check=True)


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def case_partitions_endpoint(client) -> None:
    """Monitor.PartitionsEndpoint：/api/partitions 返回分区列表，含 0 与 1。"""
    r = client.get("/api/partitions")
    assert r.status_code == 200, r.text
    parts = r.json()["partitions"]
    assert isinstance(parts, list), parts
    assert 0 in parts, f"partition 0 missing: {parts}"
    assert 1 in parts, f"partition 1 missing: {parts}"
    # 升序去重
    assert parts == sorted(set(parts)), f"not sorted/dedup: {parts}"
    print("OK Monitor.PartitionsEndpoint (/api/partitions -> %s)" % parts)


def case_points_filtered_by_partition(client) -> None:
    """Monitor.PointsFilteredByPartition：分区过滤 + 无参列全部。"""
    r = client.get("/api/points?partition=1")
    assert r.status_code == 200, r.text
    p1 = r.json()
    assert p1, "partition 1 list empty"
    assert all(p.get("partition") == 1 for p in p1), p1
    ids1 = {p["id"] for p in p1}
    assert {20, 21} <= ids1, f"partition-1 points missing: {ids1}"
    assert not ({10, 11} & ids1), f"partition-0 points leaked into filter: {ids1}"

    # 无参 → 列出全部（保留 v3.8 行为），含分区 0 与分区 1
    r = client.get("/api/points")
    assert r.status_code == 200, r.text
    allp = r.json()
    parts = {p.get("partition") for p in allp}
    assert 0 in parts and 1 in parts, f"all-list missing a partition: {parts}"
    assert 10 in {p["id"] for p in allp}, "partition-0 point 10 missing from all-list"
    print("OK Monitor.PointsFilteredByPartition (?partition=1 -> ids=%s; all -> %d pts)"
          % (sorted(ids1), len(allp)))


def case_view_renders() -> None:
    """Monitor.ViewRenders：前端源码契约（占位移除 + 分区下拉 + API 接线）。

    前端由 T7 前端任务落地（本后端任务不改 web/**）；此处仅做源码契约断言，
    真实渲染由编排者用浏览器截图留档。
    """
    app_vue = _read(os.path.join(TOOL, "web", "src", "App.vue"))
    api_js = _read(os.path.join(TOOL, "web", "src", "api.js"))
    table_vue = _read(os.path.join(TOOL, "web", "src", "components", "PointTable.vue"))

    # 1) partitions 占位文案已移除（不再显示「v3.7 规划」）
    assert "v3.7 规划" not in app_vue, "partitions 占位文案仍在 App.vue"
    # 2) 分区下拉：绑定 selectedPartition 的 el-select
    assert "<el-select" in app_vue and re.search(r'v-model="selectedPartition"', app_vue), \
        "App.vue 缺分区下拉（v-model=selectedPartition）"
    # 3) API 接线：listPartitions + listPoints(<partition>)
    assert "listPartitions" in api_js, "api.js 缺 listPartitions"
    assert re.search(r"listPoints\s*[:=]\s*\(\s*partition", api_js), \
        "api.js 的 listPoints 未接受 partition 参数"
    # 4) 点位表呈现 partition
    assert "partition" in table_vue, "PointTable.vue 未呈现 partition 字段"
    print("OK Monitor.ViewRenders (占位已移除 + 分区下拉 + listPoints/listPartitions 接线)")


def main() -> None:
    ensure_built()
    tmp = tempfile.mkdtemp()
    sock = os.path.join(tmp, "rtdbd.sock")
    cfg = os.path.join(tmp, "cfg.yaml")
    pol = os.path.join(tmp, "policy.txt")
    with open(cfg, "w") as f:
        f.write(CFG_YAML)
    # 分区位用 * 通配，否则分区 1 的 CREATE_POINT 会被策略拒绝。
    with open(pol, "w") as f:
        f.write(f"{os.getuid()}:*:0:63\n")

    proc = subprocess.Popen(
        [RTDBD, "--instance", "partitions", "--socket", sock,
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
        from rtdb_client import RtdbClient
        from fastapi.testclient import TestClient
        import app as appmod

        # 运行期建分区 1 点位（核心配置无逐点分区字段）。
        with RtdbClient(sock) as c:
            c.create_point(20, "temp", "int32", 3, partition=1)
            c.create_point(21, "press", "int32", 3, partition=1)

        with TestClient(appmod.app) as client:
            case_partitions_endpoint(client)
            case_points_filtered_by_partition(client)

        case_view_renders()

        print("PARTITIONS OK: partitions endpoint + partition filter + view contract")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
