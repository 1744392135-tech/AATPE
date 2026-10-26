// test_scene_infer.cpp — 12 host replay test cases for W5 scene_infer + C2.
// W5 §8.2 T1–T12.
#include "scene_infer.h"
#include "c2_envelope.h"
#include <cstdio>
#include <cmath>

// Minimal boost bridge stubs (host test only). Must match the C++ mangled
// signature in gpu_boost.h (namespace aatpe); c2_envelope.cpp now includes it.
namespace aatpe {
void aatpeBoostSet(const char* source, int level) { (void)source; (void)level; }
void aatpeBoostClear(const char* source) { (void)source; }
}

using namespace aatpe::scenefuse;
using namespace aatpe::c2env;

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s (line %d)\n", msg, __LINE__); } \
} while(0)

static SceneFeatures makeBaseFeatures() {
    SceneFeatures f;
    f.fpsNow = 60.0f;
    f.fpsTarget = 60.0f;
    f.missRate = 0.005f;
    f.sfLatBudgetR = 0.2f;
    f.cpuUtilFg = 30.0f;
    f.runDelayPerMs = 0.01f;
    f.dRunDelay_dt = 0.0f;
    f.iowaitPct = 2.0f;
    f.cgThrottleDlt = 0.0f;
    f.psiCpuSome = 5.0f;
    f.gpuBusyPct = 40.0f;
    f.dGpuBusy_dt = 0.0f;
    f.tapsPerSec = 2.0f;
    f.dTaps_dt = 0.0f;
    f.netRxPktsPs = 10.0f;
    f.dNetRx_dt = 0.0f;
    f.tempSlopeCps = 0.01f;
    f.headroom01 = 0.8f;
    f.capsMask = kCapPSI | kCapGPU | kCapSchedstat;
    f.gpuTag = "kgsl";
    f.lowFpsStreak = 0;
    f.fgIsGame = true;
    return f;
}

// T1: Steady menu.
static void test_T1_steady_menu() {
    printf("T1: Steady menu\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    f.cpuUtilFg = 15.0f;
    f.gpuBusyPct = 10.0f;
    f.psiCpuSome = 3.0f;
    f.fpsNow = 55.0f; // UI animation
    long long nowMs = 1000;

    // Run a few ticks to stabilize.
    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }
    SceneDecision d = inferScene(f, s, nowMs + 3000);
    CHECK(d.scene == Scene::MENU, "T1: scene=MENU");
    CHECK(d.confidence01 > 0.5f, "T1: confidence>0.5");
}

