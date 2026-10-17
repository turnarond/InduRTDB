#!/usr/bin/env bash
# =============================================================================
# InduRTDB 部署验证脚本 (scripts/verify_deploy.sh)
#
# 回答一个问题：**装完到底对不对？** 与既有脚本的区别：
#   scripts/run_e2e.sh / run_soak.sh / run_monitor_smoke.sh / verify_consume.sh
#   irt_diag {inspect,smoke,leak}
#   —— 上述全部针对「构建树」，验证的是「代码没问题」。
#   本脚本针对「已安装的部署」，验证的是「这台机器上的安装没问题」。
#
# 用法:
#   bash scripts/verify_deploy.sh                        # 默认 prefix=/usr/local
#   bash scripts/verify_deploy.sh --prefix /opt/irtdb
#   bash scripts/verify_deploy.sh --keep                 # 验证后保留 rtdbd 与段
#
# 退出码: 0 = 全部通过, 非 0 = 存在失败项
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ---------- 参数 ----------
PREFIX="/usr/local"
KEEP=0
INSTANCE="verify_deploy"
SOCKET="/tmp/indurtdb_verify.sock"
HARNESS="${REPO_ROOT}/build/tools/irt_diag/irt_diag_harness"

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --keep)   KEEP=1; shift ;;
        --instance) INSTANCE="$2"; shift 2 ;;
        --socket) SOCKET="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) printf '未知参数: %s\n' "$1"; exit 2 ;;
    esac
done

# ---------- 输出 ----------
PASS=0; FAIL=0; SKIP=0
FAILED_ITEMS=()
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

ok()   { printf "${GREEN}[ PASS ]${NC} %s\n" "$*"; PASS=$((PASS+1)); }
bad()  { printf "${RED}[ FAIL ]${NC} %s\n" "$*"; FAIL=$((FAIL+1)); FAILED_ITEMS+=("$*"); }
warn() { printf "${YELLOW}[ SKIP ]${NC} %s\n" "$*"; SKIP=$((SKIP+1)); }
hint() { printf "${CYAN}         → %s${NC}\n" "$*"; }
step() { printf "\n${CYAN}── %s ──${NC}\n" "$*"; }

cleanup() {
    if [ -n "${RTDBD_PID:-}" ] && kill -0 "$RTDBD_PID" 2>/dev/null; then
        if [ "$KEEP" -eq 1 ]; then
            printf "\n${YELLOW}[KEEP]${NC} rtdbd pid=${RTDBD_PID} 未停止（--keep）\n"
        else
            kill -TERM "$RTDBD_PID" 2>/dev/null
            wait "$RTDBD_PID" 2>/dev/null
            printf "${CYAN}[CLEAN]${NC} rtdbd 已停止\n"
        fi
    fi
    if [ "$KEEP" -eq 0 ]; then
        rm -f "/dev/shm/indurtdb_${INSTANCE}" "$SOCKET"
        printf "${CYAN}[CLEAN]${NC} 段与 socket 已清理\n"
    fi
}
trap cleanup EXIT

# 探测 rtdbd 可执行文件位置
find_rtdbd() {
    for c in "${PREFIX}/bin/rtdbd" "${REPO_ROOT}/build/rtdbd/rtdbd" "$(command -v rtdbd 2>/dev/null)"; do
        [ -n "$c" ] && [ -x "$c" ] && { printf '%s' "$c"; return 0; }
    done
    return 1
}

printf "${CYAN}========================================${NC}\n"
printf "${CYAN}  InduRTDB 部署验证 (prefix=%s)${NC}\n" "$PREFIX"
printf "${CYAN}========================================${NC}\n"

# ========== 1. 运行环境 ==========
step "1. 运行环境"

if [ "$(uname -s)" = "Linux" ]; then
    ok "平台 Linux"
else
    bad "平台 $(uname -s) 非 Linux"
    hint "InduRTDB 当前仅在 Linux 上验证过（见 CODEBUDDY.md 平台支持）"
fi

if [ -d /dev/shm ] && [ -w /dev/shm ]; then
    ok "/dev/shm 可写（共享内存段所在）"
else
    bad "/dev/shm 不可写"
    hint "共享内存依赖 /dev/shm；容器中需挂载 --shm-size 或 -v /dev/shm:/dev/shm"
fi

# ========== 2. 库安装 ==========
step "2. 库安装产物"

LIB="${PREFIX}/lib/libindurtdb.a"
HDR="${PREFIX}/include/indurtdb/indurtdb.h"

if [ -f "$LIB" ]; then
    ok "静态库存在: $LIB ($(du -h "$LIB" | cut -f1))"
