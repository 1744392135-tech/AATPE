#!/bin/bash
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/build"
mkdir -p "$OUT"
LOGDIR="${AATPE_SOAK_LOGDIR:-$SRC/logs}"
mkdir -p "$LOGDIR"

CXX=g++
PIN=""
if command -v taskset >/dev/null 2>&1; then PIN="taskset -c 0"; fi

echo "CC bench_decision"
$CXX -std=c++17 -O2 -Wall -Wextra -Werror -pthread -DAATPE_HOST_TEST \
    -I"$SRC" "$SRC/c4_freezer.cpp" "$HERE/bench_decision.cpp" \
    -o "$OUT/bench_decision"
echo "RUN bench_decision (pinned=${PIN:-none})"
$PIN "$OUT/bench_decision" 2>&1 | tee "$LOGDIR/bench_decision.log"

echo "CC bench_cpu_metrics_lock"
$CXX -std=c++17 -O3 -Wall -Wextra -Werror -pthread -DAATPE_HOST_TEST \
    "$HERE/bench_cpu_metrics_lock.cpp" -o "$OUT/bench_cpu_metrics_lock"
echo "RUN bench_cpu_metrics_lock (pinned=${PIN:-none})"
$PIN "$OUT/bench_cpu_metrics_lock" 2>&1 | tee "$LOGDIR/bench_cpu_metrics_lock.log"

echo "CC backup_model_cold"
$CXX -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
    "$HERE/backup_model_cold.cpp" -o "$OUT/backup_model_cold"
echo "RUN backup_model_cold 65536 15 (pinned=${PIN:-none})"
$PIN "$OUT/backup_model_cold" 65536 15 2>&1 | tee "$LOGDIR/backup_model_cold.log"

echo "ALL BENCH DONE -> logs in $LOGDIR"
