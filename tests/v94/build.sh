#!/bin/bash
# =============================================================================
# AATPE v94 — unified build + run for the hermetic v94 unit tests (P2 issue #32).
#
# P2-18 (round-2): the authoritative test set is enumerated in the
# `case` below (do NOT hard-code a count here — it drifted from "9" while the
# set grew). Today the buildable targets are:
#   observability uevent event_monitor interfaces webui_json thermal cli probe
#   watchdog config state control thread_scan frame webui_http e2e main
# (thread_scan / frame / e2e / main are the ones that link heavier production
# TUs; spsc_queue/ml/scoring/placement exist only as SKIPPED recipes.)
#
# Each test links the REAL production translation unit(s) it exercises (no mocks
# of production logic). Android-only symbols (__system_property_get, android/log)
# are provided by host stubs under tests/stubs/.
#
# Usage:
#   bash tests/v94/build.sh            # build + run all buildable targets
#   bash tests/v94/build.sh <name>     # build + run one (e.g. observability)
#   SKIP_RUN=1 bash tests/v94/build.sh # compile only (do not execute binaries)
#
# Flags policy (P2 issue #33):
#   - The TEST .cpp files are compiled with -std=c++17 -Wall -Wextra -Werror
#     (warnings are errors for the v94 tests themselves).
#   - Production .cpp files are compiled with -w (they are owned by the daemon
#     build and carry their own warnings policy; -Wall -Werror is enforced only
#     on the test files, per issue #33's scope).
# =============================================================================
set -u

CXX="${CXX:-g++}"
# paths relative to tests/v94/build.sh
V94_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$V94_DIR/../.." && pwd)"          # v94_source root
STUB="$SRC/tests/stubs"
OUT="$V94_DIR/build"
mkdir -p "$OUT"

TFLAGS="-std=c++17 -Wall -Wextra -Werror -fPIC -pthread -DAATPE_HOST_TEST"
# g++ (host) does not predefine __clang_version__ (an NDK clang builtin). The
# production aatpe-cli.cpp references it; define a neutral placeholder so the
# #include-based host test compiles. Harmless for all other tests.
TFLAGS="$TFLAGS -D__clang_version__=\"host-g++\""
# v4.2.9 E6: production .o are now compiled with -Wall -Wextra (was -w, which hid
# every diagnostic). Deliberately NO -Werror: historical warnings must surface for
# the report but must not turn the build red. Test .cpp files already carry
# -Wall -Wextra -Werror in TFLAGS above.
# RC6 (C6): every HOST build defines AATPE_HOST_TEST so the guarded *ForTest seams
# remain linkable. The release/NDK build does NOT define it, so those symbols are
# absent from production binaries (zero *ForTest in release).
PFLAGS="-std=c++17 -O1 -Wall -Wextra -fPIC -pthread -DAATPE_HOST_TEST"
INCLUDES="-I$SRC -I$STUB -I$SRC/shared -I$SRC/mech/include"

pass=0; fail=0
declare -a FAILURES=()

