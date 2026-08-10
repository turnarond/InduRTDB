#!/usr/bin/env bash
# InduRTDB 性能基准测试: 编译 bench.c 并在 x86 上运行, 输出延迟分布 + 吞吐报告
#
# 用法:
#   bash scripts/run_bench.sh                    # 默认参数
#   bash scripts/run_bench.sh --quick            # 快速模式 (减少迭代)
#   WARMUP=5000 ITERS=50000 bash scripts/run_bench.sh  # 自定义参数
#
# 环境变量 (可选):
#   CC          编译器 (默认 gcc)
#   WARMUP      预热迭代次数
#   ITERS       测量迭代次数
#   BATCH_CNT   批量操作点数
#   TP_SEC      吞吐测试时长 (秒)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
BENCH_DIR="$REPO_ROOT/tests/bench"

# ---- 参数 ----
QUICK_MODE=false
if [[ "${1:-}" == "--quick" ]]; then
    QUICK_MODE=true
fi

if $QUICK_MODE; then
    WARMUP="${WARMUP:-5000}"
    ITERS="${ITERS:-50000}"
    BATCH_CNT="${BATCH_CNT:-20}"
    TP_SEC="${TP_SEC:-1}"
else
    WARMUP="${WARMUP:-20000}"
    ITERS="${ITERS:-200000}"
    BATCH_CNT="${BATCH_CNT:-100}"
    TP_SEC="${TP_SEC:-2}"
fi

echo "============================================"
echo " InduRTDB Performance Benchmark (x86)"
echo "============================================"
echo "REPO_ROOT:     $REPO_ROOT"
echo "WARMUP:        $WARMUP"
echo "ITERATIONS:    $ITERS"
echo "BATCH_COUNT:   $BATCH_CNT"
echo "THROUGHPUT_SEC: ${TP_SEC}s"
echo ""

# ---- 清理上次运行残留的共享内存段 ----
cleanup_shm() {
    if [ -e /dev/shm/indurtdb_bench ]; then
        echo "[cleanup] removing stale shm: /dev/shm/indurtdb_bench"
        rm -f /dev/shm/indurtdb_bench
    fi
}
trap cleanup_shm EXIT
cleanup_shm

# ---- [1/3] 构建 libindurtdb.a (如需要) ----
echo "== [1/3] Building libindurtdb.a =="
if [ ! -f "$BUILD_DIR/libindurtdb.a" ]; then
    echo "   Library not found, running cmake build..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$BUILD_DIR" -j"$(nproc)" > /dev/null
    echo "   Build complete."
else
    echo "   Library already built: $BUILD_DIR/libindurtdb.a"
fi
echo ""

# ---- [2/3] 编译基准程序 ----
echo "== [2/3] Compiling bench.c =="
mkdir -p "$BUILD_DIR/bench"

CC="${CC:-gcc}"
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -O2 -I$REPO_ROOT/include"
LDFLAGS="-L$BUILD_DIR -lindurtdb -lpthread -lrt -lm"

# 编译期参数通过 -D 传入
DEFINES="-DBENCH_WARMUP=$WARMUP -DBENCH_ITERATIONS=$ITERS"
DEFINES="$DEFINES -DBENCH_BATCH_COUNT=$BATCH_CNT"
DEFINES="$DEFINES -DBENCH_THROUGHPUT_SEC=$TP_SEC"

echo "   $CC $CFLAGS $DEFINES $BENCH_DIR/bench.c $LDFLAGS -o $BUILD_DIR/bench/bench"
$CC $CFLAGS $DEFINES "$BENCH_DIR/bench.c" $LDFLAGS \
    -o "$BUILD_DIR/bench/bench"
echo "   bench compiled OK (zero warnings)"
echo ""

# ---- [3/3] 运行基准 ----
echo "== [3/3] Running benchmark =="
"$BUILD_DIR/bench/bench"
RC=$?
echo ""

if [ $RC -eq 0 ]; then
    echo ">> bench: PASS (exit 0)"
else
    echo ">> bench: FAIL (exit $RC)"
    exit $RC
fi

echo ""
echo "RESULT: ALL PASS"
exit 0
