#!/bin/bash
# Run integration scenario(s) under ASan while sampling fd count + VmRSS.
# Usage: AATPE_ONLY="S3_MLLearningLoop" TAG=s3 fdrss_it.sh
set -u
SANDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SANDIR/../.." && pwd)"
cd "$SRC"
TS=$(date +%Y%m%d_%H%M%S)
TAG="${TAG:-it}"
LOG="$SANDIR/logs/san_it_${TAG}_${TS}.txt"
export MODE=au
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:symbolize=0
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0:print_summary=1
bash "$SANDIR/run_it_san.sh" > "$LOG" 2>&1 &
WRAP=$!
echo "wrapper pid=$WRAP log=$LOG"
fd0=""; rss0=""; peak_fd=0; peak_rss=0
for i in $(seq 1 200); do
  P=$(pgrep -f aatpe_integration_test_au | head -1)
  if [ -n "$P" ]; then
    FD=$(ls /proc/$P/fd 2>/dev/null | wc -l)
    RSS=$(awk '/VmRSS/{print $2}' /proc/$P/status 2>/dev/null)
    [ -z "$fd0" ] && { fd0=$FD; rss0=$RSS; }
    [ "$FD" -gt "$peak_fd" ] && peak_fd=$FD
    [ "${RSS:-0}" -gt "$peak_rss" ] && peak_rss=$RSS
    echo "sample $i pid=$P fd=$FD (start=$fd0) rss_kb=$RSS (peak_rss=$peak_rss)"
  fi
  if ! kill -0 $WRAP 2>/dev/null; then break; fi
  sleep 0.2
done
wait $WRAP 2>/dev/null
echo "=== FD/RSS SUMMARY start_fd=$fd0 peak_fd=$peak_fd start_rss_kb=$rss0 peak_rss_kb=$peak_rss ==="
echo "=== tail ==="
grep -E 'S[0-9]+:|Result|SUMMARY:|ERROR: AddressSanitizer|leaked|passed,' "$LOG" | head -30
