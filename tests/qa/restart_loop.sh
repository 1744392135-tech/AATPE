#!/bin/bash
set -u
QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$QA_DIR/common.sh"
qa_set_mode asan
PROD_FLAGS="$PROD_FLAGS -DAATPE_HOST_TEST"
DRIVER_FLAGS="$DRIVER_FLAGS -DAATPE_HOST_TEST"
rm -f "$QA_BUILD/aatpe_subsystem.o"
ART="$QA_DIR/artifacts"; mkdir -p "$ART"

ITERS="${1:-100}"

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
