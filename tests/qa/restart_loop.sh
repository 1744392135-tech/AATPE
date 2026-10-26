#!/bin/bash
# =============================================================================
# restart_loop.sh — 100x start/stop stability loop.
#
# The full daemon (aatped) needs ARM64/Android. On host we approximate the
# daemon start/stop cycle by repeatedly launching a fresh hermetic subsystem
# test binary (aatpe_subsystem lifecycle + health, the real production TU) and
# checking it comes up clean, does its work, and exits 0. 100 iterations with
# leak detection; any non-zero exit or ASan/Leak report aborts the loop.
#
# Usage: bash tests/qa/restart_loop.sh [iterations]
# =============================================================================
set -u
QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$QA_DIR/common.sh"
qa_set_mode asan    # build with ASan so leaks are caught on every cycle
# v4.4.8: test_v342_round5.cpp calls safemode::setStreakThresholdForTest /
# setRecoverCooldownSecForTest / g_crashLoopObserveSecTestOverride, which are
# guarded by #ifdef AATPE_HOST_TEST in aatpe_subsystem.cpp (RC7 C6). The v94/san
# build passes -DAATPE_HOST_TEST to BOTH prod and test objects (see
# tests/san/build_san.sh PFLAGS/TFLAGS). Mirror that here so the production TU
# exports the seams and the driver can see them.
PROD_FLAGS="$PROD_FLAGS -DAATPE_HOST_TEST"
DRIVER_FLAGS="$DRIVER_FLAGS -DAATPE_HOST_TEST"
# Invalidate any cached aatpe_subsystem.o built without the macro so the seam
# symbols are actually present.
rm -f "$QA_BUILD/aatpe_subsystem.o"
ART="$QA_DIR/artifacts"; mkdir -p "$ART"

ITERS="${1:-100}"

# Build the round5 subsystem test once (real aatpe_subsystem.cpp lifecycle).
BIN="$QA_BUILD/test_restart"
echo "==> building subsystem restart binary (asan+leak)..."
$CXX $DRIVER_FLAGS -I$SRC -I$STUB -c "$SRC/tests/v94/test_v342_round5.cpp" -o "$QA_BUILD/restart_drv.o" 2>"$QA_BUILD/restart_drv.err" || {
  echo "driver build failed"; tail -20 "$QA_BUILD/restart_drv.err"; exit 1; }
qa_prod_obj aatpe_subsystem || exit 1
qa_stub_obj log_stub || exit 1
$CXX $DRIVER_FLAGS "$QA_BUILD/aatpe_subsystem.o" "$QA_BUILD/stub_log_stub.o" "$QA_BUILD/restart_drv.o" -o "$BIN" 2>"$QA_BUILD/restart.link.err" || {
  echo "link failed"; tail -20 "$QA_BUILD/restart.link.err"; exit 1; }

echo "==> restart loop: $ITERS start/stop cycles"
log="$ART/restart_loop.log"
: >"$log"
fail=0
for i in $(seq 1 "$ITERS"); do
  if ! ASAN_OPTIONS=detect_leaks=1 "$BIN" >>"$log" 2>&1; then
    echo "  cycle $i: FAIL (non-zero exit)"; fail=$((fail+1)); break
  fi
  if grep -qE "AddressSanitizer|LeakSanitizer|runtime error" "$log"; then
    echo "  cycle $i: sanitizer report"; fail=$((fail+1)); break
  fi
  if [ $((i % 20)) -eq 0 ]; then echo "  completed $i / $ITERS cycles ok"; fi
done
echo "-------------------------------------------------------------------"
if [ "$fail" -eq 0 ]; then
  echo "RESTART LOOP: PASS ($ITERS clean start/stop cycles, no leak)"
else
  echo "RESTART LOOP: FAIL at cycle $((i)) (see $(basename "$log"))"; tail -20 "$log"
fi
[ "$fail" -eq 0 ]
