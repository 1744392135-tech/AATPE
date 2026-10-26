#!/bin/bash
# =============================================================================
# run_stress.sh — build + run the host stress/load test.
# Stresses the lock-free SPSC ring (throughput, ring-full drop-newest, in-order
# delivery) and an 8-producer/2-consumer thread storm. Runs under plain -O2.
# =============================================================================
set -u
QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$QA_DIR/common.sh"
qa_set_mode stress
ART="$QA_DIR/artifacts"; mkdir -p "$ART"

echo "==> [stress] stress_test.cpp"
$CXX $DRIVER_FLAGS -I$SRC -I$STUB "$QA_DIR/stress_test.cpp" -o "$QA_BUILD/stress_test" \
    2>"$QA_BUILD/stress_test.err" || {
  echo "BUILD FAILED:"; cat "$QA_BUILD/stress_test.err"; exit 1; }

ITERS="${STRESS_ITERS:-1000000}"
log="$ART/stress_test.log"
echo "  running (iterations=$ITERS)..."
"$QA_BUILD/stress_test" "$ITERS" >"$log" 2>&1
rc=$?
cat "$log"
echo "-------------------------------------------------------------------"
if [ "$rc" -eq 0 ]; then
  echo "STRESS: PASS (log=$(basename "$log"))"
else
  echo "STRESS: FAIL (exit=$rc, log=$(basename "$log"))"
fi
exit "$rc"
