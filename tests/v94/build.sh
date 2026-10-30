#!/bin/bash
set -u

CXX="${CXX:-g++}"
V94_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$V94_DIR/../.." && pwd)"
STUB="$SRC/tests/stubs"
OUT="$V94_DIR/build"
mkdir -p "$OUT"

TFLAGS="-std=c++17 -Wall -Wextra -Werror -fPIC -pthread -DAATPE_HOST_TEST"
TFLAGS="$TFLAGS -D__clang_version__=\"host-g++\""
PFLAGS="-std=c++17 -O1 -Wall -Wextra -fPIC -pthread -DAATPE_HOST_TEST"
INCLUDES="-I$SRC -I$STUB -I$SRC/shared -I$SRC/mech/include"

pass=0; fail=0
declare -a FAILURES=()

prod_obj() {
  local o="$OUT/$1.o"
  local src=""
  [ -f "$SRC/$1.cpp" ] && src="$SRC/$1.cpp"
  [ -z "$src" ] && [ -f "$SRC/$1.c" ] && src="$SRC/$1.c"
  [ -z "$src" ] && { echo "PROD COMPILE FAILED: $1 (no .cpp/.c found)"; return 1; }
  mkdir -p "$(dirname "$o")"
  if [ -f "$o" ] && [ "$src" -ot "$o" ]; then return 0; fi
  case "$src" in
    *.c)
      echo "  CC(prod-c) $1.c" >&2
      gcc -std=c11 -O1 -Wall -Wextra -fPIC -pthread $INCLUDES -c "$src" -o "$o" -lm 2>"$OUT/$1.err" || {
        echo "PROD COMPILE FAILED: $1"; head -20 "$OUT/$1.err"; return 1; }
      ;;
    *)
      echo "  CC(prod) $1.cpp" >&2
      g++ $PFLAGS $INCLUDES -c "$src" -o "$o" 2>"$OUT/$1.err" || {
        echo "PROD COMPILE FAILED: $1"; head -20 "$OUT/$1.err"; return 1; }
      ;;
  esac
}

stub_obj() {
  local o="$OUT/stub_$1.o"
  if [ -f "$o" ] && [ "$STUB/$1.cpp" -ot "$o" ]; then return 0; fi
  echo "  CC(stub) $1.cpp" >&2
  g++ $PFLAGS -I$SRC -c "$STUB/$1.cpp" -o "$o" 2>"$OUT/$1.err" || {
    echo "STUB COMPILE FAILED: $1"; head -20 "$OUT/$1.err"; return 1; }
}

build_test() {
  local name="$1" prod_basenames="$2" stub_basenames="$3" extra_defines="${4:-}"
  local tcpp="tests/v94/test_${name}.cpp"
  local bin="$OUT/test_${name}"
  if [ ! -f "$V94_DIR/test_${name}.cpp" ]; then
    echo "==> [$name]"
    echo "  [SKIPPED] tests/v94/test_${name}.cpp not present (planned, not yet ported)"
    return 2
  fi
  echo "==> [$name]"
  local objs=""
  for p in $prod_basenames; do
    local o="$OUT/$p.o"
    local psrc=""
    [ -f "$SRC/$p.cpp" ] && psrc="$SRC/$p.cpp"
    [ -z "$psrc" ] && [ -f "$SRC/$p.c" ] && psrc="$SRC/$p.c"
    [ -z "$psrc" ] && { echo "PROD COMPILE FAILED: $p (no .cpp/.c)"; return 1; }
    mkdir -p "$(dirname "$o")"
    if [ -n "$extra_defines" ]; then
      case "$psrc" in
        *.c)
          echo "  CC(prod-c) $p.c [$extra_defines]" >&2
          gcc -std=c11 -O1 -Wall -Wextra -fPIC -pthread $extra_defines $INCLUDES -c "$psrc" -o "$o" -lm 2>"$OUT/$p.err" || {
            echo "PROD COMPILE FAILED: $p"; head -20 "$OUT/$p.err"; return 1; }
          ;;
        *)
          echo "  CC(prod) $p.cpp [$extra_defines]" >&2
          g++ $PFLAGS $extra_defines $INCLUDES -c "$psrc" -o "$o" 2>"$OUT/$p.err" || {
            echo "PROD COMPILE FAILED: $p"; head -20 "$OUT/$p.err"; return 1; }
          ;;
      esac
    else
      prod_obj "$p" || return 1
    fi
    objs="$objs $o"
  done
  for s in $stub_basenames; do stub_obj "$s" || return 1; objs="$objs $OUT/stub_$s.o"; done
  echo "  CC(test) test_${name}.cpp" >&2
  if ! g++ $TFLAGS $extra_defines $INCLUDES -c "$V94_DIR/test_${name}.cpp" -o "$OUT/test_${name}.o" 2>"$OUT/${name}.err"; then
    echo "  [FAIL] test_${name}.cpp does not compile under -Werror:"
    head -30 "$OUT/${name}.err"; return 1; fi
  if ! g++ $TFLAGS $objs "$OUT/test_${name}.o" -o "$bin" -lz 2>"$OUT/${name}.link.err"; then
    echo "  [FAIL] link:"; head -30 "$OUT/${name}.link.err"; return 1; fi
  if [ "${SKIP_RUN:-0}" = "1" ]; then echo "  (compiled, SKIP_RUN=1)"; return 0; fi
  echo "  RUN:"
  if [ -n "${RUN_CMD:-}" ]; then
    BIN="$bin" bash -c "$RUN_CMD" && return 0 || { echo "  [FAIL] exit=$?"; return 1; }
  fi
  "$bin" && return 0 || { echo "  [FAIL] exit=$?"; return 1; }
}