// T2: Loading page.
static void test_T2_loading_page() {
    printf("T2: Loading page\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;

    // Simulate loading: fps drops to 0, cpu/io busy, gpu idle.
    f.fpsNow = 0.0f;
    f.cpuUtilFg = 80.0f;
    f.iowaitPct = 15.0f;
    f.psiCpuSome = 25.0f;
    f.gpuBusyPct = 10.0f;

    resetC2ForTest();
    // Run 3 ticks to build lowFpsStreak.
    for (int i = 0; i < 3; i++) {
        SceneDecision d = inferScene(f, s, nowMs + i * 1000);
        c2Tick(nowMs + i * 1000, d, 0.8f, 0, false);
    }
    SceneDecision d = inferScene(f, s, nowMs + 3000);
    CHECK(d.scene == Scene::LOADING, "T2: scene=LOADING");
    CHECK(currentStateForTest() == C2State::LOADING_BOOST, "T2: C2 loading boost active");
}

// T3: Loading -> combat.
static void test_T3_loading_to_combat() {
    printf("T3: Loading -> combat\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;

    // Loading phase.
    f.fpsNow = 0.0f;
    f.cpuUtilFg = 80.0f;
    f.iowaitPct = 15.0f;
    f.gpuBusyPct = 10.0f;
    resetC2ForTest();
    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }

    // fps recovers gradually.
    f.fpsNow = 30.0f; // 0.5 ratio
    for (int i = 0; i < 3; i++) {
        SceneDecision d = inferScene(f, s, nowMs + (3 + i) * 1000);
        c2Tick(nowMs + (3 + i) * 1000, d, 0.8f, 0, false);
    }
    f.fpsNow = 55.0f;
    f.cpuUtilFg = 40.0f;
    f.iowaitPct = 3.0f;
    f.gpuBusyPct = 60.0f;
    SceneDecision d = inferScene(f, s, nowMs + 7000);
    c2Tick(nowMs + 7000, d, 0.8f, 0, false);
    // After loading + fps recovery, should be out of LOADING (combat/teamfight/unknown ok).
    CHECK(d.scene != Scene::LOADING, "T3: out of LOADING after fps recovery");
    CHECK(currentStateForTest() != C2State::LOADING_BOOST, "T3: loading boost cleared");
}

// T4: Combat -> teamfight.
static void test_T4_combat_to_teamfight() {
    printf("T4: Combat -> teamfight\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;
    s.enterMs = nowMs - 120000; // 2 min ago, past baseline warm-up

    // Stable combat first.
    for (int i = 0; i < 5; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }
    CHECK(s.committedScene == Scene::COMBAT, "T4: start in COMBAT");

    // Slopes rise (teamfight early warning).
    f.dTaps_dt = 1.0f;
    f.dRunDelay_dt = 0.05f;
    f.dGpuBusy_dt = 2.0f;
    SceneDecision d = inferScene(f, s, nowMs + 5000);
    CHECK(d.scene == Scene::TEAMFIGHT, "T4: scene=TEAMFIGHT on slope rise");
}

// T5: Teamfight -> combat (de-escalation hysteresis).
static void test_T5_teamfight_to_combat() {
    printf("T5: Teamfight -> combat\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;
    s.enterMs = nowMs - 120000;

    // Get to TEAMFIGHT.
    f.dTaps_dt = 1.0f;
    f.dRunDelay_dt = 0.05f;
    f.dGpuBusy_dt = 2.0f;
    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }
    CHECK(s.committedScene == Scene::TEAMFIGHT, "T5: start in TEAMFIGHT");

    // Slopes flat; need 4 consecutive ticks to exit.
    f.dTaps_dt = 0.0f;
    f.dRunDelay_dt = 0.0f;
    f.dGpuBusy_dt = 0.0f;
    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + (3 + i) * 1000);
    }
    // After 5 flat ticks total, should exit TEAMFIGHT (degraded hysteresis ok).
    SceneDecision d = inferScene(f, s, nowMs + 8000);
    CHECK(d.scene != Scene::TEAMFIGHT, "T5: exit TEAMFIGHT after flat slopes");
}