# compile a production .cpp into build/<name>.o (rebuild if source newer)
prod_obj() {  # $1 = prod basename (no ext); supports .cpp (g++) and .c (gcc -std=c11, v4.3.6 pure cores)
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

# compile a stub .cpp once (rebuild if source newer than object)
stub_obj() {  # $1 = stub basename (no ext), under tests/stubs/
  local o="$OUT/stub_$1.o"
  if [ -f "$o" ] && [ "$STUB/$1.cpp" -ot "$o" ]; then return 0; fi
  echo "  CC(stub) $1.cpp" >&2
  g++ $PFLAGS -I$SRC -c "$STUB/$1.cpp" -o "$o" 2>"$OUT/$1.err" || {
    echo "STUB COMPILE FAILED: $1"; head -20 "$OUT/$1.err"; return 1; }
}

# build+link one test. args: $1=testname $2=prod basenames(space) $3=stub basenames(space) [$4=extra defines]
build_test() {
  local name="$1" prod_basenames="$2" stub_basenames="$3" extra_defines="${4:-}"
  local tcpp="tests/v94/test_${name}.cpp"
  local bin="$OUT/test_${name}"
  # Honest SKIP: a recipe exists for a test whose .cpp has not been ported yet
  # (spsc_queue/ml/scoring/placement). Report SKIPPED (exit 2) instead of a
  # fake [FAIL] from g++ "No such file". These are deliberately OUT of the
  # default ALL set; explicit `build.sh <name>` surfaces them as SKIPPED.
  if [ ! -f "$V94_DIR/test_${name}.cpp" ]; then
    echo "==> [$name]"
    echo "  [SKIPPED] tests/v94/test_${name}.cpp not present (planned, not yet ported)"
    return 2
  fi
  echo "==> [$name]"
  local objs=""
  for p in $prod_basenames; do
    local o="$OUT/$p.o"
    # v4.3.6: detect .cpp vs .c (pure C cores like psi_decision/touch_uclamp/thermal_ff)
    local psrc=""
    [ -f "$SRC/$p.cpp" ] && psrc="$SRC/$p.cpp"
    [ -z "$psrc" ] && [ -f "$SRC/$p.c" ] && psrc="$SRC/$p.c"
    [ -z "$psrc" ] && { echo "PROD COMPILE FAILED: $p (no .cpp/.c)"; return 1; }
    mkdir -p "$(dirname "$o")"
    if [ -n "$extra_defines" ]; then
      # Force-recompile with extra defines (test-only hooks like AATPE_GPU_BOOST_TEST)
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
  # strict test-object compile
  echo "  CC(test) test_${name}.cpp" >&2
  if ! g++ $TFLAGS $extra_defines $INCLUDES -c "$V94_DIR/test_${name}.cpp" -o "$OUT/test_${name}.o" 2>"$OUT/${name}.err"; then
    echo "  [FAIL] test_${name}.cpp does not compile under -Werror:"
    head -30 "$OUT/${name}.err"; return 1; fi
  # link (aatped_apklabel now decompresses via system zlib -> -lz; host always
  # provides it, so appending to the shared link line covers apklabel/cli/
  # webui_http/webui_authz and any indirect referrer without touching recipes)
  if ! g++ $TFLAGS $objs "$OUT/test_${name}.o" -o "$bin" -lz 2>"$OUT/${name}.link.err"; then
    echo "  [FAIL] link:"; head -30 "$OUT/${name}.link.err"; return 1; fi
  if [ "${SKIP_RUN:-0}" = "1" ]; then echo "  (compiled, SKIP_RUN=1)"; return 0; fi
  echo "  RUN:"
  # RUN_CMD (optional, set by caller) overrides how the binary is executed —
  # used by thermal, which must run inside a mount namespace that bind-mocks
  # /sys/class/thermal + /sys/class/power_supply. $BIN is exported into it.
  if [ -n "${RUN_CMD:-}" ]; then
    BIN="$bin" bash -c "$RUN_CMD" && return 0 || { echo "  [FAIL] exit=$?"; return 1; }
  fi
  "$bin" && return 0 || { echo "  [FAIL] exit=$?"; return 1; }
}

# thermal needs a fake /sys bind-mounted in a mount namespace (it writes
# /sys/class/thermal/thermal_zone0/temp directly). Run the binary once per
# AATPE_THERMAL_SCENARIO under unshare.
run_thermal_namespaced() {
  local bin="$BIN"
  local mock; mock=$(mktemp -d /tmp/aatpe_thermal.XXXXXX)
  mkdir -p "$mock/thermal/thermal_zone0" "$mock/thermal/thermal_zone1" \
           "$mock/thermal/thermal_zone2" "$mock/power_supply/battery"
  # thermal.cpp identifies the cpu/skin/battery zones by their `type` file.
  echo "mtktscpu" > "$mock/thermal/thermal_zone0/type"
  echo "skin" > "$mock/thermal/thermal_zone1/type"
  echo "battery" > "$mock/thermal/thermal_zone2/type"
  local rc=0
  # v4.1.1 hotfix acceptance scenarios (boot0/mcjitter/clampregress/fallbackonly/
  # secondary/throttle0) build their OWN fake sys tree via thermalSetTestRoot, so
  # the bind-mock seeding below is inert for them; they run as fresh processes.
  for sc in normal emergency bootgrace battclamp freshness degrdecouple hwmontempn pmicnotcpu thread zonestick constclamp constclamp_throttled constclamp_cooling boot0 mcjitter clampregress fallbackonly secondary throttle0; do
    # readThermal() auto-detects units by magnitude (thermal.cpp v66 P2#14):
    #   tRaw>1500 -> milli-C (/1000); tRaw>150 -> deci-C (/10); else degC.
    # Seed each zone to a cool baseline, then override per-scenario with the
    # RAW value that test_thermal.cpp's expected degC maps back to:
    #   cpu zone0="cpu-thermal", skin zone1="skin", battery zone2="battery".
    echo 30000 > "$mock/thermal/thermal_zone0/temp"
    echo 30000 > "$mock/thermal/thermal_zone1/temp"
    echo 30000 > "$mock/thermal/thermal_zone2/temp"
    echo 300   > "$mock/power_supply/battery/temp"
    case "$sc" in
      normal)    # expect cpu=55 (55000mC), skin=42 (420 deciC), batt=35 (350/10)
        echo 55000 > "$mock/thermal/thermal_zone0/temp"
        echo 420   > "$mock/thermal/thermal_zone1/temp"
        echo 350   > "$mock/power_supply/battery/temp" ;;
      emergency) # expect cpu>=95 -> 100000mC = 100C (3-read debounce in test)
        echo 100000 > "$mock/thermal/thermal_zone0/temp" ;;
      bootgrace) echo 100000 > "$mock/thermal/thermal_zone0/temp" ;;
      battclamp) # battery raw 900000mC must clamp to 100C
        echo 900000 > "$mock/power_supply/battery/temp" ;;
    esac
    echo "  --- scenario: $sc ---"
    # tmpfs over /sys/class so we control thermal + power_supply entirely
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

# v4.3.8 F2.1/F2.2/F2.3: battery health scenarios need a fake
# /sys/class/power_supply/battery/ with health/cycle_count/charge_full/
# charge_full_design nodes. Bind-mock it in a private mount namespace and run
# the binary once per AATPE_BATTERY_SCENARIO.
run_battery_namespaced() {
  local bin="$BIN"
  local mock; mock=$(mktemp -d /tmp/aatpe_battery.XXXXXX)
  mkdir -p "$mock/power_supply/battery"
  local rc=0
  for sc in power empty good_new aged_soh65 aged_soh40 aged_soh20 \
            overheat dead cycle_only_700 cycle_only_1k no_design; do
    # Reset the battery dir per scenario (scenarios omit some nodes).
    rm -rf "$mock/power_supply/battery"
    mkdir -p "$mock/power_supply/battery"
    # Default: nothing seeded. Per-scenario case adds nodes.
    case "$sc" in
      power)        # pure math; no sysfs needed (readBatteryPower sees missing nodes)
        ;;
      empty)        # no nodes at all
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

# v4.2 A2: the schedtune bridge writes /dev/stune/top-app/schedtune.boost. Run the
# binary in a private mount namespace where /dev is a tmpfs holding a writable
# stune node, so the bridge's real ::access probe adopts it and we can read the
# boost level back (max() aggregation across touch/gpu/placement producers).
run_a2_boost_namespaced() {
  local bin="$BIN"
  unshare --map-root-user --mount bash -c "
    mount -t tmpfs tmpfs /dev &&
    mkdir -p /dev/stune/top-app &&
    echo 0 > /dev/stune/top-app/schedtune.boost &&
    exec '$bin'
  "
}

# v4.2.9 T1/T2: ui-snapshot reads the compile-time kStateDir=/dev/aatpe/state and
# gates on getuid()==0. Run the cli test in a private mount namespace where /dev is
# a fresh tmpfs, so we are "root" and the test can seed /dev/aatpe/state itself.
run_cli_namespaced() {
  local bin="$BIN"
  unshare --map-root-user --mount bash -c "
    mount -t tmpfs tmpfs /dev &&
    mkdir -p /dev/aatpe/state &&
    exec '$bin'
  "
}

