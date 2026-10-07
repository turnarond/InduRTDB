#!/usr/bin/env bash
# 端到端（v3.3 T6）: host 多进程混合角色场景 + 容器模拟占位
#
# 覆盖链路：
#   驱动进程(indurtdb-client 写) -> rtdbd(写唯一入口) -> 共享内存
#     -> 控制逻辑(核心库直读) / HMI(跨进程变更通知)
#
# 用法:
#   bash scripts/run_e2e.sh
#   E2E_BUILD_DIR=build-release bash scripts/run_e2e.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${E2E_BUILD_DIR:-$REPO_ROOT/build}"
FAIL_COUNT=0

echo "============================================"
echo " InduRTDB End-to-End (v3.3 T6)"
echo "============================================"
echo "REPO_ROOT: $REPO_ROOT"
echo "BUILD_DIR: $BUILD_DIR"
echo ""

# ---- 清理上一轮残留：孤儿守护进程 / socket / 共享内存段 ----
# 警告：本脚本须在隔离环境（容器/专用 CI 主机）运行；以下仅清理本测试拉起的 rtdbd
# （其命令行含本测试的 socket 路径 /tmp/indurtdb_e2e），避免误杀主机上其它真实 rtdbd 服务。
cleanup() {
    for pid in $(pgrep -x rtdbd 2>/dev/null); do
        if tr '\0' ' ' < /proc/"$pid"/cmdline 2>/dev/null | grep -q '/tmp/indurtdb_e2e'; then
            echo "[cleanup] killing rtdbd pid $pid (our e2e instance)"
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    rm -f /tmp/indurtdb_e2e_*.sock /tmp/e2e_policy_*.txt 2>/dev/null || true
    rm -f /dev/shm/indurtdb_e2e_test_* 2>/dev/null || true
}
trap cleanup EXIT
cleanup

# ---- [1/4] 构建 ----
echo "== [1/4] Building =="
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null
fi
cmake --build "$BUILD_DIR" -j"$(nproc)" > /dev/null
echo "   build OK"
echo ""

# ---- [2/4] 端到端混合角色用例 ----
echo "== [2/4] Running mixed-role E2E cases =="
E2E_BIN="$BUILD_DIR/tests/integration/test_e2e_mixed"
set +e
if [ -x "$E2E_BIN" ]; then
    "$E2E_BIN"
    E2E_RC=$?
else
    echo "   test_e2e_mixed not built (rtdbd/irtcli disabled?) — skip"
    E2E_RC=0
fi
set -e
echo ""

# ---- [3/3] 相关集成用例回归（服务 / 客户端 / 通知 / 自恢复 / 协议 v2 / 诊断） ----
echo "== [3/3] Regression: rtdbd + client integration =="
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ -x "$BUILD_DIR/tests/integration/test_rtdbd_core" ]; then
    set +e
    ( cd "$BUILD_DIR" && ctest -R "test_rtdbd_core|test_rtdbd_recovery|test_rtdbd_notify|test_irtcli_queue|test_rtdbd_proto_v2" --output-on-failure )
    REG_RC=$?
    set -e
else
    echo "   rtdbd integration tests not built — skip"
    REG_RC=0
fi
echo ""

# ---- [4/4] 单元级集成回归（诊断工具 / 协议 / 索引 / 元数据 / 语义） ----
echo "== [4/4] Regression: unit integration (diag/proto/meta) =="
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ -x "$BUILD_DIR/tests/unit/test_c_diag" ]; then
    set +e
    ( cd "$BUILD_DIR" && ctest -R "test_c_diag|test_c_api_v2|test_c_index|test_c_quality|test_c_data_model" --output-on-failure )
    UNIT_RC=$?
    set -e
else
    echo "   unit integration tests not built — skip"
    UNIT_RC=0
fi
echo ""

# ---- 汇总 ----
echo "============================================"
echo " SUMMARY"
echo "============================================"
echo "  e2e_mixed:  $([ $E2E_RC -eq 0 ] && echo PASS || echo FAIL)"
echo "  regression: $([ $REG_RC -eq 0 ] && echo PASS || echo FAIL)"
echo "  unit_reg:   $([ $UNIT_RC -eq 0 ] && echo PASS || echo FAIL)"
echo ""
[ $E2E_RC -ne 0 ] && FAIL_COUNT=$((FAIL_COUNT + 1))
[ $REG_RC -ne 0 ] && FAIL_COUNT=$((FAIL_COUNT + 1))
[ $UNIT_RC -ne 0 ] && FAIL_COUNT=$((FAIL_COUNT + 1))

if [ $FAIL_COUNT -gt 0 ]; then
    echo "RESULT: $FAIL_COUNT stage(s) FAILED"
    exit 1
fi

echo "RESULT: ALL PASS"
exit 0