run_thermal_namespaced() {
  local bin="$BIN"
  local mock; mock=$(mktemp -d /tmp/aatpe_thermal.XXXXXX)
  mkdir -p "$mock/thermal/thermal_zone0" "$mock/thermal/thermal_zone1" \
           "$mock/thermal/thermal_zone2" "$mock/power_supply/battery"
  local rc=0
  for sc in normal emergency bootgrace battclamp freshness degrdecouple hwmontempn pmicnotcpu thread zonestick constclamp constclamp_throttled constclamp_cooling boot0 mcjitter clampregress fallbackonly secondary throttle0 noskin nobatt; do
    echo "mtktscpu" > "$mock/thermal/thermal_zone0/type"
    echo "skin"     > "$mock/thermal/thermal_zone1/type"
    echo "battery"  > "$mock/thermal/thermal_zone2/type"
    echo 30000 > "$mock/thermal/thermal_zone0/temp"
    echo 30000 > "$mock/thermal/thermal_zone1/temp"
    echo 30000 > "$mock/thermal/thermal_zone2/temp"
    echo 300   > "$mock/power_supply/battery/temp"
    case "$sc" in
      normal)
        echo 55000 > "$mock/thermal/thermal_zone0/temp"
        echo 420   > "$mock/thermal/thermal_zone1/temp"
        echo 350   > "$mock/power_supply/battery/temp" ;;
      emergency)
        echo 100000 > "$mock/thermal/thermal_zone0/temp" ;;
      bootgrace) echo 100000 > "$mock/thermal/thermal_zone0/temp" ;;
      battclamp)
        echo 900000 > "$mock/power_supply/battery/temp" ;;
      noskin)
        echo "ap_therm" > "$mock/thermal/thermal_zone1/type" ;;
      nobatt)
        rm -f "$mock/power_supply/battery/temp" ;;
    esac
    echo "  --- scenario: $sc ---"
    AATPE_THERMAL_SCENARIO="$sc" unshare --map-root-user --mount bash -c "
      mount -t tmpfs tmpfs /sys/class
      mkdir -p /sys/class/thermal /sys/class/power_supply
      mount --bind '$mock/thermal' /sys/class/thermal
      mount --bind '$mock/power_supply' /sys/class/power_supply
      exec '$bin'
    " || rc=1
  done
  rm -rf "$mock"
  return $rc
}

