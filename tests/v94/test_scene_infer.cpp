#include "scene_infer.h"
#include "c2_envelope.h"
#include <cstdio>
#include <cmath>

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

static void test_T1_steady_menu() {
    printf("T1: Steady menu\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    f.cpuUtilFg = 15.0f;
    f.gpuBusyPct = 10.0f;
    f.psiCpuSome = 3.0f;
    f.fpsNow = 55.0f;
    long long nowMs = 1000;

    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }
    SceneDecision d = inferScene(f, s, nowMs + 3000);
    CHECK(d.scene == Scene::MENU, "T1: scene=MENU");
    CHECK(d.confidence01 > 0.5f, "T1: confidence>0.5");
}

static void test_T2_loading_page() {
    printf("T2: Loading page\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;

    f.fpsNow = 0.0f;
    f.cpuUtilFg = 80.0f;
    f.iowaitPct = 15.0f;
    f.psiCpuSome = 25.0f;
    f.gpuBusyPct = 10.0f;

    resetC2ForTest();
    for (int i = 0; i < 3; i++) {
        SceneDecision d = inferScene(f, s, nowMs + i * 1000);
        c2Tick(nowMs + i * 1000, d, 0.8f, 0, false);
    }
    SceneDecision d = inferScene(f, s, nowMs + 3000);
    CHECK(d.scene == Scene::LOADING, "T2: scene=LOADING");
    CHECK(currentStateForTest() == C2State::LOADING_BOOST, "T2: C2 loading boost active");
}

static void test_T3_loading_to_combat() {
    printf("T3: Loading -> combat\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;

    f.fpsNow = 0.0f;
    f.cpuUtilFg = 80.0f;
    f.iowaitPct = 15.0f;
    f.gpuBusyPct = 10.0f;
    resetC2ForTest();
    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }

    f.fpsNow = 30.0f;
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
    CHECK(d.scene != Scene::LOADING, "T3: out of LOADING after fps recovery");
    CHECK(currentStateForTest() != C2State::LOADING_BOOST, "T3: loading boost cleared");
}

static void test_T4_combat_to_teamfight() {
    printf("T4: Combat -> teamfight\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;
    s.enterMs = nowMs - 120000;

    for (int i = 0; i < 5; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }
    CHECK(s.committedScene == Scene::COMBAT, "T4: start in COMBAT");

    f.dTaps_dt = 1.0f;
    f.dRunDelay_dt = 0.05f;
    f.dGpuBusy_dt = 2.0f;
    SceneDecision d = inferScene(f, s, nowMs + 5000);
    CHECK(d.scene == Scene::TEAMFIGHT, "T4: scene=TEAMFIGHT on slope rise");
}

static void test_T5_teamfight_to_combat() {
    printf("T5: Teamfight -> combat\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;
    s.enterMs = nowMs - 120000;

    f.dTaps_dt = 1.0f;
    f.dRunDelay_dt = 0.05f;
    f.dGpuBusy_dt = 2.0f;
    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }
    CHECK(s.committedScene == Scene::TEAMFIGHT, "T5: start in TEAMFIGHT");

    f.dTaps_dt = 0.0f;
    f.dRunDelay_dt = 0.0f;
    f.dGpuBusy_dt = 0.0f;
    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + (3 + i) * 1000);
    }
    SceneDecision d = inferScene(f, s, nowMs + 8000);
    CHECK(d.scene != Scene::TEAMFIGHT, "T5: exit TEAMFIGHT after flat slopes");
}

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