else
    bad "静态库缺失: $LIB"
    hint "cmake --install build --prefix ${PREFIX}"
fi

if [ -f "$HDR" ]; then
    ok "公共头文件存在: $HDR"
else
    bad "公共头文件缺失: $HDR"
    hint "cmake --install build --prefix ${PREFIX}"
fi

INSTALLED_VER="$(sed -n 's/^#define INDURTDB_VERSION_STRING "\(.*\)"/\1/p' "$HDR" 2>/dev/null)"
SRC_VER="$(cat "${REPO_ROOT}/VERSION" 2>/dev/null)"
if [ -n "$INSTALLED_VER" ] && [ -n "$SRC_VER" ]; then
    if [ "$INSTALLED_VER" = "$SRC_VER" ]; then
        ok "版本一致: $INSTALLED_VER"
    else
        bad "版本不一致: 已安装=$INSTALLED_VER 仓库=$SRC_VER"
        hint "装错版本了 —— 重新 cmake --install build --prefix ${PREFIX}"
    fi
fi

# ========== 3. 消费路径 ==========
step "3. 消费路径"

if command -v pkg-config >/dev/null 2>&1; then
    if PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig" pkg-config --exists indurtdb 2>/dev/null; then
        FLAGS="$(PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig" pkg-config --cflags --libs indurtdb 2>/dev/null)"
        if [ -n "$FLAGS" ]; then
            # 关键：indurtdb.pc 的 prefix 在 **configure 阶段** 写入，
            # `cmake --install --prefix` 不会改写它。若两者不一致，
            # pkg-config 会指向错误的路径 —— 编译能过，运行才炸。
            PC_PREFIX="$(sed -n 's/^prefix=//p' "${PREFIX}/lib/pkgconfig/indurtdb.pc" 2>/dev/null)"
            case "$PC_PREFIX" in
                "$PREFIX"|"$PREFIX/"*)
                    ok "pkg-config 可用且 prefix 一致: $FLAGS"
                    ;;
                *)
                    bad "pkg-config 的 prefix 与安装路径不一致"
                    printf "         → .pc 里写的是 prefix=%s\n" "$PC_PREFIX"
                    printf "         → 实际安装在 %s\n" "$PREFIX"
                    printf "         → 消费方会拿到错误的 -I/-L，编译能过、运行才炸\n"
                    hint "必须在 configure 阶段指定：cmake -S . -B build -DCMAKE_INSTALL_PREFIX=${PREFIX}"
                    ;;
            esac
        else
            bad "pkg-config 找到 indurtdb 但未返回 flags"
        fi
    else
        bad "pkg-config 找不到 indurtdb"
        hint "确认 ${PREFIX}/lib/pkgconfig/indurtdb.pc 存在"
    fi
else
    warn "pkg-config 未安装，跳过"
fi

for f in InduRTDBConfig.cmake InduRTDBTargets.cmake InduRTDBConfigVersion.cmake; do
    if [ -f "${PREFIX}/lib/cmake/InduRTDB/${f}" ]; then
        ok "CMake 包文件存在: $f"
    else
        bad "CMake 包文件缺失: $f"
        hint "cmake --install build --prefix ${PREFIX}"
    fi
done

# ========== 4. 守护进程与工具 ==========
step "4. 守护进程与诊断工具"

if RTDBD_BIN="$(find_rtdbd)"; then
    ok "rtdbd 可执行: $RTDBD_BIN"
else
    bad "找不到 rtdbd"
    hint "仓库：cmake --build build --target rtdbd"
    hint "已安装：install -m0755 <rtdbd> ${PREFIX}/bin/   （当前无 install 规则，见产品化计划 D4'）"
fi

if [ -f "${REPO_ROOT}/tools/irt_diag/irt_diag/cli.py" ]; then
    ok "irt_diag 工具在位"
else
    bad "irt_diag 工具缺失"
    hint "应随仓库存在；用法见 tools/irt_diag/README.md"
fi

# ========== 5. 配置样例 ==========
step "5. 部署配置"

POLICY_SRC="${REPO_ROOT}/config/rtdbd/policy.conf"
POINTS_SRC="${REPO_ROOT}/config/rtdbd/points.yaml"
POLICY="/tmp/verify_deploy_policy.conf"
POINTS="/tmp/verify_deploy_points.yaml"

