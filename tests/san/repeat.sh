#!/bin/bash
# repeat.sh — run a sanitizer test binary N times, count clean vs error runs,
# save each run's sanitizer report. Usage:
#   MODE=au BIN=<path> N=<iters> [ARGS...] repeat.sh
# Env:
#   MODE=au|tsan   selects sanitizer env options
#   N              iterations (default 100)
#   TAG            label for log naming (default basename of bin)
set -u
BIN="${BIN:?BIN= binary path required}"
N="${N:-100}"
TAG="${TAG:-$(basename "$BIN")}"
SANDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGD="$SANDIR/logs"
mkdir -p "$LOGD"
case "${MODE:-au}" in
  au)   export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:symbolize=1
        export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0:print_summary=1 ;;
  tsan) export TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:history_size=3 ;;
esac
clean=0; err=0; crash=0
declare -a err_iters=()
for i in $(seq 1 "$N"); do
  out="$("$BIN" "$@" 2>&1)"; rc=$?
  if echo "$out" | grep -qE 'AddressSanitizer|ThreadSanitizer|runtime error:|LeakSanitizer|SUMMARY:.*[Ss]anitizer'; then
    err=$((err+1)); err_iters+=("$i")
    echo "$out" > "$LOGD/repeat_${TAG}_iter${i}.txt"
  elif [ "$rc" -ne 0 ]; then
    crash=$((crash+1)); err_iters+=("$i(crash=$rc)")
    echo "$out" > "$LOGD/repeat_${TAG}_iter${i}.txt"
  else
    clean=$((clean+1))
  fi
done
echo "REPEAT[$TAG] N=$N clean=$clean sanitizer_err=$err crash=$crash err_iters=[${err_iters[*]:-}]"