static void test_T7_degradation_no_gpu() {
    printf("T7: Degradation (no GPU)\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;
    s.enterMs = nowMs - 120000;

    f.capsMask = kCapPSI | kCapSchedstat;
    f.gpuTag = "none";
    f.gpuBusyPct = 0.0f;

    f.dTaps_dt = 1.0f;
    f.dRunDelay_dt = 0.05f;
    f.dGpuBusy_dt = 0.0f;

    for (int i = 0; i < 3; i++) {
        inferScene(f, s, nowMs + i * 1000);
    }
    SceneDecision d = inferScene(f, s, nowMs + 3000);
    CHECK(d.degraded == true, "T7: degraded=true");
    CHECK(d.scene == Scene::TEAMFIGHT || d.scene == Scene::COMBAT, "T7: still detects combat/teamfight with degraded");
}

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

    f.gpuTag = "kgsl";
    f.capsMask = kCapPSI | kCapGPU | kCapSchedstat;
    SceneDecision d1 = inferScene(f, s1, nowMs);

    f.gpuTag = "none";
    f.capsMask = kCapPSI | kCapSchedstat;
    SceneDecision d2 = inferScene(f, s2, nowMs);

    CHECK(d1.scene == d2.scene || d1.scene == Scene::TEAMFIGHT, "T8: QC/non-QC scenes comparable");
    CHECK(d1.confidence01 >= d2.confidence01 - 0.1f, "T8: QC confidence >= non-QC");
}

static void test_T9_no_psi() {
    printf("T9: No PSI (4.14)\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;

    f.capsMask = kCapGPU | kCapSchedstat;
    f.psiCpuSome = -1.0f;

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
    for (int i = 0; i < 6; i++) {
        SceneFeatures& f = (i % 2 == 0) ? f_load : f_menu;
        inferScene(f, s, nowMs + i * 1000);
    }
    CHECK(s.changesInWindow <= 3, "T10: ping-pong lock active");
}

static void test_T11_thermal_gate() {
    printf("T11: Thermal gate reject\n");
    SceneState s; resetSceneStateForTest(s);
    SceneFeatures f = makeBaseFeatures();
    long long nowMs = 1000;
    s.enterMs = nowMs - 120000;

    f.dTaps_dt = 1.0f;
    f.dRunDelay_dt = 0.05f;
    f.dGpuBusy_dt = 2.0f;

    resetC2ForTest();
    SceneDecision d = inferScene(f, s, nowMs);
    CHECK(d.scene == Scene::TEAMFIGHT, "T11: scene=TEAMFIGHT");

    bool active = c2Tick(nowMs, d, 0.1f, 0, false);
    CHECK(active == false, "T11: C2 not active under thermal gate");
}

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
    for (int i = 0; i < 25; i++) {
        SceneDecision d = inferScene(f, s, nowMs + i * 1000);
        c2Tick(nowMs + i * 1000, d, 0.8f, 0, false);
    }
    CHECK(currentStateForTest() != C2State::LOADING_BOOST, "T12: loading boost not active after 20s cap");
}

static void test_T13_c2_uses_real_fpsratio() {
    printf("T13: C2 LOADING exit uses real fpsRatio (fail-safe when absent)\n");
    resetC2ForTest();
    SceneDecision d;
    d.scene = Scene::LOADING; d.confidence01 = 0.9f; d.fpsRatio01 = 0.0f;
    c2Tick(1000, d, 0.8f, 0, false);
    CHECK(currentStateForTest() == C2State::LOADING_BOOST, "T13: LOADING boost armed");
    for (int i = 0; i < 3; i++) {
        SceneDecision di; di.scene = Scene::LOADING; di.confidence01 = 0.1f; di.fpsRatio01 = 0.0f;
        c2Tick(2000 + i * 1000, di, 0.8f, 0, false);
    }
    CHECK(currentStateForTest() == C2State::LOADING_BOOST, "T13: no exit without fps signal");
    for (int i = 0; i < 3; i++) {
        SceneDecision di; di.scene = Scene::LOADING; di.confidence01 = 0.1f; di.fpsRatio01 = 0.7f;
        c2Tick(6000 + i * 1000, di, 0.8f, 0, false);
    }
    CHECK(currentStateForTest() != C2State::LOADING_BOOST, "T13: exit after fpsRatio>=0.5 x3");
}

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
