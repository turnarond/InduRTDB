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
    rm -f /dev/shm/indurtdb_e2e_v2src /dev/shm/indurtdb_e2e_v3dst 2>/dev/null || true
    rm -f /tmp/indurtdb_e2e_mig.sock 2>/dev/null || true
}
trap cleanup EXIT
cleanup

# ---- [1/5] 构建 ----
echo "== [1/5] Building =="
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null
fi
cmake --build "$BUILD_DIR" -j"$(nproc)" > /dev/null
echo "   build OK"
echo ""

# ---- [2/4] 端到端混合角色用例 ----
echo "== [2/5] Running mixed-role E2E cases =="
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

# ---- [3/5] 相关集成用例回归（服务 / 客户端 / 通知 / 自恢复 / 协议 v2+v3 / 诊断） ----
echo "== [3/5] Regression: rtdbd + client integration =="
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && [ -x "$BUILD_DIR/tests/integration/test_rtdbd_core" ]; then
    set +e
    ( cd "$BUILD_DIR" && ctest -R "test_rtdbd_core|test_rtdbd_recovery|test_rtdbd_notify|test_irtcli_queue|test_rtdbd_proto_v2|test_rtdbd_proto_v3|test_irtcli_cli" --output-on-failure )
    REG_RC=$?
    set -e
else
    echo "   rtdbd integration tests not built — skip"
    REG_RC=0
fi
echo ""

# ---- [4/5] 单元级集成回归（诊断工具 / 协议 / 索引 / 元数据 / 语义） ----
echo "== [4/5] Regression: unit integration (diag/proto/meta) =="
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

# ---- [5/5] 端到端：分区 + 协议 v3 + 迁移后场景 ----
# 真实链路：合成 v2 段 -> irt_migrate 迁移到 v3 -> rtdbd 挂载 -> irtcli list 校验点位。
# 该阶段是发布门禁，缺少二进制时硬失败（不得跳过）。
run_migration_stage() {
    local mig_sock="/tmp/indurtdb_e2e_mig.sock"
    local rtdbd_log="/tmp/indurtdb_e2e_mig_rtdbd.log"
    local pid="" list_out="" info_out="" listed=0
    local expected_names="v2_tag_000 v2_tag_001 v2_tag_002"

    if [ ! -x "$BUILD_DIR/rtdbd/rtdbd" ] || [ ! -x "$BUILD_DIR/client/irtcli" ]; then
        echo "   ERROR: 缺少 $BUILD_DIR/rtdbd/rtdbd 或 $BUILD_DIR/client/irtcli"
        echo "          迁移后场景是发布门禁，不能跳过"
        return 1
    fi

    rm -f "$mig_sock" "$rtdbd_log"
    rm -f /dev/shm/indurtdb_e2e_v2src /dev/shm/indurtdb_e2e_v3dst 2>/dev/null || true

    echo "   -> 生成合成 v2 源段 (e2e_v2src)"
    if ! python3 "$REPO_ROOT/tools/irt_migrate/tests/make_v2_fixture.py" \
            --id e2e_v2src --max-points 16 --max-subs 2 --names 3; then
        echo "   ASSERT FAIL: 无法生成 v2 源段"
        return 1
    fi

    echo "   -> 迁移 v2 -> v3 (e2e_v2src -> e2e_v3dst)"
    if ! ( cd "$REPO_ROOT/tools/irt_migrate" && \
           python3 -m irt_migrate migrate --src e2e_v2src --dst e2e_v3dst --force ); then
        echo "   ASSERT FAIL: irt_migrate migrate 失败"
        return 1
    fi

    echo "   -> 校验目标段版本"
    info_out="$( cd "$REPO_ROOT/tools/irt_migrate" && \
                 python3 -m irt_migrate info e2e_v3dst )" || {
        echo "   ASSERT FAIL: irt_migrate info 失败"
        return 1
    }
    if ! printf '%s\n' "$info_out" | grep -q '"version": 3'; then
        echo "   ASSERT FAIL: 迁移后段不是 version 3"
        printf '%s\n' "$info_out"
        return 1
    fi
    echo "   version=3 OK"

    echo "   -> 启动 rtdbd 挂载迁移后段"
    "$BUILD_DIR/rtdbd/rtdbd" --socket "$mig_sock" --instance e2e_v3dst \
        --max-points 16 --max-subs 2 > "$rtdbd_log" 2>&1 &
    pid=$!

    # 有界重试：直到 socket 真正可接受 list 请求（约 5s）
    local i=0
    while [ "$i" -lt 50 ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "   ASSERT FAIL: rtdbd 启动后退出"
            cat "$rtdbd_log" 2>/dev/null || true
            return 1
        fi
        if list_out="$("$BUILD_DIR/client/irtcli" --sock "$mig_sock" list 2>/dev/null)"; then
            break
        fi
        i=$((i + 1))
        sleep 0.1
    done
    if [ "$i" -ge 50 ]; then
        echo "   ASSERT FAIL: rtdbd socket 在 5s 内未就绪"
        cat "$rtdbd_log" 2>/dev/null || true
        kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        return 1
    fi

    echo "   -> irtcli list 输出:"
    printf '%s\n' "$list_out" | sed 's/^/      /'

    local rc=0
    for name in $expected_names; do
        if ! printf '%s\n' "$list_out" | grep -qE "^[0-9]+ [0-9]+ ${name}$"; then
            echo "   ASSERT FAIL: list 缺少点位 $name"
            rc=1
        fi
    done
    listed="$(printf '%s\n' "$list_out" | grep -cE '^[0-9]+ [0-9]+ v2_tag_[0-9]{3}$' || true)"
    if [ "$listed" -ne 3 ]; then
        echo "   ASSERT FAIL: 期望 3 个种子点位，实际 $listed"
        rc=1
    fi

    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$mig_sock" "$rtdbd_log"
    rm -f /dev/shm/indurtdb_e2e_v2src /dev/shm/indurtdb_e2e_v3dst 2>/dev/null || true

    if [ "$rc" -ne 0 ]; then
        return 1
    fi
    echo "   migrated segment E2E OK"
    return 0
}

echo "== [5/5] 端到端：分区 + 协议 v3 + 迁移后场景 =="
set +e
run_migration_stage
MIG_RC=$?
set -e
echo ""

# ---- 汇总 ----
echo "============================================"
echo " SUMMARY"
echo "============================================"
echo "  e2e_mixed:  $([ $E2E_RC -eq 0 ] && echo PASS || echo FAIL)"
echo "  regression: $([ $REG_RC -eq 0 ] && echo PASS || echo FAIL)"
echo "  unit_reg:   $([ $UNIT_RC -eq 0 ] && echo PASS || echo FAIL)"
echo "  migration:  $([ $MIG_RC -eq 0 ] && echo PASS || echo FAIL)"
echo ""
[ $E2E_RC -ne 0 ] && FAIL_COUNT=$((FAIL_COUNT + 1))
[ $REG_RC -ne 0 ] && FAIL_COUNT=$((FAIL_COUNT + 1))
[ $UNIT_RC -ne 0 ] && FAIL_COUNT=$((FAIL_COUNT + 1))
[ $MIG_RC -ne 0 ] && FAIL_COUNT=$((FAIL_COUNT + 1))

if [ $FAIL_COUNT -gt 0 ]; then
    echo "RESULT: $FAIL_COUNT stage(s) FAILED"
    exit 1
fi

echo "RESULT: ALL PASS"
exit 0
