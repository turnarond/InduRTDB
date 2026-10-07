#!/usr/bin/env bash
# rtdb-monitor 冒烟：启真实 rtdbd → 用 Python 客户端 list/set/get 往返 → 停止。
# 不依赖 fastapi（仅验证协议客户端与 rtdbd 通道），CI 友好。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${HERE}/../.."
BUILD="${REPO}/build"
RTDBD="${BUILD}/rtdbd/rtdbd"

cleanup() { [ -n "${RTDBD_PID:-}" ] && kill "${RTDBD_PID}" 2>/dev/null || true; rm -rf "${TMP:-}"; }
trap cleanup EXIT

# 确保 rtdbd 已构建
if [ ! -x "${RTDBD}" ]; then
  echo "[smoke] building rtdbd ..."
  cmake -S "${REPO}" -B "${BUILD}" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
  cmake --build "${BUILD}" -j"$(nproc)" >/dev/null 2>&1
fi

TMP="$(mktemp -d)"
SOCK="${TMP}/rtdbd.sock"
CFG="${TMP}/cfg.yaml"
POL="${TMP}/policy.txt"

cat > "${CFG}" <<'YAML'
points:
  - name: AHU_01.Supply_Temp
    id: 10
    type: int32
    access: read_write
  - name: Pump_Start_CMD
    id: 11
    type: bool
    access: read_write
YAML

printf '%s:0:63\n' "$(id -u)" > "${POL}"

echo "[smoke] starting rtdbd ..."
# 后台启动（rtdbd 自身不识别 --daemon，前台会阻塞脚本）；$! 即其 PID。
"${RTDBD}" --instance monitor-smoke --socket "${SOCK}" --config "${CFG}" --policy "${POL}" --max-points 64 &
RTDBD_PID=$!
# 轮询等待 socket 就绪
for _ in $(seq 1 50); do [ -S "${SOCK}" ] && break; sleep 0.1; done
[ -S "${SOCK}" ] || { echo "[smoke] FAIL: rtdbd socket not ready"; kill "${RTDBD_PID}" 2>/dev/null; exit 1; }

export RTDBD_SOCK="${SOCK}"
echo "[smoke] running client round-trip ..."
python3 - "${HERE}" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from rtdb_client import RtdbClient, TYPE_INT32, TYPE_BOOL

with RtdbClient(__import__("os").environ["RTDBD_SOCK"]) as c:
    assert c.ping(), "ping failed"
    pts = c.list_points()
    assert len(pts) == 2, f"expected 2 points, got {len(pts)}"
    ids = {p["name"]: p["id"] for p in pts}
    assert ids.get("AHU_01.Supply_Temp") == 10
    # 写 int32 再读回
    c.write_point(10, TYPE_INT32, -12345)
    v = c.get_point(10)["value"]
    assert v == -12345, f"get after write mismatch: {v}"
    # 按名查找
    assert c.find_by_name("Pump_Start_CMD") == 11
    print("[smoke] OK: ping/list/write/get/find all passed")
PY
