#!/usr/bin/env bash
# InduRTDB 本地门禁入口（Stage 链，工程规范 Playbook v1.3.0 §4）。
#
# 纪律：
#   - 每个 Stage 必须可证伪：有执行证据、能抓坏；恒绿等于没门禁。
#   - --quick 只允许跳过「构建 + 金丝雀」，ctest 结果仅代表既有产物；
#     版本分支合入 / 打 tag / 发布前**禁止** --quick。
#   - 跳过任一 Stage 必须显式 --skip-stage <名> --reason <issue/理由>，并进账本。
#
# 用法：
#   bash scripts/check_all.sh                 # 全量门禁
#   bash scripts/check_all.sh --quick         # 赶时间：复用既有构建产物
#   bash scripts/check_all.sh --skip-stage canary --reason "#123 临时"
#   BUILD_DIR=build-debug bash scripts/check_all.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

BUILD_DIR="${BUILD_DIR:-build}"
JOBS="$(nproc 2>/dev/null || echo 4)"
QUICK=0
SKIP_NAMES=()
SKIP_REASONS=()
EVIDENCE_DIR="$BUILD_DIR"
mkdir -p "$EVIDENCE_DIR" 2>/dev/null || EVIDENCE_DIR="."
EVIDENCE="$EVIDENCE_DIR/check-all-$(date +%Y%m%d-%H%M%S).log"
: > "$EVIDENCE" || { echo "FAIL: 无法写证据文件 $EVIDENCE"; exit 2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) QUICK=1; shift ;;
    --skip-stage) SKIP_NAMES+=("${2:-}"); shift 2 ;;
    --reason) SKIP_REASONS+=("${2:-}"); shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "未知参数：$1"; exit 2 ;;
  esac
done

is_skipped() {
  local n="$1" i
  for i in "${!SKIP_NAMES[@]}"; do
    [[ "${SKIP_NAMES[$i]}" == "$n" ]] && return 0
  done
  return 1
}

reason_of() {
  local n="$1" i
  for i in "${!SKIP_NAMES[@]}"; do
    [[ "${SKIP_NAMES[$i]}" == "$n" ]] && { echo "${SKIP_REASONS[$i]:-未填理由}"; return; }
  done
  echo ""
}

run_stage() {  # $1=Stage 名，其余=命令
  local name="$1"; shift
  if is_skipped "$name"; then
    local why; why="$(reason_of "$name")"
    echo "[SKIP] $name —— 理由：$why"
    echo "[SKIP] $name —— 理由：$why" >> "$EVIDENCE"
    if [[ -z "$why" || "$why" == "未填理由" ]]; then
      echo "[FAIL] $name 跳过但未给 --reason；无声明 skip 视为违规" | tee -a "$EVIDENCE"
      return 1
    fi
    return 0
  fi
  echo "=== Stage: $name ==="
  local start rc
  start=$(date +%s)
  "$@" 2>&1 | tee -a "$EVIDENCE"
  rc=${PIPESTATUS[0]}
  local cost=$(( $(date +%s) - start ))
  if [[ $rc -ne 0 ]]; then
    echo "[FAIL] $name（${cost}s）" | tee -a "$EVIDENCE"
    return 1
  fi
  echo "[OK] $name（${cost}s）" | tee -a "$EVIDENCE"
  return 0
}

echo "门禁证据：$EVIDENCE" | tee -a "$EVIDENCE"
echo "模式：$([[ $QUICK -eq 1 ]] && echo 'quick（禁用于合入/发布）' || echo 'full')" | tee -a "$EVIDENCE"

# 0.1 仓库洁净度（已跟踪的 AI/构建中间目录 + ignore 失效）
run_stage hygiene bash scripts/check_repo_hygiene.sh || exit 1

# 0.2 版本号四处一致
run_stage version python3 scripts/check_version_sync.py || exit 1

# 0.3 构建（-Wall -Wextra -Werror：任何警告即失败）
if [[ $QUICK -eq 1 ]]; then
  echo "[SKIP] build —— --quick 复用既有产物（合入/发布前必须全量）" | tee -a "$EVIDENCE"
else
  run_stage build bash -c "cmake -S . -B '$BUILD_DIR' -DCMAKE_BUILD_TYPE=Release && cmake --build '$BUILD_DIR' -j$JOBS" || exit 1
fi

# 0.4 单元 + 集成测试（37 个 ctest 用例）
run_stage ctest bash -c "cd '$BUILD_DIR' && ctest --output-on-failure -j$JOBS" || exit 1

# 0.5 API 计数（头文件与 SDK 手册声明一致）
run_stage api_count python3 scripts/check_api_count.py || exit 1

# 0.6 探针自证：检查器用例 + 守卫判据自证（防「判据被改坏仍恒绿」）
run_stage self_test bash -c "python3 scripts/test_checkers.py \
  && python3 scripts/check_version_sync.py --self-test \
  && python3 scripts/check_arch_layers.py --self-test" || exit 1

# 0.7 架构分层方向（机制 C）
run_stage arch_layers python3 scripts/check_arch_layers.py || exit 1

# 0.8 门禁金丝雀：坏样本必须被抓出（机制 B），全量模式必跑
if [[ $QUICK -eq 1 ]]; then
  echo "[SKIP] canary —— --quick 不做金丝雀（合入/发布前必须全量）" | tee -a "$EVIDENCE"
else
  run_stage canary python3 scripts/gate_canary.py || exit 1
fi

echo "ALL GREEN（证据：$EVIDENCE）" | tee -a "$EVIDENCE"
