#!/bin/bash
# =============================================================================
# run_sanitizers.sh — build the hermetic unit tests under sanitizers and run.
#
# Reuses the v94 approach (real production TUs + host stubs) but compiles and
# links with sanitizer instrumentation. Targets are independent: one failure
# does not abort the rest.
#
# Modes exercised:
#   asan : -fsanitize=address,undefined  (memory + UB)
#   tsan : -fsanitize=thread              (data races; thread-heavy targets)
#   lsan : leak check via ASan's LeakSanitizer on the same asan build
#
# Usage:
#   bash tests/qa/run_sanitizers.sh           # asan over all targets
#   bash tests/qa/run_sanitizers.sh asan
#   bash tests/qa/run_sanitizers.sh tsan
#   bash tests/qa/run_sanitizers.sh all
# =============================================================================
set -u
QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$QA_DIR/common.sh"
MODE="${1:-asan}"

# recipe table:  name | prod basenames | stub basenames | driver cpp (in v94)
# Curated to the lightest, self-contained v94 targets that link cleanly on host.
# Prod/stub sets are aligned to the canonical tests/v94/build.sh recipes (which
# all define AATPE_HOST_TEST); stubs replace the production glue that compiles
# out under AATPE_HOST_TEST (e.g. scenewire for c4FreezerStateJson).
V94="$SRC/tests/v94"
recipes() {
  # name          prod                                                                   stubs                              driver
  cat <<EOF
config_schema|aatpe_config_schema|log_stub|test_config_schema
adrc|adrc|log_stub|test_adrc
abtest|aatped_abtest|log_stub|test_abtest
hmac|aatped_hmac|log_stub|test_hmac
round4|aatpe_session_token|log_stub|test_v342_round4
round5|aatpe_subsystem|log_stub|test_v342_round5
integrity|integrity|integrity_deps log_stub|test_integrity
webui_json|webui_json aatpe_ml|webui_clusters_deps v94_daemon_deps log_stub scenewire|test_webui_json
event_monitor|event_monitor uevent_monitor aatpe_ml|v94_daemon_deps log_stub|test_event_monitor
utils|aatped_utils aatpe_helper_audit|utils_deps log_stub sprop_stub|test_utils
caps|aatpe_caps|log_stub|test_caps
recovery|aatpe_recovery||test_recovery
placement|placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil|placement_deps ml_deps gamelist_stub|test_placement
thermal|thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff|thermal_deps log_stub|test_thermal
EOF
}

# TSan targets (ones that actually spawn threads / share state).
tsan_targets="event_monitor round5 caps recovery thermal"

build_and_run() {  # $1=name $2=prod $3=stubs $4=driver
  local name="$1" prod="$2" stubs="$3" drv="$4"
  local bin="$QA_BUILD/test_${name}"
  echo "==> [$QA_MODE:$name]"
  # compile the existing v94 test driver (strict)
  echo "  [cc:drv] $drv.cpp" >&2
  if ! $CXX $DRIVER_FLAGS -I$SRC -I$STUB -c "$V94/${drv}.cpp" -o "$QA_BUILD/${drv}.o" 2>"$QA_BUILD/${drv}.err"; then
    echo "  DRIVER COMPILE FAILED:"; head -20 "$QA_BUILD/${drv}.err"
    qa_record_fail "$name" "driver compile failed see $(basename "${drv}.err")"; return 0; fi
  # link prod + stubs
  local objs="" p s
  for p in $prod; do
    qa_prod_obj "$p" || { qa_record_fail "$name" "prod compile failed: $p"; return 0; }
    objs="$objs $QA_BUILD/$p.o"
  done
  for s in $stubs; do
    qa_stub_obj "$s" || { qa_record_fail "$name" "stub compile failed: $s"; return 0; }
    objs="$objs $QA_BUILD/stub_$s.o"
  done
  echo "  [link] $name" >&2
  # -lz: host always provides it (aatped_apklabel / webui_http decompress), mirroring v94/build.sh.
  if ! $CXX $DRIVER_FLAGS $objs "$QA_BUILD/${drv}.o" -o "$bin" -lz 2>"$QA_BUILD/${drv}.link.err"; then
    echo "  LINK FAILED:"; head -25 "$QA_BUILD/${drv}.link.err"
    qa_record_fail "$name" "link failed see $(basename "${drv}.link.err")"; return 0; fi
  local log="$QA_DIR/artifacts/san_${QA_MODE}_${name}.log"
  mkdir -p "$QA_DIR/artifacts"

  # Special: thermal requires namespaced runner (bind-mock /sys/class/thermal)
  if [ "$name" = "thermal" ]; then
    run_thermal_namespaced_san "$bin" "$log" || {
      echo "  ---- thermal ns run log tail ----"; tail -20 "$log"
      qa_record_fail "$name" "thermal ns run failed see $(basename "$log")"
      return 1; }
  else
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 \
      "$bin" >"$log" 2>&1
  fi

  local rc=$?
  # Sanitizers print "ERROR: AddressSanitizer" / "runtime error" to the log.
  if grep -qE "AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer| LeakSanitizer" "$log"; then
    echo "  ---- sanitizer report tail ----"; grep -A6 -m1 -E "ERROR:|runtime error|WARNING: ThreadSanitizer" "$log" | head -20
    qa_record_fail "$name" "sanitizer report see $(basename "$log")"
    return 1
  fi
  if [ "$rc" -ne 0 ]; then
    echo "  ---- run log tail ----"; tail -15 "$log"
    qa_record_fail "$name" "exit=$rc see $(basename "$log")"
    return 1
  fi
  local line; line=$(grep -E "results:|passed|PASS" "$log" | tail -1)
  qa_record_pass "$name" "$line"
}