# ---- recipes: prod deps + stub deps -----------------------------------------
# observability: aatpe_observability.cpp (independent)
# uevent:        uevent_monitor.cpp
# event_monitor:event_monitor.cpp + uevent_monitor.cpp
# interfaces:   aatpe_interfaces.cpp + aatpe_ml.cpp
# webui_json:   webui_json.cpp (needs WEBUI_DIR/DECISION_DIR/writeFile stub)
# thermal:      thermal.cpp (needs writeFile + log stub)
# cli/probe/watchdog: #include the production .cpp directly (main renamed inside
#   the test), so no separate production .cpp TU is linked.
run_one() {
  local t="$1"
  case "$t" in
    observability) build_test observability "aatpe_observability" "log_stub" ;;
    uevent)        build_test uevent        "uevent_monitor"     "log_stub" ;;
    event_monitor) build_test event_monitor "event_monitor uevent_monitor aatpe_ml" "log_stub v94_daemon_deps" ;;
    interfaces)    build_test interfaces   "aatpe_interfaces aatpe_ml" "v94_daemon_deps log_stub" ;;
    webui_json)    build_test webui_json    "webui_json aatpe_ml"   "webui_clusters_deps v94_daemon_deps log_stub scenewire" ;;
    thermal)       export -f run_thermal_namespaced
                   RUN_CMD='run_thermal_namespaced' build_test thermal "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff" "thermal_deps log_stub" ;;
    # cli #includes aatpe-cli.cpp (main renamed) which self-provides its own
    # readFile/readInt/aatpeExec/g_cpuCount helpers — so NO v94_daemon_deps stub
    # (would multiply-define them). Only aatpe_ml.cpp is needed (LearningModel).
    # v4.2.9 T1/T2: ui-snapshot needs getuid()==0 + a writable /dev/aatpe/state,
    # so run inside the tmpfs-/dev mount namespace (run_cli_namespaced).
    cli)           export -f run_cli_namespaced
                   RUN_CMD='run_cli_namespaced' build_test cli "aatpe_ml aatped_apklabel" "log_stub" ;;
    probe)         build_test probe        ""                   "" ;;
    watchdog)      build_test watchdog     ""                   "" ;;
    # v600-fix2: header-only inline shouldRescoreThread() active/dirty gating.
    # No prod/stub deps -- test only calls inline helpers in aatped_utils.h.
    scoring_active) build_test scoring_active ""                   "" ;;
    # v4.1.1 hotfix: boot-grace / degraded self-heal / pidfile adoption.
    # Same include-the-.cpp pattern as watchdog above (main renamed inside test).
    watchdog_hotfix) build_test watchdog_hotfix ""                "" ;;
    # ---- v4.1.1 F1/F3/H5 merge: 7 new focused hotfix targets -----------------
    # F1 #1/#2: whole-TU include aatped_priority.cpp; stubs live in the test TU.
    rt_restore)        build_test rt_restore        ""                   "" "-DAATPE_HOST_TEST" ;;
    # v4.4.8 MAJOR-2: thermal soft-derate must re-write the floor DOWN on an
    # already-boosted thread. whole-TU include aatped_priority.cpp; stubs in TU.
    priority_thermal_derate) build_test priority_thermal_derate ""                  "" "-DAATPE_HOST_TEST" ;;
    # F1 #8/#9: links the REAL hardened aatpeExecArgv from aatped_utils.o.
    exec_timeout)      build_test exec_timeout     "aatped_utils"       "" "-DAATPE_HOST_TEST" ;;
    # F1 #19: thermal.o evidence predicates against a fake two-cluster sysfs.
    thermal_clusters)  build_test thermal_clusters "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff"    "thermal_deps log_stub" "-DAATPE_HOST_TEST" ;;
    # P3-1: thermal emergency recovery honors an in-latch user mode override.
    emergency_recovery) build_test emergency_recovery "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff" "thermal_deps log_stub" "-DAATPE_HOST_TEST" ;;
    # F1 #2: whole-TU include cold_start_rules.cpp; stubs live in the test TU.
    coldstart_release) build_test coldstart_release ""                   "" "-DAATPE_HOST_TEST" ;;
    # F3.1 (v4.3.8): whole-TU include cold_start_rules.cpp; drives the
    # coldStartSetProcIdentityForTest() seam for cold/warm/restart/debounce/rate.
    coldinstance)     build_test coldinstance     ""                   "" "-DAATPE_HOST_TEST" ;;
    # F3 #16: gpu_boost devfreq off-path write validation + readback + retry.
    gpu_devfreq_recovery) build_test gpu_devfreq_recovery "gpu_boost"    "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    # F3 #24: parseCpuOnlineMask (self-contained parser copy in the test TU).
    ebpf_cpu_mask)     build_test ebpf_cpu_mask    ""                   "" ;;
    # F3 #12: poolFeatureForTier normalization (header-only inline in aatpe_ml_hybrid.h).
    ml_pool_feature)   build_test ml_pool_feature  ""                   "" ;;
    # ---- v4.1.2 hotfix backflow: 4 isolation tests wired (pure wiring, no
    #      production-semantic change; stubs follow the existing recipes). ----
    # X1 T2: D1100 sparse-tier numPools == distinct-cluster count. Pure header-only
    #   poolFeatureForTier() inline in aatpe_ml_hybrid.h (same shape as ml_pool_feature).
    numpools_d1100)    build_test numpools_d1100   ""                   "" ;;
    # X1 T1: per-outcome thermal gate. Calls the real static
    #   LearningModel::measureOutcomeV2 + the two thermal atomics, both defined in
    #   aatpe_ml.o. Reuses the SHARED aatpe_ml.o (aatpe_ml.cpp gates nothing on
    #   AATPE_HOST_TEST, so NO force-recompile that would overwrite it) and the same
    #   thermal_deps/log_stub set the isolated build proved (aatpe_ml.o's only
    #   non-libc external dep is aatpeExec, which thermal_deps provides).
    thermal_gate)      build_test thermal_gate     "aatpe_ml"          "thermal_deps log_stub" ;;
    # X1 T3 + watchdog latch: pure inline decision helpers in aatpe-constants.h
    #   (aatpe::watchdog::highLoadShouldDegrade / degradedSelfHeal). Header-only.
    degraded_gate)     build_test degraded_gate    ""                   "" ;;
    # X2: links the real aatpe_diag.o under root+SELinux-enforcing fake sysfs. The
    #   test TU defines its own logMsg/atomicWriteFile stubs inline, so NO log_stub
    #   (would multiply-define logMsg). aatpe_diag.o's only non-libc external symbol
    #   is atomicWriteFile, supplied by the test. Writes a relative ./v412_testroot.
    diag_gpu)          build_test diag_gpu         "aatpe_diag"        "" ;;
    # ---- v4.2 Batch A merge: 4 new hermetic targets -------------------------
    # A1: SurfaceFlinger/compositor whitelist truth table. Links the real
    # aatped_threadutil.cpp (self-contained, same as the threadutil recipe).
    a1_sf)            build_test a1_sf            "aatped_threadutil" "" ;;
    # A2: the "placement" producer joins the schedtune single-writer bridge and
    # participates in max(touch,gpu,placement). Run under a mount namespace with a
    # writable fake /dev/stune node so the bridge adopts it and we read it back.
    a2_boost)         export -f run_a2_boost_namespaced
                      RUN_CMD='run_a2_boost_namespaced' build_test a2_boost "gpu_boost" "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    # A3: autocalBigRatio() read/corrupt/fingerprint-miss/garbage/cache. The
    # autocal test seams are gated behind AATPE_HOST_TEST, so aatpe_caps.o is
    # force-recompiled with it. autocal_deps = the batch-A link stub (STATE_DIR +
    # file IO + topology hash) for the narrow autocal path.
    a3_autocal)       build_test a3_autocal       "aatpe_caps" "autocal_deps log_stub" "-DAATPE_HOST_TEST" ;;
    # A1: uclampMinForTier mode=3 (SF floor evaluated before the PRIME early
    # return) on the full placement graph. Same link set as `placement` (plus
    # aatped_threadutil, which placement.cpp now calls for the SF whitelist); the
    # aatpeBoostSet/Clear no-op bridge stubs live in placement_deps.cpp.
    a1_placement)     build_test a1_placement "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # ---- v4.2 Batch D merge: 3 hermetic targets (D1 cgroup .rec / D2 bpftool
    #      argv hardening / D3 hotplug online-mask decision). Link sets mirror the
    #      proven isolated postfreeze/v42_hotfix/build_d.sh recipes exactly; the
    #      test TUs self-provide RUN_DIR/STATE_DIR/logMsg/caps-writer stubs.
    # D1: recovery .rec cgroup field + promoted_tids backfill + old-.rec compat.
    #     Same link set as the `recovery` recipe (aatpe_recovery only; the test TU
    #     supplies RUN_DIR/STATE_DIR + writeFileAtomic + the caps cgroup writers).
    d1recovery)       build_test d1recovery "aatpe_recovery" "" ;;
    # H14: cgroup membership .rec format round-trip.
    h14_cgroup_rec)  build_test h14_cgroup_rec "aatpe_recovery" "" ;;
    # H14 e2e: mech parser (v1/v2/bad) + recovery API round-trip.
    h14_cgroup_e2e)  build_test h14_cgroup_e2e "aatpe_placement_mech aatpe_recovery" "log_stub" ;;
    # D2: aatpeExecArgv no-shell + exit-status hardening. Links the REAL
    #     aatped_utils.o; utils_deps/log_stub/sprop_stub satisfy its host deps
    #     (no AATPE_HOST_TEST — matches build_d.sh).
    d2exec)           build_test d2exec "aatped_utils" "utils_deps log_stub sprop_stub" ;;
    # D3: eBPF hotplug online-mask -> ring-rebuild DECISION core (parseCpuOnlineMask
    #     + EbpfScheduler::onlineMaskSetChanged). Links aatpe_ebpf + subsystem +
    #     aatped_utils (exec/mkdir now referenced by eBPF) + integrity; stubs mirror
    #     build_d.sh d3.
    d3hotplug)        build_test d3hotplug "aatpe_ebpf aatpe_subsystem aatped_utils integrity" "ebpf_deps log_stub hybrid_deps utils_deps sprop_stub" ;;
    caps)          build_test caps         "aatpe_caps" "log_stub" ;;
    diag)          build_test diag         "aatpe_diag"         "log_stub" ;;
    ext_neg_psi)   build_test ext_neg_psi  "aatpe_diag"         "log_stub" ;;
    mech_uclamp)   build_test mech_uclamp  "aatpe_placement_mech aatpe_caps"  "log_stub" ;;
    coldstart)     build_test coldstart    "cold_start_rules aatpe_recovery aatpe_ml"   "placement_deps ml_deps recovery_caps_stub" "-DAATPE_HOST_TEST" ;;
    thermal_hal)   build_test thermal_hal  "thermal thermal_headroom aatpe_ml thermal_ff/thermal_ff"            "v94_daemon_deps log_stub" ;;
    gpu_boost)     build_test gpu_boost    "gpu_boost"          "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    gpu_boost_thermal) build_test gpu_boost_thermal "gpu_boost" "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    ebpf_classify) build_test ebpf_classify "aatpe_ebpf aatpe_subsystem integrity" "ebpf_deps log_stub" ;;
    ebpf_event_parse) build_test ebpf_event_parse "aatpe_ebpf aatpe_subsystem" "ebpf_deps log_stub" "-DAATPE_HOST_TEST" ;;
    ebpf_failsafe) build_test ebpf_failsafe "aatpe_ebpf aatpe_subsystem" "log_stub ebpf_deps hybrid_deps" "-DAATPE_HOST_TEST" ;;
    ebpf_elf_robust_v449) build_test ebpf_elf_robust_v449 "aatpe_ebpf aatpe_subsystem" "log_stub ebpf_deps hybrid_deps" "-DAATPE_HOST_TEST" ;;
    ebpf_p3_fixes) build_test ebpf_p3_fixes "aatpe_ebpf aatpe_subsystem" "ebpf_deps log_stub" "-DAATPE_HOST_TEST" ;;
    crypto_v449)          build_test crypto_v449          "" "" ;;
    session_v449)         build_test session_v449         "aatpe_session_token" "log_stub" ;;
    uclamp_restart_v449) build_test uclamp_restart_v449 "" "" "-DAATPE_HOST_TEST" ;;
    thermal_hardwall_v449) build_test thermal_hardwall_v449 "aatped_arbitrator mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "" "-UAATPED_ARBITRATOR_DAEMON" ;;
    vmguest_race_v449)   build_test vmguest_race_v449   "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # v4.4.9 FX2 thread-scene regression tests (freezer_scene is re-run under TSan in the TSan gate)
    freezer_scene_v449)   build_test freezer_scene_v449   "c4_freezer" "" "-UAATPE_HOST_TEST" ;;
    preboost_scene_v449)  build_test preboost_scene_v449  "pre_boost_predict" "" "-UAATPE_HOST_TEST -O2" ;;
    loadforecast_scene_v449) build_test loadforecast_scene_v449 "mech/src/load_forecast" "" "" ;;
    uevent_scene_v449)    build_test uevent_scene_v449    "uevent_monitor" "log_stub" "" ;;
    caps_pure)     build_test caps_pure     "aatpe_caps" "autocal_deps log_stub" ;;
    command_dispatcher) build_test command_dispatcher "aatpe_command_dispatcher" "log_stub" ;;
    c2_envelope)   build_test c2_envelope  "c2_envelope scene_infer" "" ;;
    # spsc_queue: header-only template in aatpe_ebpf.h, no prod/stub deps.
    spsc_queue)    build_test spsc_queue   ""                   "" ;;
    # ml: aatpe_ml.cpp is self-contained except for aatpeExec (see ml_deps.cpp).
    ml)            build_test ml           "aatpe_ml"           "ml_deps" ;;
    # scoring: scoring.cpp + aatpe_ml.cpp; scoring_deps.cpp provides the
    #   daemon globals scoring.cpp reads (pools, cpuMetrics, cfg, history...).
    #   Do NOT also link v94_daemon_deps (multiple definitions).
    scoring)       build_test scoring      "scoring adrc aatpe_ml"    "scoring_deps ml_deps log_stub" ;;
    # H1 coverage gate: pure scoring helpers not reached by test_scoring.cpp
    scoring_pure)  build_test scoring_pure  "scoring adrc aatpe_ml"    "scoring_deps ml_deps log_stub" ;;
    # placement: placement.cpp + aatpe_ml + the 5 aatpe_ml_hybrid_* pieces;
    #   placement_deps.cpp provides the ~50 daemon globals placement.cpp reads.
    placement)     build_test placement    "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # v4.4.8 MAJOR-3: g_bigPool locked-snapshot race. Same link set as placement;
    # drives the guarded rewrite/fallback seams concurrently (C2 re-runs under TSan).
    bigpool_race)  build_test bigpool_race "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # v4.4.8 A2: atomic globals (g_audioIsolatedCpu, g_gpuType) store/load round-trip.
    a2_atomics)    build_test a2_atomics "" "" "-DAATPE_HOST_TEST" ;;
    # B-001: spillover RT promote/demote/release symmetry. Same link set as
    # `placement` but OMITS aatped_priority.o — the test TU supplies recording
    # stubs for aatpeSetThreadPriority/aatpeRestoreThreadPriority so we can assert
    # the demote loop AND releaseAllSpilloverLockedImpl both call the restore channel.
    spillover_rt)  build_test spillover_rt "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # H13: hotplug topology rebuild hardening (reuses placement.o + deps).
    h13_hotplug)   build_test h13_hotplug  "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # v4.2.7: VM container guest-game resolution (placement-side).
    vm_guest)      build_test vm_guest     "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # P2: per-module independent unit tests (100% module coverage).
    # abtest: self-contained stats TU.
    abtest)        build_test abtest        "aatped_abtest"        "log_stub" ;;
    gamescene)     build_test gamescene     "aatped_gamescene"     "gamescene_deps log_stub" ;;
    # v4.3.7: arbitration layer. Pure part of aatped_arbitrator + 4 mech cores.
    # -U forces a pure recompile (the `main` target pre-builds the DAEMON variant
    #   into the shared cache; this recipe must NOT reuse that object).
    arbitrator)    build_test arbitrator    "aatped_arbitrator mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "" "-UAATPED_ARBITRATOR_DAEMON" ;;
    # v4.3.9: multi-source arbitration wiring regressions (audit4 F1/F4/F10/F-1).
    arb_wiring)    build_test arb_wiring    "aatped_arbitrator mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "" "-UAATPED_ARBITRATOR_DAEMON" ;;
    # v4.3.8 F2.1/F2.2/F2.3: battery health scenarios bind-mock a fake
    # /sys/class/power_supply/battery per scenario (run_battery_namespaced).
    battery)       export -f run_battery_namespaced
                   RUN_CMD='run_battery_namespaced' build_test battery "aatped_battery" "battery_deps" ;;
    gpu)           build_test gpu          "aatped_gpu aatpe_ml" "gpu_deps ml_deps log_stub" ;;
    audio)         build_test audio        "aatped_audio"         "audio_deps log_stub" ;;
    applaunch)     build_test applaunch    "aatped_applaunch"     "applaunch_deps log_stub" ;;
    logging)       build_test logging      "aatped_logging"       "logging_deps" ;;
    log_orphan)    build_test log_orphan   "aatped_logging"       "logging_deps" "-DAATPE_HOST_TEST" ;;
    utils)         build_test utils        "aatped_utils aatpe_helper_audit"         "utils_deps log_stub sprop_stub" ;;
    # uevent_fallback: daemon-side sysfs polling; reuses event_monitor +
    # uevent_monitor + aatpe_ml graph, v94_daemon_deps supplies pools/counters.
    uevent_fallback) build_test uevent_fallback "aatped_uevent event_monitor uevent_monitor aatpe_ml" "v94_daemon_deps log_stub uevent_deps" ;;
    ebpf_agg)      build_test ebpf_agg     "aatped_ebpf_agg"      "ebpf_agg_deps log_stub" ;;
    # ML hybrid: one HybridLearner class split across 5 TUs; each test links all
    # 5 (they are indivisible at link time) but focuses on its own file.
    ml_hybrid_main)   build_test ml_hybrid_main   "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_train)  build_test ml_hybrid_train  "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_predict) build_test ml_hybrid_predict "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_persist) build_test ml_hybrid_persist "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    ml_hybrid_alpha)  build_test ml_hybrid_alpha  "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    # Federated: one FederatedLearner split across 4 TUs (crypto header-only).
    fed_main)      build_test fed_main      "aatpe_federated_main aatpe_federated_aggregate" "log_stub" ;;
    fed_aggregate) build_test fed_aggregate "aatpe_federated_main aatpe_federated_aggregate" "log_stub" ;;
    # ROUND-014 P3 Plan B: fed_exchange/fed_security removed (dormant wire path; no TU).
    hmac)         build_test hmac          "aatped_hmac"            "log_stub" "-DAATPE_HOST_TEST" ;;
    topology)     build_test topology      "aatped_topology aatped_gpu aatpe_ml"  "v94_daemon_deps log_stub topology_deps" ;;
    config)       build_test config       "aatped_config aatpe_ml aatped_hmac aatpe_config_schema aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub config_deps hybrid_deps ml_deps" ;;
    state)        build_test state        "aatped_state aatpe_ml"  "state_deps log_stub ml_deps" ;;
    control)      build_test control      "aatped_control aatpe_ml aatpe_command_dispatcher aatpe_command_queue aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    # round-4 P1 #1/#2: real command queue directory. Same link set as
    # control (drain dispatches through the real executeControlLine()).
    cmdq)         build_test cmdq         "aatpe_command_queue aatped_control aatpe_ml aatpe_command_dispatcher aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    thread_scan)  build_test thread_scan  "thread_scan aatpe_ml aatped_threadutil" "v94_daemon_deps log_stub thread_deps" ;;
    thread_scan_hydrate) build_test thread_scan_hydrate "thread_scan aatpe_ml aatped_threadutil" "v94_daemon_deps log_stub thread_deps" "-DAATPE_HOST_TEST" ;;
    threadutil)   build_test threadutil   "aatped_threadutil"             "" ;;
    frame)        build_test frame        "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"        "log_stub frame_deps" ;;
    fx3_frame_v449) build_test fx3_frame_v449 "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" "-DAATPE_HOST_TEST" ;;
    # v4.2.1 hotfix backflow: game-layer recognition (sfListLayers/selectGameLayer/
    # sampleGameLayer/parseGameOverridesContent/parsePmPackageList). Same prod/stub link
    # set as the `frame` recipe (real frame.cpp TU + frame_deps/log_stub); pure
    # aatpe::frameaware functions, no daemon IO. 37 assertions.
    game_layer)   build_test game_layer   "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"        "log_stub frame_deps" ;;
    # F-01~F-06: 帧率/帧时测量口径回归（全局 latency 不短路游戏层、F-04 不回退面板
    # hz、F-06 GED 回退、非游戏标注）。Same prod/stub link set as frame.
    f01_fps)      build_test f01_fps      "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"        "log_stub frame_deps" ;;
    # v4.2.1 T3: per-app rolling frame stats accessor. Same link set as frame.
    per_app_frame) build_test per_app_frame "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel"    "log_stub frame_deps" ;;
    # v4.3.2 R5/R6: LRU eviction also clears g_perAppFsMetrics + monotonic frameNowMs().
    per_app_frame_r5r6) build_test per_app_frame_r5r6 "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    # v6.0.0 fix2: FRAME-01/02/03 P2 fixes (g_tsLayerSnap eviction, game_* clear,
    # updatedMs clock unification). Same link set as per_app_frame.
    frame_p2_fixes) build_test frame_p2_fixes "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    # rel12 Bug1/Bug2: dropped<=total_frames (raw vsync kept separately as
    # missed_vsyncs) + active_ms reflects real framestats span for a single fresh window.
    frame_rel12) build_test frame_rel12 "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    # v4.4.8 MAJOR-1/M4: getPkgUid packages.list parser regression. Links the
    # real frame.o and drives it through the guarded aatpeTestGetPkgUid seam
    # (replaces the old #include-frame.cpp fuzz-harness hack). 8 cases.
    pkglist_parsing) build_test pkglist_parsing "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    webui_http)   build_test webui_http   "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel"   "log_stub webui_deps gamelist_stub" ;;
    # v4.4.9 FX4b web/control/script hardening regression tests
    webui_http_v449) build_test webui_http_v449 "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel" "log_stub webui_deps gamelist_stub" ;;
    cmdq_v449)       build_test cmdq_v449       "aatpe_command_queue aatped_control aatpe_ml aatpe_command_dispatcher aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    cmdq_replaysafe) build_test cmdq_replaysafe "aatpe_command_queue aatped_control aatpe_ml aatpe_command_dispatcher aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "v94_daemon_deps log_stub control_deps hybrid_deps gamelist_stub" ;;
    interfaces_v449) build_test interfaces_v449 "aatpe_interfaces aatpe_ml" "v94_daemon_deps log_stub" ;;
    logging_v449)    build_test logging_v449    "aatped_logging" "logging_deps" ;;
    # v4.2.9 F-01: webui authorization regression (stok=SSE-only, write=gated).
    # Identical link set to webui_http; pins the F-01 hardening.
    webui_authz)  build_test webui_authz "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel"   "log_stub webui_deps gamelist_stub" ;;
    ebpf)         build_test ebpf         "aatpe_ebpf aatpe_subsystem"            "log_stub ebpf_deps hybrid_deps" ;;
    # round-2: ML drift detector + topology sidecar (#38/#39/#93).
    v343_ml)      build_test v343_ml      "aatpe_ml"             "ml_deps log_stub" ;;
    # v4.5.0: ML shadow sampling in observe-only (passive) mode.
    ml_shadow_v450) build_test ml_shadow_v450 "aatpe_ml"          "ml_deps log_stub" ;;
    # v6.0.0 P1: passive-sampler feature extraction contract (extractMlFeatures
    # + isAllZero gate + shadow ring round-trip). Same link set as `scoring`.
    ml_shadow_passive_v600) build_test ml_shadow_passive_v600 "scoring adrc aatpe_ml" "scoring_deps ml_deps log_stub" ;;
    # v6.0.0 RT-A A5: g_cpuMetrics concurrent writer/reader locking contract.
    # scoring_deps provides the real g_cpuMetrics + g_cpuMetricsMutex globals.
    cpu_metrics_race_v600) build_test cpu_metrics_race_v600 "" "scoring_deps log_stub" ;;
    # v6.0.0 RTC-01: concurrent backupModelFile (mkstemp tmp-name) race / torn-bak.
    backup_model_race_v600) build_test backup_model_race_v600 "" "" ;;
    # v4.5.0 F-03: wakeup_cache LRU_HASH in-kernel reaper + HASH fallback decision.
    wakeup_reaper_v450) build_test wakeup_reaper_v450 "aatpe_ebpf aatpe_subsystem" "log_stub ebpf_deps hybrid_deps" "-DAATPE_HOST_TEST" ;;
    # v4.3.4 F-group regression: F-001 stale-decay global alpha + F-003 v1 latch clear.
    f_v434_ml)    build_test f_v434_ml    "aatpe_ml"             "ml_deps log_stub" ;;
    # v4.3.4 F-group regression: F-002 hybrid tick thermal pause gate.
    f_v434_hybrid) build_test f_v434_hybrid "aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha" "log_stub hybrid_deps" ;;
    # round-2: placement hysteresis gate (#34/#82/#91/#92/#93).
    # Same link set as the placement recipe (placement_deps supplies globals).
    v343_placement) build_test v343_placement "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_diag aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" ;;
    # round-4: #61 session token, #89 fault-injection graceful degradation,
    # #90 100x restart stability simulation. Links the self-contained session store.
    v342_round4)  build_test v342_round4  "aatpe_session_token"    "log_stub" ;;
    # round-5 (final): unified subsystem lifecycle/health, StopToken,
    # safe-mode, self-observation, FD/thread-leak + BPF failure-injection tests.
    # Links the self-contained aatpe_subsystem.cpp; no daemon deps.
    v342_round5)  build_test v342_round5  "aatpe_subsystem"   "log_stub" ;;
    subsystem)    build_test subsystem    "aatpe_subsystem"   "log_stub" "-DAATPE_HOST_TEST" ;;
    # release-convergence item 22: Profile arbitration generation-guard stress.
    # Header-only manager + own loadProfile() stub; no production objects needed.
    profile_arb)  build_test profile_arb  ""   "" ;;
    # round-3 (#27): profile dedup + whitelist + high-concurrency storm.
    # Header-only manager with own loadProfile() stub; no production objects.
    profile_stress) build_test profile_stress ""   "" ;;
    # round-3 (#25): unified recovery module restart/orig-guard. Links
    # aatpe_recovery.cpp; the test supplies RUN_DIR + logMsg + writeFileAtomic stubs.
    recovery)     build_test recovery      "aatpe_recovery"   "" ;;
    # main: aatped_main.cpp defines main(); strip that symbol after compiling so
    # test_main.cpp's own main wins, then link with v94_daemon_deps.
    main)         prod_obj aatped_main && objcopy --strip-symbol=main "$OUT/aatped_main.o"
                  g++ $PFLAGS $INCLUDES -DAATPED_ARBITRATOR_DAEMON -c "$SRC/aatped_arbitrator.cpp" -o "$OUT/aatped_arbitrator.o"
                  build_test main          "aatped_main aatped_battery aatpe_ml aatpe_ebpf aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatpe_federated_main aatpe_federated_aggregate aatped_ebpf_agg event_monitor uevent_monitor frame_binder adrc integrity aatpe_state_ttl aatpe_helper_audit aatpe_managers aatpe_config_schema aatpe_command_dispatcher aatpe_session_token aatped_priority aatpe_recovery aatpe_command_queue aatpe_subsystem cold_start_rules gpu_boost input_monitor aatped_psi psi_decision aatpe_placement_mech touch_uclamp touch_uclamp_act aatped_arbitrator pre_boost_predict thermal_headroom mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context" "log_stub main_deps hybrid_deps ebpf_deps recovery_caps_stub touch_host_stub scenewire" ;;
    # B-005: focused module unit tests (integrity / adrc / frame_binder / config_schema).
    # integrity: self-contained SHA-256 + chained manifest; needs STATE_DIR/writeFile stubs.
    integrity)    build_test integrity     "integrity"                       "integrity_deps log_stub" ;;
    # adrc: pure ADRC controller; only LOG_INFO -> logMsg.
    adrc)         build_test adrc          "adrc"                            "log_stub" ;;
    # frame_binder: test #includes frame_binder.cpp to reach the file-static
    # parseLatencyOutput() (probe/watchdog pattern). Do NOT also list frame_binder
    # as a prod object (would multiply-define the static globals).
    frame_binder) build_test frame_binder ""                                "frame_deps log_stub" ;;
    # GPUTOP P2 fixes: test #includes frame_binder.cpp for readPipeBlocking cap;
    # links gpu_boost.o (with -DAATPE_GPU_BOOST_TEST) for devfreq mutex smoke.
    gputop_p2_fixes) build_test gputop_p2_fixes "gpu_boost" "gpu_deps log_stub" "-DAATPE_GPU_BOOST_TEST" ;;
    # config_schema: self-contained field table + validation/clamping.
    config_schema) build_test config_schema "aatpe_config_schema"              "log_stub" ;;
    # apklabel: resolve an installed APK's real <application android:label> out of
    # its own binary resources (AXML + resources.arsc), hostile-input safe. Test
    # TUs embed 14 fixtures and use mkdtemp; no /data reads, no stubs needed.
    apklabel)      build_test apklabel      "aatped_apklabel"                 "" ;;
    # v4.3.2 R7: multi-user / work-profile package enumeration (packages.list + /data/app scan).
    apklabel_multi) build_test apklabel_multi "aatped_apklabel"               "" ;;
    # v4.3.1h5 D1: self-deadlock regression. Pure std::mutex/thread model proving
    # the decision loop never blocks on a non-recursive spillover-entry lock.
    spillover_deadlock) build_test spillover_deadlock ""                        "" ;;
    # h6 D2: PID identity exe-match decision (published daemon_exe vs bootstrap set).
    pid_identity_exe) build_test pid_identity_exe ""                           "" ;;
    # D06 real-device gfxinfo comma-separated framestats regression.
    framestats_d06) build_test framestats_d06 "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    # B: D3/D4/C2/R8 frame isolation (21 cases). Same link set as per_app_frame.
    aatpe_iso)      build_test aatpe_iso      "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel" "log_stub frame_deps" ;;
    # v4.4.1 P0: windowing-mode parse + epoch age invariant + all-users enum.
    # Same link set as aatpe_iso (real frame.o exports aatpeParseWindowingMode;
    # aatped_apklabel.o exports listInstalledPackagesAllUsers).
    p0_v441)        build_test p0_v441        "frame frame_binder aatpe_ml aatped_threadutil aatped_apklabel placement_heal_decision" "log_stub frame_deps" ;;
    # C: CORS triple-predicate / OPTIONS preflight / narrow CSRF (webui_http).
    webui_cors)     build_test webui_cors     "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel" "log_stub webui_deps gamelist_stub" ;;
    # HTTP-01: delta baseline LRU eviction regression.
    webui_delta_lru) build_test webui_delta_lru "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel" "log_stub webui_deps gamelist_stub" ;;
    # 2026-10-26 webui_http P3 audit: HTTP-05/06/07 (body-vs-header-block
    # connection scan, Origin trailing-whitespace trim, unsigned /control 403-vs-405).
    webui_p3_http) build_test webui_p3_http "webui_http aatpe_ml aatpe_session_token aatpe_command_queue aatpe_config_schema integrity aatped_apklabel" "log_stub webui_deps gamelist_stub" ;;
    # D: C1 PSI/rq-wait independent cadence, C3/C4 bounded render candidates.
    # Test provides its own strong overrides for the diag pressure accessors, so
    # aatpe_diag.o is intentionally EXCLUDED (would otherwise multiple-define).
    psi_fallback)   build_test psi_fallback   "placement placement_heal_decision aatped_psi psi_decision aatpe_placement_mech aatpe_caps adrc aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_priority aatpe_state_ttl aatpe_recovery aatped_threadutil" "placement_deps ml_deps gamelist_stub" "-DAATPE_HOST_TEST" ;;
    # E: shared cpu_zone_selector.h pure logic (14) + hermetic /sys fixtures.
    thermzone)          build_test thermzone          ""                              "" ;;
    thermzone_fixtures) build_test thermzone_fixtures ""                              "" ;;
    # D-002: unified ttlHours<=0 semantics. Links the real aatpe_state_ttl.o with
    # a minimal stub supplying only its extern globals (g_history/g_threads/...).
    state_ttl_d)       build_test state_ttl_d       "aatpe_state_ttl"              "ttl_deps" ;;
    # D-006: emergIndNeedSamples ceiling-division arithmetic guard (pure, no prod).
    d_arithmetic)     build_test d_arithmetic     ""                              "" ;;
    # C1/C3 exploration tests (default-off features, pure logic host-testable).
    preboost)         build_test preboost         "pre_boost_predict"             "" "-DAATPE_HOST_TEST" ;;
    thermheadroom)    build_test thermheadroom    "thermal_headroom"             "" "-DAATPE_HOST_TEST" ;;
    # W5 scene_infer + C2 envelope (pure function, host-testable).
    scene_infer)      build_test scene_infer      "scene_infer c2_envelope"     "" "-DAATPE_HOST_TEST" ;;
    # W5 slow-thread wiring (pure mapping layer; production glue compiles out under host).
    scene_wiring)     build_test scene_wiring     "scene_infer c2_envelope scene_infer_wiring" "" "-DAATPE_HOST_TEST" ;;
    # v4.4.4 C4 freezer (pure decision layer; glue compiles out under host) + GPU classifier.
    tidy_hardening) build_test tidy_hardening "aatped_logging" "" "-DAATPE_HOST_TEST" ;;
    c4_freezer)       build_test c4_freezer       "c4_freezer"              "" "-DAATPE_HOST_TEST" ;;
    # v6.0.0 review: C4 freezer PRODUCTION glue (tick/StateJson). -U undefines the
    # PFLAGS AATPE_HOST_TEST so the real tick body is linked; -D enables the
    # lock-depth seam. Asserts STATE_DIR path resolution, lock granularity (slow
    # exec/file I/O never under s_stateMutex) and the NN restart-restore round-trip.
    c4_freezer_glue)  build_test c4_freezer_glue  "c4_freezer"              "" "-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM" ;;
    c4_freezer_v1)    build_test c4_freezer_v1    "c4_freezer"              "" "-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM" ;;
    # rel12 Bug6: v1 freezer present but NO game target -> StateJson must still
    # report freezerAvailable=true / freezerBackend=v1 (not "unavailable").
    c4_freezer_rel12) build_test c4_freezer_rel12 "c4_freezer"              "" "-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM" ;;
    *) echo "unknown test: $t (authoritative set: observability uevent event_monitor interfaces webui_json thermal cli probe watchdog; spsc_queue/ml/scoring/placement exist only as SKIPPED recipes)" ; return 1 ;;
  esac
}

