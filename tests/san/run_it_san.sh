#!/bin/bash
set -u
MODE="${MODE:?au|tsan}"
SANDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SANDIR/../.." && pwd)"
BIN="$SANDIR/aatpe_integration_test_$MODE"
[ -x "$BIN" ] || { echo "missing $BIN"; exit 1; }
case "$MODE" in
  au)   export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:symbolize=1
        export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0:print_summary=1 ;;
  tsan) export TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:history_size=3 ;;
esac
exec bash "$SRC/tests/integration/run_integration.sh" "$SRC" "$BIN"