if [ -f "$POLICY_SRC" ]; then
    cp "$POLICY_SRC" "$POLICY"
    # 规则数必须 > 0：格式错误的行被静默跳过，全部写错等同「拒绝所有写」
    RULES="$(grep -vE '^\s*(#|$)' "$POLICY" | wc -l)"
    if [ "$RULES" -gt 0 ]; then
        ok "鉴权策略就绪: $RULES 条规则"
    else
        bad "鉴权策略有效规则数为 0 —— 等同拒绝所有写"
        hint "grep -vE '^\\s*(#|\$)' policy.conf | wc -l   # 必须 > 0"
    fi
else
    bad "鉴权策略样例缺失: config/rtdbd/policy.conf"
    POLICY=""
fi

if [ -f "$POINTS_SRC" ]; then
    cp "$POINTS_SRC" "$POINTS"
    ENTRIES="$(grep -cE '^\s*-\s*id:' "$POINTS")"
    ok "点位配置就绪: $ENTRIES 个点位"
else
    bad "点位配置样例缺失: config/rtdbd/points.yaml"
    POINTS=""
fi

# ========== 6. 段与守护进程实跑 ==========
step "6. 段与守护进程实跑"

if [ -z "${RTDBD_BIN:-}" ] || [ -z "$POLICY" ] || [ -z "$POINTS" ]; then
    bad "前置条件缺失，跳过实跑"
    hint "需要 rtdbd + policy + points 三者齐备"
else
    rm -f "/dev/shm/indurtdb_${INSTANCE}" "$SOCKET"

    "$RTDBD_BIN" --instance "$INSTANCE" --socket "$SOCKET" \
                 --policy "$POLICY" --config "$POINTS" \
                 --max-points 5000 --max-subs 16 >/tmp/verify_deploy_rtdbd.log 2>&1 &
    RTDBD_PID=$!
    sleep 1.5

    if ! kill -0 "$RTDBD_PID" 2>/dev/null; then
        bad "rtdbd 启动失败（详见 /tmp/verify_deploy_rtdbd.log）"
        hint "退出码 1=参数/policy 错误  2=配置校验失败  3=段损坏  4=自检失败"
    elif [ ! -S "$SOCKET" ]; then
        bad "rtdbd 存活但未创建 socket: $SOCKET"
    else
        ok "rtdbd 启动并监听: $SOCKET"
    fi

    # 段巡检
    if [ -f "/dev/shm/indurtdb_${INSTANCE}" ]; then
        ok "共享内存段已创建: /dev/shm/indurtdb_${INSTANCE}"
        if (cd "${REPO_ROOT}/tools/irt_diag" && python3 -m irt_diag.cli inspect --id "$INSTANCE" >/tmp/verify_deploy_inspect.json 2>&1); then
            ok "段巡检通过（magic/version/CRC/owner pid）"
            IDX="$(sed -n 's/.*"index_count": *\([0-9]*\).*/\1/p' /tmp/verify_deploy_inspect.json)"
            if [ -n "$IDX" ] && [ "$IDX" -gt 0 ]; then
                ok "配置点位已生效: index_count=$IDX"
                if [ -n "$ENTRIES" ] && [ "$IDX" -ne "$ENTRIES" ]; then
                    warn "index_count($IDX) 与配置条目($ENTRIES) 不符 —— 有条目被静默丢弃"
                    hint "最常见原因：type 写法不被 irt_config_parse_type 识别"
                fi
            else
                bad "index_count=$IDX —— 配置点位一条都没生效"
                hint "检查 points 配置格式；type 仅识别 bool/int32/int64/uint32/double/string"
            fi
        else
            bad "段巡检未通过"
            hint "查看 /tmp/verify_deploy_inspect.json"
        fi
    else
        bad "共享内存段未创建"
    fi

    # 读路径（直读共享内存，不经 rtdbd）
    if [ -f "${REPO_ROOT}/build/examples/basic_example" ]; then
        ok "示例可执行在位（可作读路径冒烟）"
    else
        warn "示例未构建，跳过读路径冒烟"
        hint "cmake --build build --target basic_example"
    fi
fi

# ========== 汇总 ==========
printf "\n${CYAN}========================================${NC}\n"
printf "  通过 ${GREEN}%d${NC}   失败 ${RED}%d${NC}   跳过 ${YELLOW}%d${NC}\n" "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -eq 0 ]; then
    printf "${GREEN}  部署验证通过 (PASS)${NC}\n"
else
    printf "${RED}  部署验证失败 (FAIL)${NC}\n"
    printf "  失败项：\n"
    for it in "${FAILED_ITEMS[@]}"; do printf "    - %s\n" "$it"; done
fi
printf "${CYAN}========================================${NC}\n"
[ "$FAIL" -eq 0 ]