run_battery_namespaced() {
  local bin="$BIN"
  local mock; mock=$(mktemp -d /tmp/aatpe_battery.XXXXXX)
  mkdir -p "$mock/power_supply/battery"
  local rc=0
  for sc in power empty good_new aged_soh65 aged_soh40 aged_soh20 \
            overheat dead cycle_only_700 cycle_only_1k no_design fresh_spurious_soh; do
    rm -rf "$mock/power_supply/battery"
    mkdir -p "$mock/power_supply/battery"
    case "$sc" in
      power)
        ;;
      empty)
        ;;
      good_new)
        echo "Good"      > "$mock/power_supply/battery/health"
        echo 100         > "$mock/power_supply/battery/cycle_count"
        echo 4000000     > "$mock/power_supply/battery/charge_full"
        echo 4000000     > "$mock/power_supply/battery/charge_full_design" ;;
      aged_soh65)
        echo "Good"      > "$mock/power_supply/battery/health"
        echo 400         > "$mock/power_supply/battery/cycle_count"
        echo 2600000     > "$mock/power_supply/battery/charge_full"
        echo 4000000     > "$mock/power_supply/battery/charge_full_design" ;;
      aged_soh40)
        echo "Good"      > "$mock/power_supply/battery/health"
        echo 400         > "$mock/power_supply/battery/cycle_count"
        echo 1600000     > "$mock/power_supply/battery/charge_full"
        echo 4000000     > "$mock/power_supply/battery/charge_full_design" ;;
      aged_soh20)
        echo "Good"      > "$mock/power_supply/battery/health"
        echo 400         > "$mock/power_supply/battery/cycle_count"
        echo 800000      > "$mock/power_supply/battery/charge_full"
        echo 4000000     > "$mock/power_supply/battery/charge_full_design" ;;
      overheat)
        echo "Overheat"  > "$mock/power_supply/battery/health"
        echo 100         > "$mock/power_supply/battery/cycle_count" ;;
      dead)
        echo "Dead"      > "$mock/power_supply/battery/health"
        echo 100         > "$mock/power_supply/battery/cycle_count" ;;
      cycle_only_700)
        echo "Good"      > "$mock/power_supply/battery/health"
        echo 700         > "$mock/power_supply/battery/cycle_count" ;;
      cycle_only_1k)
        echo "Good"      > "$mock/power_supply/battery/health"
        echo 1000        > "$mock/power_supply/battery/cycle_count" ;;
      no_design)
        echo "Good"      > "$mock/power_supply/battery/health"
        echo 100         > "$mock/power_supply/battery/cycle_count"
        echo 3000000     > "$mock/power_supply/battery/charge_full" ;;
      fresh_spurious_soh)
        echo "Good"      > "$mock/power_supply/battery/health"
        echo 1           > "$mock/power_supply/battery/cycle_count"
        echo 2800000     > "$mock/power_supply/battery/charge_full"
        echo 4000000     > "$mock/power_supply/battery/charge_full_design" ;;
    esac
    echo "  --- scenario: $sc ---"
    AATPE_BATTERY_SCENARIO="$sc" unshare --map-root-user --mount bash -c "
      mount -t tmpfs tmpfs /sys/class
      mkdir -p /sys/class/power_supply
      mount --bind '$mock/power_supply' /sys/class/power_supply
      exec '$bin'
    " || rc=1
  done
  rm -rf "$mock"
  return $rc
}

run_a2_boost_namespaced() {
  local bin="$BIN"
  unshare --map-root-user --mount bash -c "
    mount -t tmpfs tmpfs /dev &&
    mkdir -p /dev/stune/top-app &&
    echo 0 > /dev/stune/top-app/schedtune.boost &&
    exec '$bin'
  "
}

run_cli_namespaced() {
  local bin="$BIN"
  unshare --map-root-user --mount bash -c "
    mount -t tmpfs tmpfs /dev &&
    mkdir -p /dev/aatpe/state &&
    exec '$bin'
  "
}

