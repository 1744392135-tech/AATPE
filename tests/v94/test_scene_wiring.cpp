#include "scene_infer_wiring.h"
#include "scene_infer.h"
#include "c2_envelope.h"
#include <cstdio>
#include <cmath>
#include <cstring>

namespace aatpe {
void aatpeBoostSet(const char* source, int level) { (void)source; (void)level; }
void aatpeBoostClear(const char* source) { (void)source; }
}

using namespace aatpe;
using namespace aatpe::scenefuse;
using namespace aatpe::scenewire;

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s (line %d)\n", msg, __LINE__); } \
} while(0)

int main() {
    printf("=== scene_infer wiring host tests ===\n");

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

    printf("T3: mapSceneToGameScene\n");
    {
        CHECK((int)scenewire::mapSceneToGameScene(Scene::MENU)       == (int)GameScene::MENU,    "T3: MENU->MENU");
        CHECK((int)scenewire::mapSceneToGameScene(Scene::LOADING)    == (int)GameScene::LOADING, "T3: LOADING->LOADING");
        CHECK((int)scenewire::mapSceneToGameScene(Scene::COMBAT)     == (int)GameScene::COMBAT,  "T3: COMBAT->COMBAT");
        CHECK((int)scenewire::mapSceneToGameScene(Scene::TEAMFIGHT)  == (int)GameScene::COMBAT,  "T3: TEAMFIGHT->COMBAT (no separate enum)");
        CHECK((int)scenewire::mapSceneToGameScene(Scene::BACKGROUND) == (int)GameScene::UNKNOWN, "T3: BACKGROUND->UNKNOWN");
        CHECK((int)scenewire::mapSceneToGameScene(Scene::UNKNOWN)    == (int)GameScene::UNKNOWN, "T3: UNKNOWN->UNKNOWN");
    }

    printf("T4: chooseRoute gate\n");
    {
        CHECK(scenewire::chooseRoute(false) == scenewire::SceneRoute::LEGACY_DETECTGAME,
              "T4: switch off -> legacy detectGameScene (byte-identical)");
        CHECK(scenewire::chooseRoute(true) == scenewire::SceneRoute::NEW_SCENEFUSE,
              "T4: switch on -> new scenefuse engine");
    }

    printf("T5: shouldSuppressPreboost (LOADING must not arm C1 latency)\n");
    {
        CHECK(scenewire::shouldSuppressPreboost(false, Scene::LOADING) == false,
              "T5: off+LOADING -> no suppression (C1 untouched)");
        CHECK(scenewire::shouldSuppressPreboost(false, Scene::COMBAT) == false,
              "T5: off+COMBAT -> no suppression");
        CHECK(scenewire::shouldSuppressPreboost(true, Scene::LOADING) == true,
              "T5: on+LOADING -> C1 latency held back");
        CHECK(scenewire::shouldSuppressPreboost(true, Scene::COMBAT) == false,
              "T5: on+COMBAT -> C1 allowed");
        CHECK(scenewire::shouldSuppressPreboost(true, Scene::TEAMFIGHT) == false,
              "T5: on+TEAMFIGHT -> C1 allowed (peak combat)");
        CHECK(scenewire::shouldSuppressPreboost(true, Scene::MENU) == false,
              "T5: on+MENU -> no suppression");
    }

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
        CHECK(d.scene != Scene::BACKGROUND, "T6: foreground game not BACKGROUND");
        CHECK(d.confidence01 > 0.0f, "T6: confidence produced");
    }

    printf("T7: computeTickSlopes first tick has no delta\n");
    {
        TickSlopeState st;
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
        CHECK(std::fabs(o.dGpuBusy_dt - (10.0f/25.0f)) < 1e-3f, "T9: dGpuBusy_dt uses 25s not 5s");
        CHECK(std::fabs(o.tapsPerSec - (100.0f/25.0f)) < 1e-3f, "T9: tapsPerSec uses 25s not 5s");
        CHECK(o.dGpuBusy_dt < 1.0f, "T9: 25s slope far below the old inflated 5s value");
    }

    printf("T10: run-delay slope is computed (not hardcoded 0)\n");
    {
        TickSlopeState st; TickSlopeState r1;
        scenewire::computeTickSlopes(st, 40.0f, 100, 0.0f, 5.0f, r1);
        TickSlopeState r2;
        TickSlopeOut o = scenewire::computeTickSlopes(r1, 40.0f, 100, 0.1f, 5.0f, r2);
        CHECK(std::fabs(o.dRunDelay_dt - 0.02f) < 1e-3f, "T10: dRunDelay_dt = +0.02 /s");
    }

    printf("T11: TEAMFIGHT hits on >=2 slopes with no GPU present\n");
    {
        SceneState st; resetSceneStateForTest(st);
        st.enterMs = 1000 - 120000;
        scenewire::WiringInputs in;
        in.fpsNow = 60.0f; in.fpsTarget = 60.0f; in.missRate = 0.005f;
        in.cpuUtilFg = 30.0f; in.psiCpuSome = 5.0f;
        in.gpuBusyPct = 0.0f; in.gpuAvail = false; in.gpuTag = "none";
        in.psiAvail = true; in.schedAvail = true; in.fgIsGame = true;
        in.tapsPerSec = 3.0f; in.dTaps_dt = 1.0f;
        in.runDelayPerMs = 0.02f; in.dRunDelay_dt = 0.05f;
        in.dGpuBusy_dt = 0.0f;
        SceneFeatures f = scenewire::buildFeatures(in);
        CHECK((f.capsMask & kCapGPU) == 0, "T11: GPU cap absent");
        inferScene(f, st, 1000);
        SceneDecision d2 = inferScene(f, st, 2000);
        CHECK(d2.scene == Scene::TEAMFIGHT, "T11: TEAMFIGHT reached on 2 slopes w/o GPU");
    }

    printf("T12: coexistence registry carries the two proven audit entries\n");
    {
        CHECK(scenewire::externalSchedRegistrySize() >= 2,
              "T12: registry non-empty");
        CHECK(scenewire::externalSchedRegistryHas("scene_fas_ft_provider"),
              "T12: registry pins scene_fas_ft_provider");
        CHECK(scenewire::externalSchedRegistryHas("scene_swap_controller"),
              "T12: registry pins scene_swap_controller");
    }

    printf("T13: detectExternalSchedulers flags the audit processes, ignores normal ones\n");
    {
        scenewire::ExternalSchedReport quiet = scenewire::detectExternalSchedulers(
            {"surfaceflinger", "system_server", "com.android.systemui", "mt_manager"});
        CHECK(quiet.detected == false, "T13: ordinary processes -> no detection");
        CHECK(quiet.matchCount == 0, "T13: matchCount 0 on quiet system");

        scenewire::ExternalSchedReport hit = scenewire::detectExternalSchedulers(
            {"surfaceflinger", "scene_fas_ft_provider", "system_server", "scene_swap_controller"});
        CHECK(hit.detected == true, "T13: Scene工具箱 processes detected");
        CHECK(hit.matchCount == 2, "T13: both competing schedulers counted");
        CHECK(hit.matchedNames.find("scene_fas_ft_provider") != std::string::npos,
              "T13: first name reported");
        CHECK(hit.matchedNames.find("scene_swap_controller") != std::string::npos,
              "T13: second name reported");

        scenewire::ExternalSchedReport mod = scenewire::detectExternalSchedulers(
            {"some_scenetoolbox_module"});
        CHECK(mod.detected == true, "T13: module-dir substring match detects Scene工具箱");
    }

    printf("T14: state-file contract for the WebUI external-scheduler banner (#102)\n");
    {
        scenewire::ExternalSchedReport clean = scenewire::detectExternalSchedulers(
            {"surfaceflinger", "system_ui"});
        CHECK(clean.detected == false, "T14: clean system -> detected=false (file would be '0')");
        CHECK(clean.matchedNames.empty(), "T14: clean system -> names empty (no banner)");

        scenewire::ExternalSchedReport hit = scenewire::detectExternalSchedulers(
            {"scene_fas_ft_provider", "scene_swap_controller"});
        CHECK(hit.detected == true, "T14: competing schedulers -> detected=true (file would be '1')");
        CHECK(hit.matchedNames.find(',') != std::string::npos,
              "T14: two matches -> names contain a comma separator");
        CHECK(hit.matchedNames.find("scene_fas_ft_provider") != std::string::npos,
              "T14: names carry scene_fas_ft_provider");
        CHECK(hit.matchedNames.find("scene_swap_controller") != std::string::npos,
              "T14: names carry scene_swap_controller");
    }

    printf("\n=== Results: %d passed, %d failed ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}