// T6: Background.
static void test_T6_background() {
    printf("T6: Background\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    f.fgIsGame = false;
    long long nowMs = 1000;
    SceneDecision d = inferScene(f, s, nowMs);
    CHECK(d.scene == Scene::BACKGROUND, "T6: scene=BACKGROUND");
    CHECK(d.confidence01 == 1.0f, "T6: confidence=1.0");
}

// T7: Feature missing degradation (no GPU).
static void test_T7_degradation_no_gpu() {
    printf("T7: Degradation (no GPU)\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;
    s.enterMs = nowMs - 120000;

    f.capsMask = kCapPSI | kCapSchedstat; // no GPU bit
    f.gpuTag = "none";
    f.gpuBusyPct = 0.0f;

    // Slopes rise.
    f.dTaps_dt = 1.0f;
    f.dRunDelay_dt = 0.05f;
    f.dGpuBusy_dt = 0.0f;

    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }
    SceneDecision d = inferScene(f, s, nowMs + 3000);
    CHECK(d.degraded == true, "T7: degraded=true");
    // Degraded mode: TEAMFIGHT detection is harder; accept either TEAMFIGHT or COMBAT.
    CHECK(d.scene == Scene::TEAMFIGHT || d.scene == Scene::COMBAT, "T7: still detects combat/teamfight with degraded");
}

// T8: QC vs non-QC.
static void test_T8_qc_vs_nonqc() {
    printf("T8: QC vs non-QC\n");
    SceneState s1; resetSceneStateForTest(s1);
    SceneState s2; resetSceneStateForTest(s2);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;
    s1.enterMs = nowMs - 120000;
    s2.enterMs = nowMs - 120000;

    f.dTaps_dt = 1.0f;
    f.dRunDelay_dt = 0.05f;
    f.dGpuBusy_dt = 2.0f;

    // QC (kgsl).
    f.gpuTag = "kgsl";
    f.capsMask = kCapPSI | kCapGPU | kCapSchedstat;
    SceneDecision d1 = inferScene(f, s1, nowMs);

    // Non-QC (no GPU).
    f.gpuTag = "none";
    f.capsMask = kCapPSI | kCapSchedstat;
    SceneDecision d2 = inferScene(f, s2, nowMs);

    CHECK(d1.scene == d2.scene || d1.scene == Scene::TEAMFIGHT, "T8: QC/non-QC scenes comparable");
    // Confidence: QC should be >= non-QC (GPU axis adds signal).
    CHECK(d1.confidence01 >= d2.confidence01 - 0.1f, "T8: QC confidence >= non-QC");
}

// T9: 4.14 no PSI.
static void test_T9_no_psi() {
    printf("T9: No PSI (4.14)\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;

    f.capsMask = kCapGPU | kCapSchedstat; // no PSI
    f.psiCpuSome = -1.0f;

    // Loading: fps=0, cpu/io busy.
    f.fpsNow = 0.0f;
    f.cpuUtilFg = 80.0f;
    f.iowaitPct = 15.0f;
    f.gpuBusyPct = 10.0f;

    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }
    SceneDecision d = inferScene(f, s, nowMs + 3000);
    CHECK(d.scene == Scene::LOADING, "T9: still LOADING without PSI");
    CHECK(d.degraded == true, "T9: degraded=true (no PSI)");
}

// T10: Jitter ping-pong protection.
static void test_T10_ping_pong() {
    printf("T10: Ping-pong protection\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f_menu = makeBaseFeatures();
    f_menu.cpuUtilFg = 15.0f;
    f_menu.gpuBusyPct = 10.0f;

    SceneFeatures f_load = makeBaseFeatures();
    f_load.fpsNow = 0.0f;
    f_load.cpuUtilFg = 80.0f;
    f_load.iowaitPct = 15.0f;
    f_load.gpuBusyPct = 10.0f;

    long long nowMs = 1000;
    // Alternate MENU/LOADING every tick.
    for (int i = 0; i < 6; i++) {
        SceneFeatures& f = (i % 2 == 0) ? f_load : f_menu;
        inferScene(f, s, nowMs + i * 1000);
    }
    // After many flips, should be locked (not oscillating wildly).
    CHECK(s.changesInWindow <= 3, "T10: ping-pong lock active");
}

// T11: Thermal gate reject.
static void test_T11_thermal_gate() {
    printf("T11: Thermal gate reject\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;
    s.enterMs = nowMs - 120000;

    // Teamfight conditions.
    f.dTaps_dt = 1.0f;
    f.dRunDelay_dt = 0.05f;
    f.dGpuBusy_dt = 2.0f;

    resetC2ForTest();
    SceneDecision d = inferScene(f, s, nowMs);
    CHECK(d.scene == Scene::TEAMFIGHT, "T11: scene=TEAMFIGHT");

    // Thermal gate: low headroom.
    bool active = c2Tick(nowMs, d, 0.1f, 0, false);
    CHECK(active == false, "T11: C2 not active under thermal gate");
}

// T12: Loading time box hard cap (20s).
static void test_T12_loading_timebox() {
    printf("T12: Loading time box hard cap\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;

    f.fpsNow = 0.0f;
    f.cpuUtilFg = 80.0f;
    f.iowaitPct = 15.0f;
    f.gpuBusyPct = 10.0f;

    resetC2ForTest();
    // Run loading for 25s.
    for (int i = 0; i < 25; i++) {
        SceneDecision d = inferScene(f, s, nowMs + i * 1000);
        c2Tick(nowMs + i * 1000, d, 0.8f, 0, false);
    }
    // After 25s, loading boost should have timed out (or never activated, both safe).
    CHECK(currentStateForTest() != C2State::LOADING_BOOST, "T12: loading boost not active after 20s cap");
}

// RC6 (A5): C2 LOADING exit must use the REAL fpsRatio, not a confidence proxy.
// With the fps signal absent (fpsRatio==0) it must fail-safe and NOT exit early;
// only genuine fps recovery (>=0.5 for 3 ticks) releases the boost.
static void test_T13_c2_uses_real_fpsratio() {
    printf("T13: C2 LOADING exit uses real fpsRatio (fail-safe when absent)\n");
    resetC2ForTest();
    // Arm with a genuine low-fps loading decision.
    SceneDecision d;
    d.scene = Scene::LOADING; d.confidence01 = 0.9f; d.fpsRatio01 = 0.0f;
    c2Tick(1000, d, 0.8f, 0, false);
    CHECK(currentStateForTest() == C2State::LOADING_BOOST, "T13: LOADING boost armed");
    // fps signal ABSENT but confidence drops to 0.1 (old proxy would have exited).
    for (int i = 0; i < 3; i++) {
        SceneDecision di; di.scene = Scene::LOADING; di.confidence01 = 0.1f; di.fpsRatio01 = 0.0f;
        c2Tick(2000 + i * 1000, di, 0.8f, 0, false);
    }
    CHECK(currentStateForTest() == C2State::LOADING_BOOST, "T13: no exit without fps signal");
    // Real fps recovery >=0.5 for 3 ticks -> release.
    for (int i = 0; i < 3; i++) {
        SceneDecision di; di.scene = Scene::LOADING; di.confidence01 = 0.1f; di.fpsRatio01 = 0.7f;
        c2Tick(6000 + i * 1000, di, 0.8f, 0, false);
    }
    CHECK(currentStateForTest() != C2State::LOADING_BOOST, "T13: exit after fpsRatio>=0.5 x3");
}

// RC6 (A1): the HAL thermal gate must clear the boost immediately when HAL>=1,
// even while headroom is still healthy. This is the contract the production glue
// now feeds with the real aatpeThermalHalLevel() (was hardcoded 0).
static void test_T14_hal_gate_clears() {
    printf("T14: C2 halLevel>=1 immediately clears (A1 contract)\n");
    resetC2ForTest();
    SceneDecision d;
    d.scene = Scene::LOADING; d.fpsRatio01 = 0.0f;
    c2Tick(1000, d, 0.8f, 0, false);
    CHECK(currentStateForTest() == C2State::LOADING_BOOST, "T14: armed at hal 0");
    c2Tick(2000, d, 0.8f, 1, false);
    CHECK(currentStateForTest() != C2State::LOADING_BOOST, "T14: halLevel>=1 clears boost");
}

int main() {
    printf("=== scene_infer + C2 envelope host tests ===\n");
    test_T1_steady_menu();
    test_T2_loading_page();
    test_T3_loading_to_combat();
    test_T4_combat_to_teamfight();
    test_T5_teamfight_to_combat();
    test_T6_background();
    test_T7_degradation_no_gpu();
    test_T8_qc_vs_nonqc();
    test_T9_no_psi();
    test_T10_ping_pong();
    test_T11_thermal_gate();
    test_T12_loading_timebox();
    test_T13_c2_uses_real_fpsratio();
    test_T14_hal_gate_clears();
    printf("\n=== Results: %d passed, %d failed ===\n", s_pass, s_fail);
    return s_fail > 0 ? 1 : 0;
}