run_one() {
  local t="$1"
  case "$t" in
    observability) build_test observability "aatpe_observability" "log_stub" ;;
    uevent)        build_test uevent        "uevent_monitor"     "log_stub" ;;
    event_monitor) build_test event_monitor "event_monitor uevent_monitor aatpe_ml" "log_stub v94_daemon_deps" ;;
    interfaces)    build_test interfaces   "aatpe_interfaces aatpe_ml" "v94_daemon_deps log_stub" ;;
    webui_json)    build_test webui_json    "webui_json aatpe_ml"   "webui_clusters_deps v94_daemon_deps log_stub scenewire" ;;
    webui_percpu)  build_test webui_percpu  "webui_json aatpe_ml"   "webui_clusters_deps v94_daemon_deps log_stub scenewire" ;;
    thermal)       export -f run_thermal_namespaced
                   RUN_CMD='run_thermal_namespaced' build_test thermal "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff" "thermal_deps log_stub" ;;
    cli)           export -f run_cli_namespaced
                   RUN_CMD='run_cli_namespaced' build_test cli "aatpe_ml aatped_apklabel" "log_stub" ;;
    probe)         build_test probe        ""                   "" ;;
    watchdog)      build_test watchdog     ""                   "" ;;
    scoring_active) build_test scoring_active ""                   "" ;;
    rel15_freeze)   build_test rel15_freeze   ""                   "" ;;
    watchdog_hotfix) build_test watchdog_hotfix ""                "" ;;
    rt_restore)        build_test rt_restore        ""                   "" "-DAATPE_HOST_TEST" ;;
    priority_thermal_derate) build_test priority_thermal_derate ""                  "" "-DAATPE_HOST_TEST" ;;
    exec_timeout)      build_test exec_timeout     "aatped_utils"       "" "-DAATPE_HOST_TEST" ;;
    thermal_clusters)  build_test thermal_clusters "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff"    "thermal_deps log_stub" "-DAATPE_HOST_TEST" ;;
    emergency_recovery) build_test emergency_recovery "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff" "thermal_deps log_stub" "-DAATPE_HOST_TEST" ;;
    emergency_timing)  build_test emergency_timing "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff" "thermal_deps log_stub" "-DAATPE_HOST_TEST" ;;
    coldstart_release) build_test coldstart_release ""                   "" "-DAATPE_HOST_TEST" ;;
    coldinstance)     build_test coldinstance     ""                   "" "-DAATPE_HOST_TEST" ;;
    gpu_devfreq_recovery) build_test gpu_devfreq_recovery "gpu_boost"    "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    ebpf_cpu_mask)     build_test ebpf_cpu_mask    "aatpe_ebpf aatpe_subsystem aatped_utils integrity" "ebpf_deps log_stub hybrid_deps utils_deps sprop_stub" ;;
    ml_pool_feature)   build_test ml_pool_feature  ""                   "" ;;
    numpools_d1100)    build_test numpools_d1100   ""                   "" ;;
    thermal_gate)      build_test thermal_gate     "aatpe_ml"          "thermal_deps log_stub" ;;
    degraded_gate)     build_test degraded_gate    ""                   "" ;;
    diag_gpu)          build_test diag_gpu         "aatpe_diag"        "" ;;
    a1_sf)            build_test a1_sf            "aatped_threadutil" "" ;;
    a2_boost)         export -f run_a2_boost_namespaced
                      RUN_CMD='run_a2_boost_namespaced' build_test a2_boost "gpu_boost" "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    a3_autocal)       build_test a3_autocal       "aatpe_caps" "autocal_deps log_stub" "-DAATPE_HOST_TEST" ;;
    a1_placement)     build_test a1_placement "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    d1recovery)       build_test d1recovery "aatpe_recovery" "" ;;
    h14_cgroup_rec)  build_test h14_cgroup_rec "aatpe_recovery" "" ;;
    h14_cgroup_e2e)  build_test h14_cgroup_e2e "aatpe_placement_mech aatpe_recovery" "log_stub" ;;
    d2exec)           build_test d2exec "aatped_utils" "utils_deps log_stub sprop_stub" ;;
    d3hotplug)        build_test d3hotplug "aatpe_ebpf aatpe_subsystem aatped_utils integrity" "ebpf_deps log_stub hybrid_deps utils_deps sprop_stub" ;;
    caps)          build_test caps         "aatpe_caps" "log_stub" ;;
    diag)          build_test diag         "aatpe_diag"         "log_stub" ;;
    ext_neg_psi)   build_test ext_neg_psi  "aatpe_diag"         "log_stub" ;;
    mech_uclamp)   build_test mech_uclamp  "aatpe_placement_mech aatpe_caps"  "log_stub" ;;
    coldstart)     build_test coldstart    "cold_start_rules aatpe_recovery aatpe_ml"   "placement_deps ml_deps recovery_caps_stub" "-DAATPE_HOST_TEST" ;;
    thermal_hal)   build_test thermal_hal  "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff"            "v94_daemon_deps log_stub" ;;
    thermal_ff_intg)
      echo "==> [thermal_ff_intg]"
      mkdir -p "$OUT"
      if ! gcc -std=c11 -O1 -Wall -Wextra -fPIC -pthread $INCLUDES -c "$SRC/thermal_ff/thermal_ff.c" -o "$OUT/thermal_ff/thermal_ff.o" -lm 2>"$OUT/thermal_ff_intg.c.err"; then
        echo "  [FAIL] thermal_ff.c compile"; head -20 "$OUT/thermal_ff_intg.c.err"; return 1; fi
      if ! g++ $TFLAGS -I$SRC/thermal_ff -c "$SRC/thermal_ff/test/test_integration.cpp" -o "$OUT/thermal_ff_intg.o" 2>"$OUT/thermal_ff_intg.t.err"; then
        echo "  [FAIL] test_integration.cpp does not compile under -Werror:"; head -30 "$OUT/thermal_ff_intg.t.err"; return 1; fi
      if ! g++ $TFLAGS "$OUT/thermal_ff/thermal_ff.o" "$OUT/thermal_ff_intg.o" -o "$OUT/test_thermal_ff_intg" -lm 2>"$OUT/thermal_ff_intg.link.err"; then
        echo "  [FAIL] link:"; head -30 "$OUT/thermal_ff_intg.link.err"; return 1; fi
      echo "  RUN:"
      "$OUT/test_thermal_ff_intg" && return 0 || { echo "  [FAIL] exit=$?"; return 1; }
      ;;
    gpu_boost)     build_test gpu_boost    "gpu_boost"          "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    gpu_boost_thermal) build_test gpu_boost_thermal "gpu_boost" "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    ebpf_classify) build_test ebpf_classify "aatpe_ebpf aatpe_subsystem integrity" "ebpf_deps log_stub" ;;
    ebpf_event_parse) build_test ebpf_event_parse "aatpe_ebpf aatpe_subsystem" "ebpf_deps log_stub" "-DAATPE_HOST_TEST" ;;
    ebpf_failsafe) build_test ebpf_failsafe "aatpe_ebpf aatpe_subsystem" "log_stub ebpf_deps hybrid_deps" "-DAATPE_HOST_TEST" ;;
    ebpf_elf_robust_v449) build_test ebpf_elf_robust_v449 "aatpe_ebpf aatpe_subsystem" "log_stub ebpf_deps hybrid_deps" "-DAATPE_HOST_TEST" ;;
    ebpf_p3_fixes) build_test ebpf_p3_fixes "aatpe_ebpf aatpe_subsystem" "ebpf_deps log_stub" "-DAATPE_HOST_TEST" ;;
    ebpf_p1_fix)   build_test ebpf_p1_fix "aatpe_ebpf aatpe_subsystem" "ebpf_deps log_stub" "-DAATPE_HOST_TEST" ;;
    ebpf_h01h02) build_test ebpf_h01h02 "aatpe_ebpf aatpe_subsystem" "ebpf_deps log_stub" "-DAATPE_HOST_TEST" ;;
    crypto_v449)          build_test crypto_v449          "" "" ;;
    session_v449)         build_test session_v449         "aatpe_session_token" "log_stub" ;;
    uclamp_restart_v449) build_test uclamp_restart_v449 "" "" "-DAATPE_HOST_TEST" ;;
    thermal_hardwall_v449) build_test thermal_hardwall_v449 "aatped_arbitrator mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "" "-UAATPED_ARBITRATOR_DAEMON" ;;
    vmguest_race_v449)   build_test vmguest_race_v449   "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    freezer_scene_v449)   build_test freezer_scene_v449   "c4_freezer" "" "-UAATPE_HOST_TEST" ;;
    preboost_scene_v449)  build_test preboost_scene_v449  "pre_boost_predict" "" "-UAATPE_HOST_TEST -O2" ;;
    loadforecast_scene_v449) build_test loadforecast_scene_v449 "mech/src/load_forecast" "" "" ;;
    uevent_scene_v449)    build_test uevent_scene_v449    "uevent_monitor" "log_stub" "" ;;
    caps_pure)     build_test caps_pure     "aatpe_caps" "autocal_deps log_stub" ;;
    command_dispatcher) build_test command_dispatcher "aatpe_command_dispatcher" "log_stub" ;;
    c2_envelope)   build_test c2_envelope  "c2_envelope scene_infer" "" ;;
    spsc_queue)    build_test spsc_queue   ""                   "" ;;
    ml)            build_test ml           "aatpe_ml"           "ml_deps" ;;
    scoring)       build_test scoring      "scoring adrc aatpe_ml"    "scoring_deps ml_deps log_stub" ;;
    scoring_pure)  build_test scoring_pure  "scoring adrc aatpe_ml"    "scoring_deps ml_deps log_stub" ;;
    placement)     build_test placement    "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    bigpool_race)  build_test bigpool_race "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    a2_atomics)    build_test a2_atomics "" "" "-DAATPE_HOST_TEST" ;;
    spillover_rt)  build_test spillover_rt "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    h13_hotplug)   build_test h13_hotplug  "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    vm_guest)      build_test vm_guest     "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    abtest)        build_test abtest        "aatped_abtest"        "log_stub" ;;
    gamescene)     build_test gamescene     "aatped_gamescene"     "gamescene_deps log_stub" ;;
    arbitrator)    build_test arbitrator    "aatped_arbitrator mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "" "-UAATPED_ARBITRATOR_DAEMON" ;;
    arb_wiring)    build_test arb_wiring    "aatped_arbitrator mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "" "-UAATPED_ARBITRATOR_DAEMON" ;;
    battery)       export -f run_battery_namespaced
                   RUN_CMD='run_battery_namespaced' build_test battery "aatped_battery" "battery_deps" ;;
    gpu)           build_test gpu          "aatped_gpu aatpe_ml" "gpu_deps ml_deps log_stub" ;;
    audio)         build_test audio        "aatped_audio"         "audio_deps log_stub" ;;
    applaunch)     build_test applaunch    "aatped_applaunch"     "applaunch_deps log_stub" ;;
    logging)       build_test logging      "aatped_logging"       "logging_deps" ;;
    log_orphan)    build_test log_orphan   "aatped_logging"       "logging_deps" "-DAATPE_HOST_TEST" ;;
    utils)         build_test utils        "aatped_utils aatpe_helper_audit"         "utils_deps log_stub sprop_stub" ;;
    uevent_fallback) build_test uevent_fallback "aatped_uevent event_monitor uevent_monitor aatpe_ml" "v94_daemon_deps log_stub uevent_deps" ;;
    ebpf_agg)      build_test ebpf_agg     "aatped_ebpf_agg"      "ebpf_agg_deps log_stub" ;;
    ml_hybrid_main)   build_test ml_hybrid_main   "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_train)  build_test ml_hybrid_train  "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_predict) build_test ml_hybrid_predict "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_persist) build_test ml_hybrid_persist "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_alpha)  build_test ml_hybrid_alpha  "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    fed_main)      build_test fed_main      "aatpe_federated_main aatpe_federated_aggregate" "log_stub" ;;
    fed_aggregate) build_test fed_aggregate "aatpe_federated_main aatpe_federated_aggregate" "log_stub" ;;
    hmac)         build_test hmac          "aatped_hmac"            "log_stub" "-DAATPE_HOST_TEST" ;;
    topology)     build_test topology      "aatped_topology aatped_gpu aatpe_ml"  "v94_daemon_deps log_stub topology_deps" ;;
    config)       build_test config       "aatped_config aatpe_ml aatped_hmac aatpe_config_schema aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub config_deps hybrid_deps ml_deps" ;;
    state)        build_test state        "aatped_state aatpe_ml"  "state_deps log_stub ml_deps" ;;
    control)      build_test control      "aatped_control aatpe_ml aatpe_command_dispatcher aatpe_command_queue aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    active_lock)  build_test active_lock  "aatped_control aatpe_ml aatpe_command_dispatcher aatpe_command_queue aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    cmdq)         build_test cmdq         "aatpe_command_queue aatped_control aatpe_ml aatpe_command_dispatcher aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    thread_scan)  build_test thread_scan  "thread_scan aatpe_ml aatped_threadutil" "v94_daemon_deps log_stub thread_deps" ;;
    thread_scan_hydrate) build_test thread_scan_hydrate "thread_scan aatpe_ml aatped_threadutil" "v94_daemon_deps log_stub thread_deps" "-DAATPE_HOST_TEST" ;;
    threadutil)   build_test threadutil   "aatped_threadutil"             "" ;;
    frame)        build_test frame        "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"        "log_stub frame_deps" ;;
    fx3_frame_v449) build_test fx3_frame_v449 "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" "-DAATPE_HOST_TEST" ;;
    game_layer)   build_test game_layer   "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"        "log_stub frame_deps" ;;
    f01_fps)      build_test f01_fps      "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"        "log_stub frame_deps" ;;
    per_app_frame) build_test per_app_frame "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"    "log_stub frame_deps" ;;
    per_app_frame_r5r6) build_test per_app_frame_r5r6 "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    frame_p2_fixes) build_test frame_p2_fixes "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    frame_rel12) build_test frame_rel12 "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    pkglist_parsing) build_test pkglist_parsing "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    webui_http)   build_test webui_http   "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel"   "log_stub webui_deps gamelist_stub" ;;
    webui_http_v449) build_test webui_http_v449 "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel" "log_stub webui_deps gamelist_stub" ;;
    cmdq_v449)       build_test cmdq_v449       "aatpe_command_queue aatped_control aatpe_ml aatpe_command_dispatcher aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    cmdq_replaysafe) build_test cmdq_replaysafe "aatpe_command_queue aatped_control aatpe_ml aatpe_command_dispatcher aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    interfaces_v449) build_test interfaces_v449 "aatpe_interfaces aatpe_ml" "v94_daemon_deps log_stub" ;;
    logging_v449)    build_test logging_v449    "aatped_logging" "logging_deps" ;;
    webui_authz)  build_test webui_authz "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel"   "log_stub webui_deps gamelist_stub" ;;
    ebpf)         build_test ebpf         "aatpe_ebpf aatpe_subsystem"            "log_stub ebpf_deps hybrid_deps" ;;
    v343_ml)      build_test v343_ml      "aatpe_ml"             "ml_deps log_stub" ;;
    ml_empty_pool) build_test ml_empty_pool "aatpe_ml"           "ml_deps log_stub" ;;
    ml_shadow_v450) build_test ml_shadow_v450 "aatpe_ml"          "ml_deps log_stub" ;;
    ml_shadow_passive_v600) build_test ml_shadow_passive_v600 "scoring adrc aatpe_ml" "scoring_deps ml_deps log_stub" ;;
    cpu_metrics_race_v600) build_test cpu_metrics_race_v600 "" "scoring_deps log_stub" ;;
    backup_model_race_v600) build_test backup_model_race_v600 "" "" ;;
    wakeup_reaper_v450) build_test wakeup_reaper_v450 "aatpe_ebpf aatpe_subsystem" "log_stub ebpf_deps hybrid_deps" "-DAATPE_HOST_TEST" ;;
    f_v434_ml)    build_test f_v434_ml    "aatpe_ml"             "ml_deps log_stub" ;;
    f_v434_hybrid) build_test f_v434_hybrid "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    v343_placement) build_test v343_placement "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" ;;
    v342_round4)  build_test v342_round4  "aatpe_session_token"    "log_stub" ;;
    v342_round5)  build_test v342_round5  "aatpe_subsystem"   "log_stub" ;;
    subsystem)    build_test subsystem    "aatpe_subsystem"   "log_stub" "-DAATPE_HOST_TEST" ;;
    profile_arb)  build_test profile_arb  ""   "" ;;
    profile_stress) build_test profile_stress ""   "" ;;
    recovery)     build_test recovery      "aatpe_recovery"   "" ;;
    main)         prod_obj aatped_main && objcopy --strip-symbol=main "$OUT/aatped_main.o"
                  g++ $PFLAGS $INCLUDES -DAATPED_ARBITRATOR_DAEMON -c "$SRC/aatped_arbitrator.cpp" -o "$OUT/aatped_arbitrator.o"
                  build_test main          "aatped_main aatped_battery aatpe_ml aatpe_ebpf aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatpe_federated_main aatpe_federated_aggregate aatped_ebpf_agg event_monitor uevent_monitor frame_binder adrc integrity aatpe_state_ttl aatpe_helper_audit aatpe_managers aatpe_config_schema aatpe_command_dispatcher aatpe_session_token aatped_priority aatpe_recovery aatpe_command_queue aatpe_subsystem cold_start_rules gpu_boost input_monitor aatped_psi psi_decision aatpe_placement_mech touch_uclamp touch_uclamp_act aatped_arbitrator pre_boost_predict thermal_headroom mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "log_stub main_deps hybrid_deps ebpf_deps recovery_caps_stub touch_host_stub scenewire" ;;
    integrity)    build_test integrity     "integrity"                       "integrity_deps log_stub" ;;
    adrc)         build_test adrc          "adrc"                            "log_stub" ;;
    frame_binder) build_test frame_binder ""                                "frame_deps log_stub" ;;
    gputop_p2_fixes) build_test gputop_p2_fixes "gpu_boost" "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    config_schema) build_test config_schema "aatpe_config_schema"              "log_stub" ;;
    apklabel)      build_test apklabel      "aatped_apklabel"                 "" ;;
    apklabel_multi) build_test apklabel_multi "aatped_apklabel"               "" ;;
    spillover_deadlock) build_test spillover_deadlock ""                        "" ;;
    pid_identity_exe) build_test pid_identity_exe ""                           "" ;;
    framestats_d06) build_test framestats_d06 "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    aatpe_iso)      build_test aatpe_iso      "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    p0_v441)        build_test p0_v441        "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel placement_heal_decision" "log_stub frame_deps" ;;
    webui_cors)     build_test webui_cors     "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel" "log_stub webui_deps gamelist_stub" ;;
    webui_delta_lru) build_test webui_delta_lru "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel" "log_stub webui_deps gamelist_stub" ;;
    webui_p3_http) build_test webui_p3_http "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel" "log_stub webui_deps gamelist_stub" ;;
    psi_fallback)   build_test psi_fallback   "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    thermzone)          build_test thermzone          ""                              "" ;;
    thermzone_fixtures) build_test thermzone_fixtures ""                              "" ;;
    state_ttl_d)       build_test state_ttl_d       "aatpe_state_ttl"              "ttl_deps" ;;
    d_arithmetic)     build_test d_arithmetic     ""                              "" ;;
    preboost)         build_test preboost         "pre_boost_predict"             "" "-DAATPE_HOST_TEST" ;;
    thermheadroom)    build_test thermheadroom    "thermal_headroom"             "" "-DAATPE_HOST_TEST" ;;
    scene_infer)      build_test scene_infer      "scene_infer c2_envelope"     "" "-DAATPE_HOST_TEST" ;;
    scene_wiring)     build_test scene_wiring     "scene_infer c2_envelope scene_infer_wiring" "" "-DAATPE_HOST_TEST" ;;
    tidy_hardening) build_test tidy_hardening "aatped_logging" "" "-DAATPE_HOST_TEST" ;;
    c4_freezer)       build_test c4_freezer       "c4_freezer"              "" "-DAATPE_HOST_TEST" ;;
    c4_freezer_glue)  build_test c4_freezer_glue  "c4_freezer"              "" "-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM" ;;
    c4_freezer_v1)    build_test c4_freezer_v1    "c4_freezer"              "" "-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM" ;;
    c4_freezer_rel12) build_test c4_freezer_rel12 "c4_freezer"              "" "-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM" ;;
    c4_freezer_mounts) build_test c4_freezer_mounts "c4_freezer"            "" "-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM" ;;
    c4_freezer_autogame) build_test c4_freezer_autogame "c4_freezer"        "" "-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM" ;;
    c4_freezer_bucket) build_test c4_freezer_bucket "c4_freezer"            "" "-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM" ;;
    *) echo "unknown test: $t (authoritative set: observability uevent event_monitor interfaces webui_json thermal cli probe watchdog; spsc_queue/ml/scoring/placement exist only as SKIPPED recipes)" ; return 1 ;;
  esac
}

