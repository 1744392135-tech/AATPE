// test_scene_wiring.cpp — host tests for the W5 slow-thread wiring layer.
// Covers:  input -> SceneFeatures mapping,  Scene -> GameScene mapping,
// gate routing (on=new engine / off=legacy byte-identical), and the W5 §6
// LOADING->C1-preboost suppression. The production I/O glue is compiled out under
// AATPE_HOST_TEST, so this links only the pure mapping layer.
#include "scene_infer_wiring.h"
#include "scene_infer.h"
#include "c2_envelope.h"
#include <cstdio>
#include <cmath>
#include <cstring>

// Minimal boost bridge stubs (c2_envelope.o references them even though this test
// does not drive c2Tick; keep the link hermetic). Match gpu_boost.h C++ mangled sig.
namespace aatpe {
void aatpeBoostSet(const char* source, int level) { (void)source; (void)level; }
void aatpeBoostClear(const char* source) { (void)source; }
}

using namespace aatpe;
using namespace aatpe::scenefuse;
using namespace aatpe::scenewire;   // RC6: TickSlopeState/TickSlopeOut pure slope types

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s (line %d)\n", msg, __LINE__); } \
} while(0)

int main() {
    printf("=== scene_infer wiring host tests ===\n");

    // ---- 1. input -> SceneFeatures mapping ----
    printf("T1: buildFeatures maps raw inputs -> SceneFeatures\n");
    {
        scenewire::WiringInputs in;
        in.fpsNow = 45.0f;
        in.fpsTarget = 60.0f;
        in.missRate = 0.05f;
        in.sfLatBudgetR = 0.4f;
        in.cpuUtilFg = 55.0f;
        in.runDelayPerMs = 0.02f;
        in.dRunDelay_dt = 0.03f;
        in.iowaitPct = 12.0f;
        in.psiCpuSome = 8.5f;
        in.gpuBusyPct = 70.0f;
        in.dGpuBusy_dt = 1.5f;
        in.tapsPerSec = 4.0f;
        in.dTaps_dt = 0.6f;
        in.netRxPktsPs = 20.0f;
        in.dNetRx_dt = 0.2f;
        in.headroom01 = 0.7f;
        in.psiAvail = true;
        in.gpuAvail = true;
        in.schedAvail = true;
        in.gpuTag = "kgsl";
        in.fgIsGame = true;

        SceneFeatures f = scenewire::buildFeatures(in);
        CHECK(f.fpsNow == 45.0f, "T1: fpsNow passthrough");
        CHECK(f.fpsTarget == 60.0f, "T1: fpsTarget passthrough");
        CHECK(f.missRate == 0.05f, "T1: missRate passthrough");
        CHECK(f.cpuUtilFg == 55.0f, "T1: cpuUtilFg passthrough");
        CHECK(f.psiCpuSome == 8.5f, "T1: psiCpuSome passthrough");
        CHECK(f.gpuBusyPct == 70.0f, "T1: gpuBusyPct passthrough");
        CHECK(f.dGpuBusy_dt == 1.5f, "T1: dGpuBusy_dt passthrough");
        CHECK(f.tapsPerSec == 4.0f, "T1: tapsPerSec passthrough");
        CHECK((f.capsMask & (kCapPSI|kCapGPU|kCapSchedstat)) ==
              (kCapPSI|kCapGPU|kCapSchedstat), "T1: capsMask all bits set");
        CHECK(std::strcmp(f.gpuTag, "kgsl") == 0, "T1: gpuTag copied");
        CHECK(f.fgIsGame == true, "T1: fgIsGame passthrough");
        CHECK(f.lowFpsStreak == 0, "T1: lowFpsStreak left to engine (0)");
    }

    // fpsTarget clamp: 0 -> default 60 (no div by zero in engine).
    printf("T2: buildFeatures fpsTarget clamp + caps subset\n");
    {
        scenewire::WiringInputs in;
        in.fpsTarget = 0.0f;
        in.psiAvail = true;
        in.gpuAvail = false;
        in.schedAvail = false;
        in.gpuTag = "none";
        SceneFeatures f = scenewire::buildFeatures(in);
        CHECK(f.fpsTarget == 60.0f, "T2: fpsTarget clamped to 60 when 0");
        CHECK(f.capsMask == kCapPSI, "T2: capsMask only PSI set");
        CHECK(f.gpuTag && std::strcmp(f.gpuTag, "none") == 0, "T2: gpuTag none");
    }

    // ---- 2. Scene -> GameScene mapping ----
    printf("T3: mapSceneToGameScene\n");
    {
        CHECK((int)scenewire::mapSceneToGameScene(Scene::MENU)       == (int)GameScene::MENU,    "T3: MENU->MENU");
        CHECK((int)scenewire::mapSceneToGameScene(Scene::LOADING)    == (int)GameScene::LOADING, "T3: LOADING->LOADING");
        CHECK((int)scenewire::mapSceneToGameScene(Scene::COMBAT)     == (int)GameScene::COMBAT,  "T3: COMBAT->COMBAT");
        CHECK((int)scenewire::mapSceneToGameScene(Scene::TEAMFIGHT)  == (int)GameScene::COMBAT,  "T3: TEAMFIGHT->COMBAT (no separate enum)");
        CHECK((int)scenewire::mapSceneToGameScene(Scene::BACKGROUND) == (int)GameScene::UNKNOWN, "T3: BACKGROUND->UNKNOWN");
        CHECK((int)scenewire::mapSceneToGameScene(Scene::UNKNOWN)    == (int)GameScene::UNKNOWN, "T3: UNKNOWN->UNKNOWN");
    }

    // ---- 3. gate routing ----
    printf("T4: chooseRoute gate\n");
    {
        CHECK(scenewire::chooseRoute(false) == scenewire::SceneRoute::LEGACY_DETECTGAME,
              "T4: switch off -> legacy detectGameScene (byte-identical)");
        CHECK(scenewire::chooseRoute(true) == scenewire::SceneRoute::NEW_SCENEFUSE,
              "T4: switch on -> new scenefuse engine");
    }

    // ---- 4. W5 §6 LOADING -> C1 preboost suppression ----
    printf("T5: shouldSuppressPreboost (LOADING must not arm C1 latency)\n");
    {
        // Off path: never suppress, whatever the committed scene.
        CHECK(scenewire::shouldSuppressPreboost(false, Scene::LOADING) == false,
              "T5: off+LOADING -> no suppression (C1 untouched)");
        CHECK(scenewire::shouldSuppressPreboost(false, Scene::COMBAT) == false,
              "T5: off+COMBAT -> no suppression");
        // On path: only LOADING suppresses.
        CHECK(scenewire::shouldSuppressPreboost(true, Scene::LOADING) == true,
              "T5: on+LOADING -> C1 latency held back");
        CHECK(scenewire::shouldSuppressPreboost(true, Scene::COMBAT) == false,
              "T5: on+COMBAT -> C1 allowed");
        CHECK(scenewire::shouldSuppressPreboost(true, Scene::TEAMFIGHT) == false,
              "T5: on+TEAMFIGHT -> C1 allowed (peak combat)");
        CHECK(scenewire::shouldSuppressPreboost(true, Scene::MENU) == false,
              "T5: on+MENU -> no suppression");
    }

    // ---- 5. end-to-end through the engine on a known flat load (smoke) ----
    printf("T6: buildFeatures -> inferScene produces a sane decision\n");
    {
        SceneState st; resetSceneStateForTest(st);
        scenewire::WiringInputs in;
        in.fpsNow = 60.0f; in.fpsTarget = 60.0f; in.missRate = 0.005f;
        in.cpuUtilFg = 30.0f; in.psiCpuSome = 5.0f;
        in.gpuBusyPct = 40.0f; in.psiAvail = true; in.gpuAvail = true;
        in.schedAvail = true; in.gpuTag = "kgsl"; in.fgIsGame = true;
        in.tapsPerSec = 2.0f;
        SceneFeatures f = scenewire::buildFeatures(in);
        SceneDecision d = inferScene(f, st, 1000);
        // A stable game frame at 60fps on-target should not be UNKNOWN/BACKGROUND.
        CHECK(d.scene != Scene::BACKGROUND, "T6: foreground game not BACKGROUND");
        CHECK(d.confidence01 > 0.0f, "T6: confidence produced");
    }

    // ---- 7. RC6 (A2/A3): pure slope computation over REAL wall time ----------
    printf("T7: computeTickSlopes first tick has no delta\n");
    {
        TickSlopeState st;            // have=false
        TickSlopeState next;
        TickSlopeOut o = scenewire::computeTickSlopes(st, 50.0f, 100, 0.0f, 5.0f, next);
        CHECK(next.have == true, "T7: state marked have after first tick");
        CHECK(o.tapsPerSec == 0.0f, "T7: tapsPerSec 0 on first tick");
        CHECK(o.dGpuBusy_dt == 0.0f, "T7: dGpuBusy_dt 0 on first tick");
        CHECK(o.dTaps_dt == 0.0f, "T7: dTaps_dt 0 on first tick");
        CHECK(o.dRunDelay_dt == 0.0f, "T7: dRunDelay_dt 0 on first tick");
    }

    printf("T8: 5s cadence slopes are correct magnitudes\n");
    {
        TickSlopeState st; TickSlopeState n1;
        scenewire::computeTickSlopes(st, 40.0f, 100, 0.0f, 5.0f, n1);
        TickSlopeState n2;
        TickSlopeOut o = scenewire::computeTickSlopes(n1, 50.0f, 200, 0.0f, 5.0f, n2);
        // gpu 40->50 over 5s => +2 %/s ; touch 100 events over 5s => 20 taps/s
        CHECK(std::fabs(o.dGpuBusy_dt - 2.0f) < 1e-3f, "T8: dGpuBusy_dt = +2 %/s over 5s");
        CHECK(std::fabs(o.tapsPerSec - 20.0f) < 1e-3f, "T8: tapsPerSec = 20 over 5s");
        TickSlopeState n3;
        TickSlopeOut o3 = scenewire::computeTickSlopes(n2, 60.0f, 300, 0.0f, 5.0f, n3);
        CHECK(std::fabs(o3.dGpuBusy_dt - 2.0f) < 1e-3f, "T8: dGpuBusy_dt stays +2 %/s");
        CHECK(std::fabs(o3.dTaps_dt - 0.0f) < 1e-3f, "T8: dTaps_dt ~0 when tap rate flat");
    }

    printf("T9: SAME raw deltas over 25s are NOT inflated 5x\n");
    {
        TickSlopeState st; TickSlopeState m1;
        scenewire::computeTickSlopes(st, 40.0f, 100, 0.0f, 25.0f, m1);
        TickSlopeState m2;
        TickSlopeOut o = scenewire::computeTickSlopes(m1, 50.0f, 200, 0.0f, 25.0f, m2);
        // gpu 40->50 over 25s => +0.4 %/s (old hardcoded-5s code would report +2.0).
        CHECK(std::fabs(o.dGpuBusy_dt - (10.0f/25.0f)) < 1e-3f, "T9: dGpuBusy_dt uses 25s not 5s");
        CHECK(std::fabs(o.tapsPerSec - (100.0f/25.0f)) < 1e-3f, "T9: tapsPerSec uses 25s not 5s");
        // The 5s computation reported +2.0 %/s; the 25s must be ~5x smaller.
        CHECK(o.dGpuBusy_dt < 1.0f, "T9: 25s slope far below the old inflated 5s value");
    }

    printf("T10: run-delay slope is computed (not hardcoded 0)\n");
    {
        TickSlopeState st; TickSlopeState r1;
        scenewire::computeTickSlopes(st, 40.0f, 100, 0.0f, 5.0f, r1);
        TickSlopeState r2;
        // run-delay 0.0 -> 0.1 over 5s => dRunDelay_dt = 0.02 /s
        TickSlopeOut o = scenewire::computeTickSlopes(r1, 40.0f, 100, 0.1f, 5.0f, r2);
        CHECK(std::fabs(o.dRunDelay_dt - 0.02f) < 1e-3f, "T10: dRunDelay_dt = +0.02 /s");
    }

    // ---- 11. RC6 (M3): TEAMFIGHT fires on >=2 rising slopes even WITHOUT GPU ----
    printf("T11: TEAMFIGHT hits on >=2 slopes with no GPU present\n");
    {
        SceneState st; resetSceneStateForTest(st);
        st.enterMs = 1000 - 120000;   // past the 60s warm-up so requiredHits=2
        scenewire::WiringInputs in;
        in.fpsNow = 60.0f; in.fpsTarget = 60.0f; in.missRate = 0.005f;
        in.cpuUtilFg = 30.0f; in.psiCpuSome = 5.0f;
        // No GPU at all: no cap bit, "none" tag, gpuBusy=0.
        in.gpuBusyPct = 0.0f; in.gpuAvail = false; in.gpuTag = "none";
        in.psiAvail = true; in.schedAvail = true; in.fgIsGame = true;
        // Two rising slopes: taps AND run-delay (no GPU slope available).
        in.tapsPerSec = 3.0f; in.dTaps_dt = 1.0f;
        in.runDelayPerMs = 0.02f; in.dRunDelay_dt = 0.05f;
        in.dGpuBusy_dt = 0.0f;
        SceneFeatures f = scenewire::buildFeatures(in);
        CHECK((f.capsMask & kCapGPU) == 0, "T11: GPU cap absent");
        // A couple of ticks to settle, then assert TEAMFIGHT (>=2 hits: taps+runDelay).
        inferScene(f, st, 1000);
        SceneDecision d2 = inferScene(f, st, 2000);
        CHECK(d2.scene == Scene::TEAMFIGHT, "T11: TEAMFIGHT reached on 2 slopes w/o GPU");
    }

    printf("\n=== Results: %d passed, %d failed ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}