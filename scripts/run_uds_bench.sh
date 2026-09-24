#!/usr/bin/env bash
# UDS 往返延迟基准: 编译并运行 tests/bench/uds_latency.c
#
# 用途: v3.3 架构 T0 门槛验证 —— 确认 UDS 往返延迟落在 5–15 μs 预算内。
#       若不达标, 应停止投入并回到方案评审。
#
# 用法:
#   bash scripts/run_uds_bench.sh                 # 默认参数
#   bash scripts/run_uds_bench.sh --quick         # 快速模式 (减少迭代)
#   WARMUP=5000 ITERS=50000 MSG_SIZE=48 bash scripts/run_uds_bench.sh
#
# 环境变量 (可选):
#   CC        编译器 (默认 gcc)
#   WARMUP    预热迭代次数
#   ITERS     测量迭代次数
#   MSG_SIZE  请求/响应字节数 (默认 16; 真实写请求建议用 48)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
BENCH_DIR="$REPO_ROOT/tests/bench"

QUICK_MODE=false
if [[ "${1:-}" == "--quick" ]]; then
    QUICK_MODE=true
fi

if $QUICK_MODE; then
    WARMUP="${WARMUP:-5000}"
    ITERS="${ITERS:-50000}"
else
    WARMUP="${WARMUP:-20000}"
    ITERS="${ITERS:-200000}"
fi

MSG_SIZE="${MSG_SIZE:-16}"

echo "============================================"
echo " InduRTDB UDS Latency Benchmark (T0 gate)"
echo "============================================"
echo "REPO_ROOT:    $REPO_ROOT"
echo "WARMUP:       $WARMUP"
echo "ITERATIONS:   $ITERS"
echo "MSG_SIZE:     $MSG_SIZE B"
echo ""

cleanup_sock() {
    if [ -e /tmp/indurtdb_uds_bench.sock ]; then
        rm -f /tmp/indurtdb_uds_bench.sock
    fi
}
trap cleanup_sock EXIT
cleanup_sock

echo "== [1/2] Compiling uds_latency.c =="
mkdir -p "$BUILD_DIR/bench"
CC="${CC:-gcc}"
"$CC" -std=gnu11 -Wall -Wextra -Werror -O2 \
    -DBENCH_WARMUP="$WARMUP" -DBENCH_ITERATIONS="$ITERS" \
    -DBENCH_MSG_SIZE="$MSG_SIZE" \
    "$BENCH_DIR/uds_latency.c" -o "$BUILD_DIR/bench/uds_latency"
echo "   compiled OK (zero warnings)"
echo ""

echo "== [2/2] Running benchmark =="
"$BUILD_DIR/bench/uds_latency"
RC=$?
echo ""

exit $RC
