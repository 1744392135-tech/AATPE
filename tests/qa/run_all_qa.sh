#!/bin/bash
set -u
QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ART="$QA_DIR/artifacts"; mkdir -p "$ART"
MASTER="$ART/master_table.txt"; : >"$MASTER"

declare -a ROWS=()
step() {
  local name="$1" label="$2"; shift 2
  echo ""
  echo "############################################################"
  echo "# QA STAGE: $label"
  echo "############################################################"
  local log="$ART/stage_${name}.log"
  if "$@" >"$log" 2>&1; then
    ROWS+=("$name|PASS|see stage_${name}.log")
    echo ">> $label: PASS"
  else
    local rc=$?
    ROWS+=("$name|FAIL(rc=$rc)|see stage_${name}.log")
    echo ">> $label: FAIL (rc=$rc) -- tail:"; tail -15 "$log"
  fi
}

step fault     "Fault-injection harness (D4 recovery/exec)" bash "$QA_DIR/run_fault_inject.sh"
step fuzz       "Fuzz sweep (json/config/event/command)" bash "$QA_DIR/run_fuzz.sh"
step sanitizer  "ASan+UBSan+LSan"                          bash "$QA_DIR/run_sanitizers.sh" asan
step tsan       "TSan (data race)"                         bash "$QA_DIR/run_sanitizers.sh" tsan
step coverage   "Coverage (gcov)"                          bash "$QA_DIR/run_coverage.sh"
step stress     "Stress / load"                            bash "$QA_DIR/run_stress.sh"
step restart    "Restart loop (100x)"                      bash "$QA_DIR/restart_loop.sh" 100

echo ""
echo "=============================================================="
echo "                  AATPE QA MASTER RESULTS"
echo "=============================================================="
printf "%-14s %-12s %s\n" "STAGE" "RESULT" "EVIDENCE" | tee -a "$MASTER"
n_fail=0
for r in "${ROWS[@]}"; do
  name="${r%%|*}"; rest="${r#*|}"; res="${rest%%|*}"; ev="${rest#*|}"
  printf "%-14s %-12s %s\n" "$name" "$res" "$ev" | tee -a "$MASTER"
  case "$res" in FAIL*) n_fail=$((n_fail+1));; esac
done
echo "--------------------------------------------------------------"
if [ -f "$ART/coverage.txt" ]; then
  echo "Per-module line coverage:" | tee -a "$MASTER"
  cat "$ART/coverage.txt" | tee -a "$MASTER"
fi
echo "=============================================================="
if [ "$n_fail" -eq 0 ]; then echo "OVERALL: PASS"; else echo "OVERALL: $n_fail stage(s) FAILED"; fi
echo "Artifacts: $ART"
[ "$n_fail" -eq 0 ]
