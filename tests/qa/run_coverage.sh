#!/bin/bash
set -u
QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$QA_DIR/common.sh"
qa_set_mode coverage
ART="$QA_DIR/artifacts"; mkdir -p "$ART"
V94="$SRC/tests/v94"

build_run() {
  local name="$1" prod="$2" stubs="$3" drv="$4"
  echo "==> [cov:$name]"
  $CXX $DRIVER_FLAGS -I$SRC -I$STUB -c "$V94/${drv}.cpp" -o "$QA_BUILD/${drv}.o" 2>"$QA_BUILD/${drv}.err" || {
    echo "  driver compile fail"; tail -10 "$QA_BUILD/${drv}.err"; return 1; }
  local objs="" p s
  for p in $prod; do qa_prod_obj "$p" || return 1; objs="$objs $QA_BUILD/$p.o"; done
  for s in $stubs; do qa_stub_obj "$s" || return 1; objs="$objs $QA_BUILD/stub_$s.o"; done
  $CXX $DRIVER_FLAGS --coverage $objs "$QA_BUILD/${drv}.o" -o "$QA_BUILD/test_${name}" 2>"$QA_BUILD/${drv}.link.err" || {
    echo "  link fail"; tail -15 "$QA_BUILD/${drv}.link.err"; return 1; }
  "$QA_BUILD/test_${name}" >"$ART/cov_${name}.log" 2>&1 && qa_record_pass "cov_$name" ran || qa_record_fail "cov_$name" "see cov_${name}.log"
}

build_run config_schema "aatpe_config_schema" "log_stub" test_config_schema
build_run adrc          "adrc"               "log_stub" test_adrc
build_run abtest        "aatped_abtest"      "log_stub" test_abtest
build_run hmac          "aatped_hmac"        "log_stub" test_hmac
build_run round4        "aatpe_session_token" "log_stub" test_v342_round4
build_run round5        "aatpe_subsystem"    "log_stub" test_v342_round5
build_run integrity     "integrity"          "integrity_deps log_stub" test_integrity
build_run event_monitor "event_monitor uevent_monitor aatpe_ml" "v94_daemon_deps log_stub" test_event_monitor

echo ""
echo "=== gcov coverage (line) per production TU ==="
GCOV_OUT="$ART/gcov"; mkdir -p "$GCOV_OUT"
cd "$QA_BUILD"
: >"$ART/coverage.txt"
printf "%-32s %8s %8s %8s %8s\n" "FILE" "Lines" "Covered" "Miss" "Line%" | tee -a "$ART/coverage.txt"
for gcno in *.gcno; do
  [ -e "$gcno" ] || continue
  base="${gcno%.gcno}"
  case "$base" in stub_*|test_*) continue;; esac
  gcov "$base.gcno" >/dev/null 2>&1 || true
  gcovfile="${base}.cpp.gcov"
  [ -f "$gcovfile" ] || continue
  counts=$(awk -F: '$1 ~ /^[[:space:]]*[0-9]/ {hit++} $1 ~ /^[[:space:]]*#/ {miss++}
                     END{ printf "%d %d", hit+0, miss+0 }' "$gcovfile")
  covered=$(echo "$counts" | cut -d' ' -f1)
  missed=$(echo "$counts" | cut -d' ' -f2)
  exec_total=$((covered + missed))
  pct=0; [ "$exec_total" -gt 0 ] && pct=$(( covered * 100 / exec_total ))
  printf "%-32s %8d %8d %8d %7d%%\n" "$base.cpp" "$exec_total" "$covered" "$missed" "$pct" | tee -a "$ART/coverage.txt"
done

BASELINE="$QA_DIR/coverage_baseline.txt"
echo ""
echo "=== coverage gate (baseline non-regression) ==="
if [ ! -f "$BASELINE" ]; then
  echo "  [WARN] no coverage baseline at $BASELINE; skipping gate"
else
  printf "%-32s %10s %10s %8s\n" "FILE" "Baseline" "Measured" "Status"
  REGRESS=0
  while read -r file lines covered miss pct; do
    [ -z "$file" ] && continue
    measured="${pct%\%}"
    base=$(grep -E "^[[:space:]]*${file}:" "$BASELINE" 2>/dev/null | head -1 | sed 's/.*:[[:space:]]*//; s/%.*//')
    if [ -z "$base" ]; then
      printf "%-32s %10s %10s %8s\n" "$file" "-" "${measured}%" "no-base"
      continue
    fi
    if [ "$measured" -lt "$base" ]; then
      printf "%-32s %9d%% %9d%% %8s\n" "$file" "$base" "$measured" "DROP"
      REGRESS=1
    else
      printf "%-32s %9d%% %9d%% %8s\n" "$file" "$base" "$measured" "ok"
    fi
  done < <(tail -n +2 "$ART/coverage.txt")
  [ "$REGRESS" -ne 0 ] && { echo "  [GATE FAIL] coverage dropped below baseline"; QA_FAIL=1; }

  echo ""
  echo "=== gap report (aspirational; non-blocking) ==="
  while read -r file lines covered miss pct; do
    [ -z "$file" ] && continue
    measured="${pct%\%}"
    case "$file" in
      aatped_hmac.cpp|integrity.cpp) target=90;;
      *) target=70;;
    esac
    if [ "$measured" -lt "$target" ]; then
      echo "  gap: $file ${measured}% < target ${target}%"
    fi
  done < <(tail -n +2 "$ART/coverage.txt")
fi
qa_summary
[ "$QA_FAIL" -eq 0 ]
