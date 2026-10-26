#!/bin/bash
# AATPE host micro-benchmark runner (deterministic, single-command).
#
# Builds and runs every self-contained bench harness under tests/bench/.
# All harnesses are host x86_64 relative-regression benches (NOT arm64 release):
#   - g++ -O2/-O3, -pthread, -DAATPE_HOST_TEST
#   - single-core pinning via taskset -c 0 (falls back gracefully)
#   - std::chrono::steady_clock timing, P50/P95/P99 reported inside each harness
#   - run each harness >=10 rounds and take the median for a formal gate
#
# Harnesses:
#   1. bench_decision         - C4 decision hot-path (links production c4_freezer.cpp)
#   2. bench_cpu_metrics_lock - g_cpuMetrics 5-site short-lock hold time / 1w+4r
#                               contention / cost-vs-500ms-cycle (round3 perf gate,
#                               self-contained, mirrors aatped_main.cpp lock sites)
#   3. backup_model_cold      - backupModelFile mkstemp vs old getpid fixed-name
#                               cold-path cost (self-contained; args: MODEL_BYTES ROUNDS)
#
# Usage:  bash tests/bench/run_bench.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/build"
mkdir -p "$OUT"
LOGDIR="${AATPE_SOAK_LOGDIR:-$SRC/logs}"
mkdir -p "$LOGDIR"

CXX=g++
# Single-core pinning reduces jitter; fall back gracefully if taskset missing.
PIN=""
if command -v taskset >/dev/null 2>&1; then PIN="taskset -c 0"; fi

# --- 1. bench_decision (links production c4_freezer.cpp) ---
echo "CC bench_decision"
$CXX -std=c++17 -O2 -Wall -Wextra -Werror -pthread -DAATPE_HOST_TEST \
    -I"$SRC" "$SRC/c4_freezer.cpp" "$HERE/bench_decision.cpp" \
    -o "$OUT/bench_decision"
echo "RUN bench_decision (pinned=${PIN:-none})"
$PIN "$OUT/bench_decision" 2>&1 | tee "$LOGDIR/bench_decision.log"

# --- 2. bench_cpu_metrics_lock (self-contained; g_cpuMetrics short-lock gate) ---
echo "CC bench_cpu_metrics_lock"
$CXX -std=c++17 -O3 -Wall -Wextra -Werror -pthread -DAATPE_HOST_TEST \
    "$HERE/bench_cpu_metrics_lock.cpp" -o "$OUT/bench_cpu_metrics_lock"
echo "RUN bench_cpu_metrics_lock (pinned=${PIN:-none})"
$PIN "$OUT/bench_cpu_metrics_lock" 2>&1 | tee "$LOGDIR/bench_cpu_metrics_lock.log"

# --- 3. backup_model_cold (self-contained; args: MODEL_BYTES ROUNDS) ---
echo "CC backup_model_cold"
$CXX -std=c++17 -O2 -Wall -Wextra -Werror -pthread \
    "$HERE/backup_model_cold.cpp" -o "$OUT/backup_model_cold"
echo "RUN backup_model_cold 65536 15 (pinned=${PIN:-none})"
$PIN "$OUT/backup_model_cold" 65536 15 2>&1 | tee "$LOGDIR/backup_model_cold.log"

echo "ALL BENCH DONE -> logs in $LOGDIR"