ALL="observability uevent event_monitor interfaces webui_json thermal cli probe watchdog scoring placement spillover_rt \
abtest gamescene arbitrator arb_wiring battery gpu audio applaunch logging utils uevent_fallback ebpf_agg \
ml_hybrid_main ml_hybrid_train ml_hybrid_predict ml_hybrid_persist ml_hybrid_alpha \
fed_main fed_aggregate hmac topology config state control active_lock \
thread_scan threadutil frame webui_http ebpf main v343_ml f_v434_ml f_v434_hybrid v343_placement v342_round4 profile_arb profile_stress recovery cmdq v342_round5 \
integrity adrc frame_binder config_schema caps diag mech_uclamp coldstart coldinstance thermal_hal thermal_ff_intg gpu_boost gpu_boost_thermal ebpf_classify watchdog_hotfix rt_restore exec_timeout thermal_clusters coldstart_release gpu_devfreq_recovery ebpf_cpu_mask ml_pool_feature \
numpools_d1100 thermal_gate degraded_gate diag_gpu \
a1_sf a2_boost a3_autocal a1_placement d1recovery d2exec d3hotplug game_layer f01_fps apklabel apklabel_multi per_app_frame_r5r6 spillover_deadlock pid_identity_exe framestats_d06 aatpe_iso p0_v441 webui_cors psi_fallback thermzone thermzone_fixtures state_ttl_d d_arithmetic preboost thermheadroom scene_infer scene_wiring c4_freezer c4_freezer_glue c4_freezer_v1 c4_freezer_mounts c4_freezer_autogame c4_freezer_bucket tidy_hardening pkglist_parsing priority_thermal_derate bigpool_race a2_atomics log_orphan subsystem thread_scan_hydrate ebpf_event_parse caps_pure command_dispatcher c2_envelope ebpf_failsafe uclamp_restart_v449 thermal_hardwall_v449 emergency_recovery emergency_timing vmguest_race_v449 freezer_scene_v449 preboost_scene_v449 loadforecast_scene_v449 uevent_scene_v449 scoring_pure ebpf_elf_robust_v449 ebpf_p3_fixes ebpf_p1_fix crypto_v449 session_v449 fx3_frame_v449 webui_http_v449 cmdq_v449 cmdq_replaysafe interfaces_v449 logging_v449 ml_empty_pool ml_shadow_v450 wakeup_reaper_v450 ml_shadow_passive_v600 cpu_metrics_race_v600 backup_model_race_v600 gputop_p2_fixes webui_p3_http scoring_active rel15_freeze webui_percpu"
if [ "$#" -eq 0 ]; then set -- $ALL; fi
pass=0; fail=0; skipped=0
declare -a FAILURES=()
declare -a SKIPPED=()
for t in "$@"; do
  run_one "$t"
  rc=$?
  if   [ "$rc" -eq 0 ]; then pass=$((pass+1))
  elif [ "$rc" -eq 2 ]; then skipped=$((skipped+1)); SKIPPED+=("$t")
  else fail=$((fail+1)); FAILURES+=("$t")
  fi
done

echo "==================================================================="
echo "v94 unit tests: $((pass)) ok, $((fail)) failed, $((skipped)) skipped"
[ "$fail" -eq 0 ] || { echo "FAILED: ${FAILURES[*]}"; exit 1; }
exit 0