# ---- main -------------------------------------------------------------------
# Authoritative v94 hermetic unit tests = the files that exist under
# tests/v94/ (kept in sync with tests/README.md section 1.1). scoring and
# placement recipes were previously SKIPPED (no test_*.cpp yet); adds
# test_scoring.cpp + test_placement.cpp so they join the authoritative set.
ALL="observability uevent event_monitor interfaces webui_json thermal cli probe watchdog scoring placement spillover_rt \
abtest gamescene arbitrator arb_wiring battery gpu audio applaunch logging utils uevent_fallback ebpf_agg \
ml_hybrid_main ml_hybrid_train ml_hybrid_predict ml_hybrid_persist ml_hybrid_alpha \
fed_main fed_aggregate hmac topology config state control \
thread_scan threadutil frame webui_http ebpf main v343_ml f_v434_ml f_v434_hybrid v343_placement v342_round4 profile_arb profile_stress recovery cmdq v342_round5 \
integrity adrc frame_binder config_schema caps diag mech_uclamp coldstart coldinstance thermal_hal gpu_boost gpu_boost_thermal ebpf_classify watchdog_hotfix rt_restore exec_timeout thermal_clusters coldstart_release gpu_devfreq_recovery ebpf_cpu_mask ml_pool_feature \
numpools_d1100 thermal_gate degraded_gate diag_gpu \
a1_sf a2_boost a3_autocal a1_placement d1recovery d2exec d3hotplug game_layer f01_fps apklabel apklabel_multi per_app_frame_r5r6 spillover_deadlock pid_identity_exe framestats_d06 aatpe_iso p0_v441 webui_cors psi_fallback thermzone thermzone_fixtures state_ttl_d d_arithmetic preboost thermheadroom scene_infer scene_wiring c4_freezer c4_freezer_glue c4_freezer_v1 tidy_hardening pkglist_parsing priority_thermal_derate bigpool_race a2_atomics log_orphan subsystem thread_scan_hydrate ebpf_event_parse caps_pure command_dispatcher c2_envelope ebpf_failsafe uclamp_restart_v449 thermal_hardwall_v449 emergency_recovery vmguest_race_v449 freezer_scene_v449 preboost_scene_v449 loadforecast_scene_v449 uevent_scene_v449 scoring_pure ebpf_elf_robust_v449 ebpf_p3_fixes crypto_v449 session_v449 fx3_frame_v449 webui_http_v449 cmdq_v449 cmdq_replaysafe interfaces_v449 logging_v449 ml_shadow_v450 wakeup_reaper_v450 ml_shadow_passive_v600 cpu_metrics_race_v600 backup_model_race_v600 gputop_p2_fixes webui_p3_http scoring_active"
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
