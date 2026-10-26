#!/bin/bash
# run_stress_storm.sh — Run all three long-running stress tests with proper production TU linkage.
#
# 1. Config hot-reload storm (2000 iterations) — links aatped_config.cpp
# 2. Foreground switch storm (5000 iterations) — runs S26 from integration binary
# 3. Thread create/destroy storm (2000 iterations) — runs S25 from integration binary
#
# Reuses the verified PROD/stub recipe from run_storm_san.sh for thread/fg storms.
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QA_DIR="$SRC/tests/qa"
IT="$SRC/tests/integration"
BUILD="$QA_DIR/build/stress"
ART="$QA_DIR/artifacts"
mkdir -p "$BUILD" "$ART"

CXX=g++
BASEFLAGS="-std=c++17 -O1 -w -fPIC -pthread -I$SRC -I$SRC/shared -I$IT/stubs -I$IT/src -I$SRC/tests/stubs -I$SRC/mech/include"

echo "=============================================="
echo "AATPE Long-Running Stress Storm Suite"
echo "=============================================="

# 1. Config hot-reload storm — build with production config TU
echo ""
echo "=== [1/3] Config Hot-Reload Storm (2000 iterations) ==="
PROD_CONFIG="aatped_config aatpe_config_schema aatpe_ml aatpe_ml_hybrid_main aatpe_ml_hybrid_predict aatpe_ml_hybrid_train aatpe_ml_hybrid_persist aatpe_ml_hy_alpha aatped_hmac"
STUB_CONFIG="log_stub config_deps hybrid_deps ml_deps"

SANFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
PRODFLAGS="$BASEFLAGS $SANFLAGS -DAATPE_HOST_TEST"

echo "  Building (links aatped_config.cpp + deps)..."
OBJS=""
for p in $PROD_CONFIG; do
    obj="$BUILD/$p.o"
    if [ ! -f "$obj" ] || [ "$SRC/$p.cpp" -nt "$obj" ]; then
        echo "    CC $p"
        $CXX $PRODFLAGS -c "$SRC/$p.cpp" -o "$obj" 2> "$BUILD/$p.err" || {
            echo "FAILED: $p"; head -30 "$BUILD/$p.err"; exit 1; }
    fi
    OBJS="$OBJS $obj"
done
for s in $STUB_CONFIG; do
    obj="$BUILD/stub_$s.o"
    if [ ! -f "$obj" ] || [ "$IT/stubs/$s.cpp" -nt "$obj" ]; then
        # config stubs live in tests/stubs, not integration/stubs
        stubpath="$SRC/tests/stubs/$s.cpp"
        if [ ! -f "$stubpath" ]; then stubpath="$IT/stubs/$s.cpp"; fi
        echo "    CC stub_$s"
        $CXX $PRODFLAGS -c "$stubpath" -o "$obj" 2> "$BUILD/stub_$s.err" || {
            echo "FAILED: stub_$s"; head -20 "$BUILD/stub_$s.err"; exit 1; }
    fi
    OBJS="$OBJS $obj"
done

echo "    CC stress_reload_storm.cpp"
$CXX $BASEFLAGS $SANFLAGS -c "$QA_DIR/stress_reload_storm.cpp" -o "$BUILD/stress_reload_storm.o" 2> "$BUILD/reload.err" || {
    echo "FAILED: stress_reload_storm.cpp"; head -30 "$BUILD/reload.err"; exit 1; }

echo "    LINK stress_reload_storm"
$CXX $BASEFLAGS $SANFLAGS $OBJS "$BUILD/stress_reload_storm.o" -o "$BUILD/stress_reload_storm" 2> "$BUILD/reload.link.err" || {
    echo "LINK FAILED"; head -40 "$BUILD/reload.link.err"; exit 1; }

echo "  Running 2000 iterations..."
"$BUILD/stress_reload_storm" 2000 2>&1 | tee "$ART/stress_reload_storm.log"
RC1=${PIPESTATUS[0]}

# 2. Foreground switch storm + 3. Thread storm — reuse integration binary from run_storm_san.sh
echo ""
echo "=== [2/3] Foreground Switch Storm (S26, 5000 iterations) ==="
echo "=== [3/3] Thread Storm (S25, 2000 iterations) ==="
echo "  (Reusing integration binary from run_storm_san.sh recipe)"

# Build the integration binary with ASan (same as run_storm_san.sh)
SAN="address"
SANBUILD="$IT/build_san"
mkdir -p "$SANBUILD"

