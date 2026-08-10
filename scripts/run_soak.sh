#!/usr/bin/env bash
# soak + 故障注入测试: 编译并运行 InduRTDB 稳定性与崩溃恢复验证
#
# 用法:
#   bash scripts/run_soak.sh              # 默认 soak 时长 10 秒
#   SOAK_DURATION=60 bash scripts/run_soak.sh   # 自定义时长
#   bash scripts/run_soak.sh 60           # 同上 (位置参数)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SOAK_DIR="$REPO_ROOT/tests/soak"
SOAK_DURATION="${1:-${SOAK_DURATION:-10}}"
FAIL_COUNT=0

echo "============================================"
echo " InduRTDB Soak + Fault Injection Tests"
echo "============================================"
echo "REPO_ROOT:     $REPO_ROOT"
echo "SOAK_DURATION: ${SOAK_DURATION}s"
echo ""

# ---- 清理上次运行残留的共享内存段 ----
cleanup_shm() {
    for name in /dev/shm/indurtdb_soak_test /dev/shm/indurtdb_fi_test; do
        if [ -e "$name" ]; then
            echo "[cleanup] removing stale shm: $name"
            rm -f "$name"
        fi
    done
}
trap cleanup_shm EXIT
cleanup_shm

# ---- [1/4] 构建 libindurtdb.a (如需要) ----
echo "== [1/4] Building libindurtdb.a =="
if [ ! -f "$BUILD_DIR/libindurtdb.a" ]; then
    echo "   Library not found, running cmake build..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$BUILD_DIR" -j"$(nproc)" > /dev/null
    echo "   Build complete."
else
    echo "   Library already built: $BUILD_DIR/libindurtdb.a"
fi
echo ""

# ---- [2/4] 编译测试程序 ----
echo "== [2/4] Compiling test programs =="
mkdir -p "$BUILD_DIR/soak"

CC="${CC:-gcc}"
CFLAGS="-std=gnu11 -Wall -Wextra -O2 -I$REPO_ROOT/include"
LDFLAGS="-L$BUILD_DIR -lindurtdb -lpthread -lrt -lm"

$CC $CFLAGS "$SOAK_DIR/soak_test.c" $LDFLAGS \
    -o "$BUILD_DIR/soak/soak_test"
echo "   soak_test compiled OK"

$CC $CFLAGS "$SOAK_DIR/fault_injection_test.c" $LDFLAGS \
    -o "$BUILD_DIR/soak/fault_injection_test"
echo "   fault_injection_test compiled OK"
echo ""

# ---- [3/4] 运行 soak 测试 ----
echo "== [3/4] Running soak test (${SOAK_DURATION}s) =="
set +e
"$BUILD_DIR/soak/soak_test" "$SOAK_DURATION"
SOAK_RC=$?
set -e

if [ $SOAK_RC -eq 0 ]; then
    echo ">> soak_test: PASS"
else
    echo ">> soak_test: FAIL (exit $SOAK_RC)"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi
echo ""

# ---- [4/4] 运行故障注入测试 ----
echo "== [4/4] Running fault injection test =="
set +e
"$BUILD_DIR/soak/fault_injection_test"
FI_RC=$?
set -e

if [ $FI_RC -eq 0 ]; then
    echo ">> fault_injection_test: PASS"
else
    echo ">> fault_injection_test: FAIL (exit $FI_RC)"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi
echo ""

# ---- 汇总 ----
echo "============================================"
echo " SUMMARY"
echo "============================================"
echo "  soak_test:            $([ $SOAK_RC -eq 0 ] && echo PASS || echo FAIL)"
echo "  fault_injection_test: $([ $FI_RC -eq 0 ] && echo PASS || echo FAIL)"
echo ""

if [ $FAIL_COUNT -gt 0 ]; then
    echo "RESULT: $FAIL_COUNT test(s) FAILED"
    exit 1
fi

echo "RESULT: ALL PASS"
exit 0
