#!/usr/bin/env bash
# v3.10 单站规模化负载：构建 Release + 生成 20k 点配置 + 启动 rtdbd + 跑 50×500 订阅
#
# 用法:
#   bash scripts/run_station_load.sh
#   bash scripts/run_station_load.sh --duration 60 --points 2000 --clients 5 --subs 50 --write-rate 2000
#   STATION_LOAD_KEEP=1 bash scripts/run_station_load.sh --no-build   # 保留临时目录/日志
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${STATION_LOAD_BUILD_DIR:-$REPO_ROOT/build}"
POINTS=20000
CLIENTS=50
SUBS=500
DURATION=600
WRITE_RATE=20000
NO_BUILD=0

while [ $# -gt 0 ]; do
    case "$1" in
        --points) POINTS="$2"; shift 2 ;;
        --clients) CLIENTS="$2"; shift 2 ;;
        --subs) SUBS="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --write-rate) WRITE_RATE="$2"; shift 2 ;;
        --no-build) NO_BUILD=1; shift ;;
        -h|--help)
            sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

if ! [[ "$POINTS" =~ ^[0-9]+$ ]] || [ "$POINTS" -lt 2 ]; then
    echo "--points 必须是 >=2 的整数" >&2; exit 2
fi

WORK="$(mktemp -d /tmp/indurtdb-station-load.XXXXXX)"
SOCK="$WORK/rtdbd.sock"
CFG="$WORK/points.yaml"
POL="$WORK/policy.conf"
LOG="$WORK/rtdbd.log"
PID=""

cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" 2>/dev/null || true
        for _ in $(seq 1 50); do
            kill -0 "$PID" 2>/dev/null || break
            sleep 0.1
        done
        kill -KILL "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -f /dev/shm/indurtdb_station_load 2>/dev/null || true
    if [ "${STATION_LOAD_KEEP:-0}" = "1" ]; then
        echo "[keep] $WORK (socket=$SOCK log=$LOG)" >&2
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

if [ "$NO_BUILD" != "1" ]; then
    echo "== build Release rtdbd =="
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
    fi
    cmake --build "$BUILD_DIR" --target rtdbd -j"$(nproc)" >/dev/null
fi
RTDBD="$BUILD_DIR/rtdbd/rtdbd"
[ -x "$RTDBD" ] || { echo "缺少 $RTDBD（先构建或去掉 --no-build）" >&2; exit 2; }

echo "== generate $POINTS-point config =="
python3 - "$CFG" "$POINTS" <<'PY'
import sys
path, n = sys.argv[1], int(sys.argv[2])
with open(path, "w", encoding="utf-8") as f:
    f.write("points:\n")
    for pid in range(n):          # id 0..n-1；max-points=n
        f.write(f"  - id: {pid}\n    name: \"Load_{pid}\"\n"
                f"    type: int32\n    access: 3\n")
PY
printf '%s:*:0:%s\n' "$(id -u)" "$((POINTS - 1))" > "$POL"
rm -f "/dev/shm/indurtdb_station_load"

echo "== start rtdbd =="
"$RTDBD" --instance station_load --socket "$SOCK" \
    --config "$CFG" --policy "$POL" \
    --max-points "$POINTS" --max-subs 1024 --timeout-ms 0 \
    >"$LOG" 2>&1 &
PID=$!

ready=0
for _ in $(seq 1 100); do
    if [ -S "$SOCK" ]; then ready=1; break; fi
    if ! kill -0 "$PID" 2>/dev/null; then break; fi
    sleep 0.1
done
if [ "$ready" != "1" ]; then
    echo "rtdbd 启动失败；日志：" >&2
    sed -n '1,80p' "$LOG" >&2 || true
    exit 1
fi

echo "== run station load =="
python3 "$REPO_ROOT/scripts/run_station_load.py" \
    --socket "$SOCK" --pid "$PID" \
    --points "$POINTS" --clients "$CLIENTS" --subs "$SUBS" \
    --duration "$DURATION" --write-rate "$WRITE_RATE"
