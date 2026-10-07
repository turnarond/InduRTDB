#!/usr/bin/env bash
# rtdb-monitor 端到端冒烟（CI 入口）。
# 在已构建 rtdbd 的前提下，启动真实 rtdbd 并用纯 Python 客户端完成
# ping/list/write/get/find 往返，验证 v3.5 新增的 OP_GET/OP_LIST 协议与监控工具。
# 依赖 tools/rtdb-monitor/run_smoke.sh；rtdbd 缺失时自动构建。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${HERE}/.."
TOOL="${REPO}/tools/rtdb-monitor"

if [ ! -x "${REPO}/build/rtdbd/rtdbd" ]; then
  echo "[monitor-smoke] rtdbd not built; building ..."
  cmake -S "${REPO}" -B "${REPO}/build" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
  cmake --build "${REPO}/build" -j"$(nproc)" >/dev/null 2>&1
fi

echo "[monitor-smoke] running tools/rtdb-monitor/run_smoke.sh"
exec bash "${TOOL}/run_smoke.sh"
