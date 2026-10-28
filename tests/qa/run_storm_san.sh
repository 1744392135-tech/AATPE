#!/bin/bash
# run_storm_san.sh — Run S25/S26 storm scenarios under ASan and TSan.
# Uses the exact same production/stub recipe as tests/integration/build.sh.
# C09: pipefail required so a failing/crashing sanitizer run (the first
# component of the `... | tail -30` pipeline) propagates its exit instead of
# tail's success.
set -eo pipefail
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
IT="$SRC/tests/integration"
BUILD="$IT/build_san"
mkdir -p "$BUILD"

CXX=g++
BASEFLAGS="-std=c++17 -O1 -w -fPIC -pthread -I$SRC -I$SRC/shared -I$IT/stubs -I$IT/src"

PROD="aatped_main aatped_utils aatped_logging aatped_config aatped_topology \
      aatped_state aatped_control aatped_hmac aatped_gamescene aatped_battery \
      aatped_gpu aatped_audio aatped_applaunch aatped_uevent aatped_abtest \
      aatped_ebpf_agg scoring event_monitor uevent_monitor thermal frame thread_scan \
      placement aatpe_placement_mech aatpe_diag webui_json webui_http aatpe_ml aatpe_ml_hybrid_main \
      aatpe_ml_hybrid_train aatpe_ml_hybrid_predict aatpe_ml_hybrid_persist \
      aatpe_ml_hy_alpha aatpe_federated_main aatpe_federated_aggregate \
      aatpe_ebpf \
      aatpe_observability aatpe_interfaces frame_binder adrc integrity aatpe_caps aatped_priority \
      aatpe_state_ttl aatpe_helper_audit aatpe_managers aatpe_config_schema aatpe_command_dispatcher aatpe_session_token aatpe_recovery aatpe_command_queue aatpe_subsystem \
      cold_start_rules gpu_boost input_monitor aatped_threadutil"

build_san() {
    local san="$1"
    local out="$BUILD/it_${san}"
    local sanflags="-fsanitize=$san -fno-omit-frame-pointer -g"
    local PRODFLAGS="$BASEFLAGS $sanflags -Dmain=aatped_shim_main -DAATPE_HOST_TEST -DAATPE_GPU_BOOST_TEST"

    echo "=== Building $san integration binary ==="
    for o in $PROD; do
        if [ ! -f "$BUILD/$o.$san.o" ] || [ "$SRC/$o.cpp" -nt "$BUILD/$o.$san.o" ]; then
            echo "  CC $o"
            $CXX $PRODFLAGS -c "$SRC/$o.cpp" -o "$BUILD/$o.$san.o" 2> "$BUILD/$o.$san.err" || {
                echo "FAILED: $o"; head -30 "$BUILD/$o.$san.err"; exit 1; }
        fi
    done

    echo "  CC stub_property"
    $CXX $BASEFLAGS $sanflags -c "$IT/stubs/stub_property.cpp" -o "$BUILD/stub_property.$san.o" 2> "$BUILD/stub.$san.err" || {
        echo "FAILED: stub_property"; head -20 "$BUILD/stub.$san.err"; exit 1; }

    for t in it_common it_scenarios it_main; do
        echo "  CC $t"
        $CXX $BASEFLAGS $sanflags -c "$IT/src/$t.cpp" -o "$BUILD/$t.$san.o" 2> "$BUILD/$t.$san.err" || {
            echo "FAILED: $t"; head -40 "$BUILD/$t.$san.err"; exit 1; }
    done

    echo "  LINK $out"
    OBJS=""
    for o in $PROD stub_property it_common it_scenarios it_main; do OBJS="$OBJS $BUILD/$o.$san.o"; done
    $CXX $BASEFLAGS $sanflags $OBJS -o "$out" 2> "$BUILD/link.$san.err" || {
        echo "LINK FAILED"; head -40 "$BUILD/link.$san.err"; exit 1; }
    echo "  Built: $out"
}

run_san() {
    local san="$1"
    local bin="$BUILD/it_${san}"
    echo ""
    echo "=== Running S25+S26 under $san ==="
    local rc=0
    # C09: `| tail -30` alone yields tail's (zero) exit even when the storm run
    # crashes; with pipefail the pipeline goes non-zero, but we still capture the
    # FIRST component's code explicitly so we can report it and exit non-zero.
    AATPE_ONLY="S25_ThreadStorm,S26_FgSwitchStorm" unshare --map-root-user --mount "$bin" 2>&1 | tail -30 || rc=${PIPESTATUS[0]}
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] $san storm run FAILED exit=$rc"
        return "$rc"
    fi
    echo "  [OK] $san storm run exit=0"
}

build_san address
build_san thread
run_san address
run_san thread

echo ""
echo "=== Storm sanitizer runs complete ==="
