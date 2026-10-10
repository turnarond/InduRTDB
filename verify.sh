#!/usr/bin/env bash
# =============================================================================
# InduRTDB 一键验证脚本 (verify.sh)
#
# 执行完整的本地验证流程：
#   1. 环境检查（cmake / g++ / gtest）
#   2. CMake 配置 + 全量编译（库 + 单元测试 + 冒烟测试）
#   3. 运行单元 / 集成测试（ctest）
#   4. 运行冒烟测试（smoke_lifecycle / smoke_c_api / smoke_multi_process）
#
# 用法:
#   ./verify.sh              # 完整验证
#   ./verify.sh --clean      # 清理 build 目录后重新验证
#   ./verify.sh --no-tests   # 仅编译，不运行测试
#   ./verify.sh --smoke-only # 仅运行冒烟测试（需已编译）
#
# 退出码: 0 = 全部通过, 非 0 = 存在失败
# =============================================================================

set -euo pipefail

# ---------- 颜色与输出工具 ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

log_info()  { printf "${CYAN}[INFO]${NC}  %s\n" "$*"; }
log_ok()    { printf "${GREEN}[ OK ]${NC}  %s\n" "$*"; }
log_warn()  { printf "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
log_fail()  { printf "${RED}[FAIL]${NC}  %s\n" "$*"; }

# ---------- 参数解析 ----------
DO_CLEAN=0
NO_TESTS=0
SMOKE_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --clean)      DO_CLEAN=1 ;;
        --no-tests)   NO_TESTS=1 ;;
        --smoke-only) SMOKE_ONLY=1 ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        *)
            log_warn "未知参数: $arg (忽略)"
            ;;
    esac
done

# ---------- 1. 环境检查 ----------
check_deps() {
    log_info "检查构建依赖..."
    local missing=0

    if ! command -v cmake >/dev/null 2>&1; then
        log_fail "cmake 未安装"
        missing=1
    else
        log_ok "cmake $(cmake --version | head -1 | awk '{print $3}')"
    fi

    if ! command -v g++ >/dev/null 2>&1; then
        log_fail "g++ 未安装"
        missing=1
    else
        log_ok "g++ $(g++ --version | head -1 | awk '{print $3}')"
    fi

    # GTest 检查（单元/集成测试需要，冒烟测试不需要）
    if [ "$SMOKE_ONLY" -eq 0 ] && [ "$NO_TESTS" -eq 0 ]; then
        if ls /usr/include/gtest/gtest.h >/dev/null 2>&1; then
            log_ok "GTest 头文件已安装"
        else
            log_warn "GTest 头文件未找到，单元/集成测试将被跳过（冒烟测试仍可运行）"
        fi
    fi

    if [ "$missing" -ne 0 ]; then
        log_fail "依赖缺失，中止验证"
        exit 1
    fi
}

# ---------- 2. 清理 ----------
clean_build() {
    if [ "$DO_CLEAN" -eq 1 ]; then
        log_info "清理 build 目录..."
        rm -rf "$BUILD_DIR"
        log_ok "build 目录已清理"
    fi
}

# ---------- 3. CMake 配置 ----------
cmake_configure() {
    log_info "CMake 配置..."
    mkdir -p "$BUILD_DIR"
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_TESTS=ON \
        -DBUILD_EXAMPLES=OFF \
        2>&1 | tee "${BUILD_DIR}/cmake_configure.log"

    if [ "${PIPESTATUS[0]}" -ne 0 ]; then
        log_fail "CMake 配置失败，详见 ${BUILD_DIR}/cmake_configure.log"
        exit 1
    fi
    log_ok "CMake 配置成功"
}

