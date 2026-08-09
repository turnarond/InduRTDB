#!/usr/bin/env bash
# 外部消费回归: 在干净环境模拟外部用户两条路径消费 InduRTDB。
# 任一失败即退出非 0。可本地运行, 也可挂入 CI。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PREFIX="$WORK/install"
JOBS="$(nproc)"

echo "== [1/4] 构建并安装 InduRTDB -> $PREFIX =="
cmake -S "$REPO_ROOT" -B "$WORK/indurtdb-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" > /dev/null
cmake --build "$WORK/indurtdb-build" -j"$JOBS" > /dev/null
cmake --install "$WORK/indurtdb-build" --prefix "$PREFIX" > /dev/null

echo "== [2/4] find_package 消费 starter =="
cmake -S "$REPO_ROOT/templates/indurtdb-starter" -B "$WORK/starter-fp" \
    -DCMAKE_PREFIX_PATH="$PREFIX" > /dev/null
cmake --build "$WORK/starter-fp" -j"$JOBS" > /dev/null
"$WORK/starter-fp/starter"

echo "== [3/4] pkg-config 解析 =="
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
pkg-config --exists --print-errors indurtdb
echo "pkg-config modversion: $(pkg-config --modversion indurtdb)"
PC_PREFIX="$(pkg-config --variable=prefix indurtdb)"
echo "pkg-config prefix: $PC_PREFIX"
if [ "$PC_PREFIX" != "$PREFIX" ]; then
    echo "ERROR: .pc prefix ($PC_PREFIX) != install prefix ($PREFIX)" >&2
    exit 1
fi
echo "prefix consistency OK"

echo "== [4/4] FetchContent 消费 starter (离线 SOURCE_DIR) =="
cmake -S "$REPO_ROOT/templates/indurtdb-starter" -B "$WORK/starter-fc" \
    -DSTARTER_USE_FETCHCONTENT=ON -DINDURTDB_SOURCE_DIR="$REPO_ROOT" > /dev/null
cmake --build "$WORK/starter-fc" -j"$JOBS" > /dev/null
"$WORK/starter-fc/starter"

echo "ALL CONSUMPTION PATHS OK"
