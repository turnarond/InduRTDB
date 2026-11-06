#!/usr/bin/env bash
# 仓库洁净度守卫（工程规则 1：工程结构洁净度）。
#
# 关键教训：`.gitignore` 对**已跟踪**文件无效，所以“加了 ignore”不等于“没入库”。
# 本脚本检查的是「实际被跟踪的路径」，不是工作区里存在什么。
#
# 判定：
#   FAIL  被跟踪的 AI/插件/构建中间目录（.claude/ .omc/ .omo/ .superpowers/ .codegraph/ 等）
#   FAIL  被跟踪的构建产物目录（build/ build-*/ node_modules/ __pycache__/）
#   WARN  被 .gitignore 命中却仍被跟踪的文件（忽略规则对它们失效，要么移出跟踪要么放行）
#   WARN  docs/ 下的英文目录或 AI 工具目录（违反文档目录规范）
#
# 用法：bash scripts/check_repo_hygiene.sh [--root <dir>] [--json]
#   --root 供门禁金丝雀对临时坏样本仓库取证；默认取脚本所在仓库根。
set -uo pipefail

ROOT=""
JSON=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --root) ROOT="${2:-}"; shift 2 ;;
    --json) JSON=1; shift ;;
    *) shift ;;
  esac
done
ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT" || exit 2

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "FAIL: 不在 git 工作区内，无法判定跟踪状态"
  exit 2
fi

FAILS=()
WARNS=()

# 1) 被跟踪的 AI / 插件 / 构建中间目录
BAD_DIR_RE='^(\.claude|\.codebuddy|\.cursor|\.codex|\.aider|\.continue|\.windsurf|\.qoder|\.omc|\.omo|\.superpowers|\.codegraph|\.cache|\.ccls-cache|node_modules|__pycache__|\.pytest_cache|build|build-[^/]*)/'
while IFS= read -r p; do
  [[ -n "$p" ]] && FAILS+=("中间目录入库：$p")
done < <(git ls-files | grep -E "$BAD_DIR_RE" || true)

# 2) 被忽略却仍被跟踪（ignore 失效）
IGNORED_TRACKED="$(git ls-files -ci --exclude-standard || true)"
if [[ -n "$IGNORED_TRACKED" ]]; then
  while IFS= read -r p; do
    [[ -z "$p" ]] && continue
    WARNS+=("忽略规则失效（已跟踪）：$p")
  done <<< "$IGNORED_TRACKED"
fi

# 3) docs/ 目录规范：一级目录须「两位数字+中文」，且不得是 AI 工具目录
while IFS= read -r d; do
  [[ -z "$d" ]] && continue
  name="${d#docs/}"
  name="${name%/}"
  [[ "$name" == */* ]] && continue
  if [[ ! "$name" =~ ^[0-9]{2}- ]]; then
    WARNS+=("docs 一级目录不满足「两位序号+中文」：docs/$name")
  fi
done < <(git ls-files docs | awk -F/ 'NF > 2 {print $1"/"$2}' | sort -u || true)

emit() {
  local level="$1"; shift
  printf '  [%s] %s\n' "$level" "$*"
}

if [[ $JSON -eq 1 ]]; then
  python3 - "$ROOT" "${#FAILS[@]}" "${#WARNS[@]}" <<'PY'
import json, sys
root, nf, nw = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
print(json.dumps({"ok": nf == 0, "root": root, "fails": nf, "warns": nw}, ensure_ascii=False))
PY
else
  for w in "${WARNS[@]:-}"; do [[ -n "$w" ]] && emit WARN "$w"; done
  for f in "${FAILS[@]:-}"; do [[ -n "$f" ]] && emit FAIL "$f"; done
  if [[ ${#FAILS[@]} -eq 0 ]]; then
    echo "OK: 仓库洁净（${#WARNS[@]} 条告警）"
  else
    echo "FAIL: ${#FAILS[@]} 项中间产物被跟踪（${#WARNS[@]} 条告警）"
  fi
fi

[[ ${#FAILS[@]} -eq 0 ]] || exit 1
exit 0