# ---------- 4. 编译 ----------
build_all() {
    log_info "编译全部目标..."
    cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1 | tee "${BUILD_DIR}/build.log"

    if [ "${PIPESTATUS[0]}" -ne 0 ]; then
        log_fail "编译失败，详见 ${BUILD_DIR}/build.log"
        exit 1
    fi
    log_ok "编译成功"

    # 验证产物
    if [ -f "${BUILD_DIR}/libindurtdb.a" ]; then
        log_ok "静态库已生成: libindurtdb.a ($(du -h "${BUILD_DIR}/libindurtdb.a" | cut -f1))"
    else
        log_fail "静态库未生成"
        exit 1
    fi
}

# ---------- 5. 运行单元/集成测试 ----------
run_unit_tests() {
    if [ "$NO_TESTS" -eq 1 ]; then
        log_warn "跳过单元/集成测试 (--no-tests)"
        return 0
    fi

    log_info "运行单元/集成测试 (ctest)..."
    local ctest_log="${BUILD_DIR}/ctest.log"

    if ! ctest --test-dir "$BUILD_DIR" --output-on-failure 2>&1 | tee "$ctest_log"; then
        log_fail "单元/集成测试存在失败，详见 $ctest_log"
        return 1
    fi

    # 提取 ctest 统计 (格式: "100% tests passed, 0 tests failed out of N")
    local total failed
    total=$(grep -oP 'out of \K[0-9]+' "$ctest_log" | head -1 || echo "?")
    failed=$(grep -oP 'tests failed, \K[0-9]+' "$ctest_log" | head -1 || echo "0")
    if [ "$total" != "?" ]; then
        log_ok "单元/集成测试通过 ($((total - failed))/${total})"
    else
        log_ok "单元/集成测试通过"
    fi
    return 0
}

# ---------- 6. 运行冒烟测试 ----------
run_smoke_tests() {
    log_info "运行冒烟测试..."
    local smoke_dir="${BUILD_DIR}/tests/smoke"
    local failures=0

    local smoke_tests=("smoke_lifecycle" "smoke_c_api" "smoke_multi_process")

    for test_name in "${smoke_tests[@]}"; do
        local test_bin="${smoke_dir}/${test_name}"
        if [ ! -x "$test_bin" ]; then
            log_fail "冒烟测试可执行文件不存在: $test_bin"
            failures=$((failures + 1))
            continue
        fi

        printf "${CYAN}--- ${test_name} ---${NC}\n"
        if "$test_bin"; then
            log_ok "${test_name} 通过"
        else
            log_fail "${test_name} 失败 (退出码 $?)"
            failures=$((failures + 1))
        fi
        printf "\n"
    done

    if [ "$failures" -ne 0 ]; then
        log_fail "${failures} 个冒烟测试失败"
        return 1
    fi
    log_ok "全部冒烟测试通过"
    return 0
}

# ---------- 7. 汇总 ----------
summary() {
    local result=$1
    printf "\n"
    printf "========================================\n"
    if [ "$result" -eq 0 ]; then
        printf "${GREEN}  InduRTDB 验证结果: 全部通过 (PASS)${NC}\n"
    else
        printf "${RED}  InduRTDB 验证结果: 存在失败 (FAIL)${NC}\n"
    fi
    printf "========================================\n"
}

# =============================================================================
# 主流程
# =============================================================================
main() {
    printf "${CYAN}========================================${NC}\n"
    printf "${CYAN}  InduRTDB 验证流程启动${NC}\n"
    printf "${CYAN}========================================${NC}\n\n"

    if [ "$SMOKE_ONLY" -eq 1 ]; then
        # 仅冒烟模式：要求 build 已存在
        if [ ! -d "$BUILD_DIR" ]; then
            log_fail "build 目录不存在，请先运行 ./verify.sh 编译"
            exit 1
        fi
        run_smoke_tests
        summary $?
        exit $?
    fi

    check_deps
    clean_build
    cmake_configure
    build_all

    local overall_rc=0
    run_unit_tests || overall_rc=1
    run_smoke_tests || overall_rc=1

    summary "$overall_rc"
    exit "$overall_rc"
}

main "$@"