# Thermal needs unshare+bind-mock /sys/class to find thermal zones.
run_thermal_namespaced_san() {  # $1=bin $2=log
  local bin="$1" log="$2"
  local mock; mock=$(mktemp -d /tmp/aatpe_san_thermal.XXXXXX)
  mkdir -p "$mock/thermal/thermal_zone0" "$mock/thermal/thermal_zone1" \
           "$mock/thermal/thermal_zone2" "$mock/power_supply/battery"
  echo "mtktscpu" > "$mock/thermal/thermal_zone0/type"
  echo "skin" > "$mock/thermal/thermal_zone1/type"
  echo "battery" > "$mock/thermal/thermal_zone2/type"
  local rc=0
  : > "$log"
  for sc in normal emergency bootgrace battclamp freshness thread zonestick constclamp constclamp_throttled constclamp_cooling hal_parse; do
    echo 30000 > "$mock/thermal/thermal_zone0/temp"
    echo 30000 > "$mock/thermal/thermal_zone1/temp"
    echo 30000 > "$mock/thermal/thermal_zone2/temp"
    echo 300   > "$mock/power_supply/battery/temp"
    case "$sc" in
      normal)    echo 55000 > "$mock/thermal/thermal_zone0/temp"
                 echo 420   > "$mock/thermal/thermal_zone1/temp"
                 echo 350   > "$mock/power_supply/battery/temp" ;;
      emergency) echo 100000 > "$mock/thermal/thermal_zone0/temp" ;;
      bootgrace) echo 100000 > "$mock/thermal/thermal_zone0/temp" ;;
      battclamp) echo 900000 > "$mock/power_supply/battery/temp" ;;
      hal_parse) ;;  # HAL parse assertions: no mock temp changes needed
    esac
    echo "  --- scenario: $sc ---" >> "$log"
    AATPE_THERMAL_SCENARIO="$sc" unshare --map-root-user --mount bash -c "
      mount -t tmpfs tmpfs /sys/class
      mkdir -p /sys/class/thermal /sys/class/power_supply
      mount --bind '$mock/thermal' /sys/class/thermal
      mount --bind '$mock/power_supply' /sys/class/power_supply
      exec '$bin'
    " >> "$log" 2>&1 || rc=1
  done
  rm -rf "$mock"
  return $rc
}

run_mode() {  # $1=mode
  qa_set_mode "$1"
  local which="$2"
  while IFS='|' read -r name prod stubs drv; do
    [ -z "$name" ] && continue
    if [ "$which" = "tsan" ] && ! echo " $tsan_targets " | grep -q " $name "; then
      continue
    fi
    # build_and_run records its own FAIL on any failure path; do NOT mask it.
    build_and_run "$name" "$prod" "$stubs" "$drv"
  done < <(recipes)
}

case "$MODE" in
  asan|lsan) run_mode asan "all" ;;
  tsan)      run_mode tsan "tsan" ;;
  all)
    run_mode asan "all"
    echo ""; echo "##### now TSan #####"
    run_mode tsan "tsan"
    ;;
  *) echo "unknown mode: $MODE (asan|tsan|lsan|all)"; exit 2 ;;
esac
# LSan is folded into the ASan run (detect_leaks=1); surface it as its own row.
if [ "$MODE" = "asan" ] || [ "$MODE" = "all" ]; then
  if grep -rq "detected memory leaks" "$QA_DIR/artifacts"/san_asan_*.log 2>/dev/null; then
    qa_record_fail "LSan" "leak detected in san_asan_*.log"
  else
    qa_record_pass "LSan" "no leaks across ASan runs"
  fi
fi
qa_summary
[ "$QA_FAIL" -eq 0 ]