# v4.4.8: PROD list mirrors tests/integration/build.sh (and the `main` san
# recipe) exactly. New v4.4.x TUs (psi/arb/preboost/scenewire glue/c4-freezer/
# thermal_ff/thermal_headroom/apklabel/placement_heal) were missing, which
# caused [3/3] thread-storm link to fail with undefined symbols.
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
      cold_start_rules gpu_boost input_monitor aatped_threadutil aatped_apklabel \
      aatped_psi psi_decision touch_uclamp touch_uclamp_act touch_uclamp_fgscan thermal_ff/thermal_ff \
      aatped_arbitrator mech/src/game_scene mech/src/device_profile mech/src/load_forecast mech/src/context \
      thermal_headroom scene_infer c2_envelope placement_heal_decision pre_boost_predict"

# AATPED_ARBITRATOR_DAEMON compiles arb::init/tick daemon wiring (same as
# integration/build.sh PRODFLAGS).
PRODFLAGS2="$BASEFLAGS -fsanitize=$SAN -fno-omit-frame-pointer -g -Dmain=aatped_shim_main -DAATPE_HOST_TEST -DAATPE_GPU_BOOST_TEST -DAATPED_ARBITRATOR_DAEMON"

echo "  Building integration binary ($SAN)..."
for o in $PROD; do
    obj="$SANBUILD/$o.$SAN.o"
    osrc=""
    [ -f "$SRC/$o.cpp" ] && osrc="$SRC/$o.cpp"
    [ -z "$osrc" ] && [ -f "$SRC/$o.c" ] && osrc="$SRC/$o.c"
    if [ -z "$osrc" ]; then echo "FAILED: $o (no .cpp/.c source)"; exit 1; fi
    mkdir -p "$SANBUILD/$(dirname "$o")"
    if [ ! -f "$obj" ] || [ "$osrc" -nt "$obj" ]; then
        echo "    CC $o"
        case "$osrc" in
            *.c) gcc -std=c11 $PRODFLAGS2 -c "$osrc" -o "$obj" -lm 2> "$SANBUILD/$o.$SAN.err" ;;
            *)   $CXX $PRODFLAGS2 -c "$osrc" -o "$obj" 2> "$SANBUILD/$o.$SAN.err" ;;
        esac
        [ $? -ne 0 ] && { echo "FAILED: $o"; head -30 "$SANBUILD/$o.$SAN.err"; exit 1; }
    fi
done

echo "    CC stub_property"
$CXX $BASEFLAGS -fsanitize=$SAN -fno-omit-frame-pointer -g -c "$IT/stubs/stub_property.cpp" -o "$SANBUILD/stub_property.$SAN.o" 2> "$SANBUILD/stub.$SAN.err" || {
    echo "FAILED: stub_property"; head -20 "$SANBUILD/stub.$SAN.err"; exit 1; }

# v4.4.5: host no-op stubs for scenewire glue + c4-freezer snapshot (same as
# tests/integration/build.sh). Without these, aatped_main.o references
# auxTickSlot3/loadingActiveSuppressesPreboost/chooseRoute/c4FreezerTick/
# c4FreezerStateJson which compile out under AATPE_HOST_TEST.
echo "    CC stub_scenewire"
$CXX $BASEFLAGS -fsanitize=$SAN -fno-omit-frame-pointer -g -I$SRC -c "$SRC/tests/stubs/scenewire.cpp" -o "$SANBUILD/stub_scenewire.$SAN.o" 2> "$SANBUILD/stub_sw.$SAN.err" || {
    echo "FAILED: stub_scenewire"; head -20 "$SANBUILD/stub_sw.$SAN.err"; exit 1; }

for t in it_common it_scenarios it_main; do
    echo "    CC $t"
    $CXX $BASEFLAGS -fsanitize=$SAN -fno-omit-frame-pointer -g -DAATPE_HOST_TEST -c "$IT/src/$t.cpp" -o "$SANBUILD/$t.$SAN.o" 2> "$SANBUILD/$t.$SAN.err" || {
        echo "FAILED: $t"; head -40 "$SANBUILD/$t.$SAN.err"; exit 1; }
done

echo "    LINK it_$SAN"
OBJS2=""
for o in $PROD stub_property stub_scenewire it_common it_scenarios it_main; do OBJS2="$OBJS2 $SANBUILD/$o.$SAN.o"; done
$CXX $BASEFLAGS -fsanitize=$SAN -fno-omit-frame-pointer -g $OBJS2 -o "$SANBUILD/it_$SAN" -lz 2> "$SANBUILD/link.$SAN.err" || {
    echo "LINK FAILED"; head -40 "$SANBUILD/link.$SAN.err"; exit 1; }

echo "  Running S25+S26 (ASan)..."
AATPE_ONLY="S25_ThreadStorm,S26_FgSwitchStorm" unshare --map-root-user --mount "$SANBUILD/it_$SAN" 2>&1 | tail -15
RC2=${PIPESTATUS[0]}

# TSan version
echo ""
echo "=== TSan versions (race detection for thread/fg storms) ==="
SAN="thread"
PRODFLAGS3="$BASEFLAGS -fsanitize=$SAN -fno-omit-frame-pointer -g -Dmain=aatped_shim_main -DAATPE_HOST_TEST -DAATPE_GPU_BOOST_TEST -DAATPED_ARBITRATOR_DAEMON"

echo "  Building integration binary ($SAN)..."
for o in $PROD; do
    obj="$SANBUILD/$o.$SAN.o"
    osrc=""
    [ -f "$SRC/$o.cpp" ] && osrc="$SRC/$o.cpp"
    [ -z "$osrc" ] && [ -f "$SRC/$o.c" ] && osrc="$SRC/$o.c"
    if [ -z "$osrc" ]; then echo "FAILED: $o (no .cpp/.c source)"; exit 1; fi
    mkdir -p "$SANBUILD/$(dirname "$o")"
    if [ ! -f "$obj" ] || [ "$osrc" -nt "$obj" ]; then
        echo "    CC $o"
        case "$osrc" in
            *.c) gcc -std=c11 $PRODFLAGS3 -c "$osrc" -o "$obj" -lm 2> "$SANBUILD/$o.$SAN.err" ;;
            *)   $CXX $PRODFLAGS3 -c "$osrc" -o "$obj" 2> "$SANBUILD/$o.$SAN.err" ;;
        esac
        [ $? -ne 0 ] && { echo "FAILED: $o"; head -30 "$SANBUILD/$o.$SAN.err"; exit 1; }
    fi
done

echo "    CC stub_property"
$CXX $BASEFLAGS -fsanitize=$SAN -fno-omit-frame-pointer -g -c "$IT/stubs/stub_property.cpp" -o "$SANBUILD/stub_property.$SAN.o" 2> "$SANBUILD/stub.$SAN.err" || {
    echo "FAILED: stub_property"; head -20 "$SANBUILD/stub.$SAN.err"; exit 1; }

echo "    CC stub_scenewire"
$CXX $BASEFLAGS -fsanitize=$SAN -fno-omit-frame-pointer -g -I$SRC -c "$SRC/tests/stubs/scenewire.cpp" -o "$SANBUILD/stub_scenewire.$SAN.o" 2> "$SANBUILD/stub_sw.$SAN.err" || {
    echo "FAILED: stub_scenewire"; head -20 "$SANBUILD/stub_sw.$SAN.err"; exit 1; }

for t in it_common it_scenarios it_main; do
    echo "    CC $t"
    $CXX $BASEFLAGS -fsanitize=$SAN -fno-omit-frame-pointer -g -DAATPE_HOST_TEST -c "$IT/src/$t.cpp" -o "$SANBUILD/$t.$SAN.o" 2> "$SANBUILD/$t.$SAN.err" || {
        echo "FAILED: $t"; head -40 "$SANBUILD/$t.$SAN.err"; exit 1; }
done

echo "    LINK it_$SAN"
OBJS3=""
for o in $PROD stub_property stub_scenewire it_common it_scenarios it_main; do OBJS3="$OBJS3 $SANBUILD/$o.$SAN.o"; done
$CXX $BASEFLAGS -fsanitize=$SAN -fno-omit-frame-pointer -g $OBJS3 -o "$SANBUILD/it_$SAN" -lz 2> "$SANBUILD/link.$SAN.err" || {
    echo "LINK FAILED"; head -40 "$SANBUILD/link.$SAN.err"; exit 1; }

echo "  Running S25+S26 (TSan)..."
AATPE_ONLY="S25_ThreadStorm,S26_FgSwitchStorm" unshare --map-root-user --mount "$SANBUILD/it_$SAN" 2>&1 | tail -15
RC_TSAN=${PIPESTATUS[0]}

# Summary
echo ""
echo "=============================================="
echo "STRESS STORM SUMMARY"
echo "=============================================="
printf "%-30s %-10s %s\n" "TEST" "RESULT" "ITERATIONS"
printf "%-30s %-10s %s\n" "Config Reload Storm" "$([ $RC1 -eq 0 ] && echo PASS || echo FAIL)" "2000"
printf "%-30s %-10s %s\n" "FG Switch Storm (ASan)" "$([ $RC2 -eq 0 ] && echo PASS || echo FAIL)" "5000"
printf "%-30s %-10s %s\n" "Thread Storm (ASan)" "$([ $RC2 -eq 0 ] && echo PASS || echo FAIL)" "2000"
printf "%-30s %-10s %s\n" "Thread/Fg Storm (TSan)" "$([ $RC_TSAN -eq 0 ] && echo PASS || echo FAIL)" "2000+5000"
echo "=============================================="

exit $((RC1 + RC2 + RC_TSAN))
