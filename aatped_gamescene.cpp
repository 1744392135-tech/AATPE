// AATPE - Game scene detection (extracted from aatped_main.cpp)
#include "aatped.h"

// #103 (rel15): the legacy detector gates on whether the foreground package is a
// real game. Those accessors live in the daemon TU; under AATPE_HOST_TEST the
// production I/O is compiled out and a test-seam flag is used instead.
#ifndef AATPE_HOST_TEST
#include "aatped_config.h"   // isPackageInGameList
#include "aatped_state.h"    // aatpeGetForegroundPackage
#endif

namespace {
#ifdef AATPE_HOST_TEST
// Host-test seam. Default true: preserves the historical load-only hysteresis
// tests which never establish a foreground package. The #103 regression test flips
// this to false to model a non-game foreground (e.g. MT manager).
std::atomic<bool> g_legacyFgIsGameForTest{true};
#else
// Runtime foreground-game evidence: the resumed package is a known game. This is
// the SAME evidence the scenewire engine uses (scene_infer_wiring.cpp), so the two
// routes agree. No package names are hardcoded here.
bool legacyFgForegroundIsGame() {
    std::string pkg = aatpeGetForegroundPackage();
    return !pkg.empty() && isPackageInGameList(pkg);
}
#endif
} // namespace

#ifdef AATPE_HOST_TEST
namespace gamescene_test { void setLegacyFgIsGame(bool v) { g_legacyFgIsGameForTest.store(v, std::memory_order_relaxed); } }
#endif

// Game scene detection
// atomic<int> storing GameScene cast to int (see aatped_gamescene.h).
std::atomic<int> g_currentScene{(int)GameScene::UNKNOWN};
// P3-36: g_sceneChangeCount now increments whenever the stable scene actually
// changes (was a dead variable that was never written).
// R2-L2: made std::atomic<int> — it is written on the daemon main loop and is read
// by the WebUI/state export thread; a plain int increment-vs-read is a data race
// (UB) even though aligned ints don't tear on arm64. Relaxed order is sufficient
// (diagnostic counter, no dependent state).
std::atomic<int> g_sceneChangeCount{0};

const char* sceneToString(GameScene s) {
    switch(s) {
        case GameScene::LOADING: return "loading";
        case GameScene::COMBAT: return "combat";
        case GameScene::MENU: return "menu";
        default: return "unknown";
    }
}

// Game scene configurable thresholds
struct SceneConfig { float combatCpu=60, combatGpu=50, combatJitter=2; float loadingCpu=70, loadingGpu=30, loadingJitter=1; float menuCpu=30, menuGpu=20; int stableCount=3; } g_sceneConfig;

GameScene detectGameScene(float cpuLoad, float frameTimeJitter, [[maybe_unused]] float avgFrameTime) {
    // #103 (rel15): foreground-game gate. The audit device had MT manager
    // (bin.mt.plus) in the foreground yet current_game_scene was stuck "combat",
    // because this legacy path thresholds CPU+jitter with ZERO game evidence. A
    // non-game foreground must never be classified into a game scene. The evidence
    // is the same runtime package-in-game-list check the scenewire engine uses.
#ifdef AATPE_HOST_TEST
    bool fgIsGame = g_legacyFgIsGameForTest.load(std::memory_order_relaxed);
#else
    bool fgIsGame = legacyFgForegroundIsGame();
#endif

    // P2-8: these statics are only accessed from detectGameScene() which runs on
    // the daemon main loop (single-threaded). No mutex needed. Moved to the top so
    // the non-game gate below can reset them.
    static GameScene s_lastScene = GameScene::UNKNOWN;
    static int s_sceneStableCount = 0;
    GameScene prevStable = (GameScene)g_currentScene.load(std::memory_order_acquire);

    if (!fgIsGame) {
        // Not a confirmed game foreground: never hold a game scene. Reset the
        // hysteresis so a later real game re-arms the >=3-consecutive gate cleanly.
        s_lastScene = GameScene::UNKNOWN;
        s_sceneStableCount = 0;
        if (prevStable != GameScene::UNKNOWN) g_sceneChangeCount++;
        g_currentScene.store((int)GameScene::UNKNOWN, std::memory_order_release);
        {
            std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
            g_sceneState.currentScene = (int)GameScene::UNKNOWN;
        }
        return GameScene::UNKNOWN;
    }

    // P6: Multi-dimensional scene detection with GPU, audio, and hysteresis
    GameScene detected = GameScene::UNKNOWN;

    // COMBAT: high CPU + high GPU + frame jitter
    float gpuUtil = g_gpuUtilization.load(std::memory_order_relaxed);
    // ROUND-014 P3 (S14-01): on non-QC devices g_gpuUtilization never gets populated and
    // stays 0, which made COMBAT unreachable and LOADING misfire on any high-CPU load.
    // Once we have EVER seen a valid (>0) GPU sample use GPU; else degrade to CPU+jitter
    // (never auto-classify LOADING on CPU alone).
    static bool gpuEverSampled = false;
    if (gpuUtil > 0.0f) gpuEverSampled = true;

    if (gpuEverSampled) {
        if (cpuLoad > g_sceneConfig.combatCpu && gpuUtil > g_sceneConfig.combatGpu && frameTimeJitter > g_sceneConfig.combatJitter) {
            detected = GameScene::COMBAT;
        }
        else if (cpuLoad > g_sceneConfig.loadingCpu && gpuUtil < g_sceneConfig.loadingGpu && frameTimeJitter < g_sceneConfig.loadingJitter) {
            detected = GameScene::LOADING;
        }
        else if (cpuLoad < g_sceneConfig.menuCpu && gpuUtil < g_sceneConfig.menuGpu) {
            detected = GameScene::MENU;
        }
    } else {
        if (cpuLoad > g_sceneConfig.combatCpu && frameTimeJitter > g_sceneConfig.combatJitter) {
            detected = GameScene::COMBAT;
        } else if (cpuLoad < g_sceneConfig.menuCpu) {
            detected = GameScene::MENU;
        }
    }

    // Hysteresis: require >= 3 consecutive same detections (stableCount threshold)
    // s_lastScene / s_sceneStableCount / prevStable are declared at the top of this
    // function so the #103 non-game gate can reset them.
    if (detected == s_lastScene) {
        s_sceneStableCount++;
        // Clamp at stableCount: once the scene is locked in, there's no need to
        // keep incrementing indefinitely (prevents overflow on long sessions).
        if (s_sceneStableCount > g_sceneConfig.stableCount)
            s_sceneStableCount = g_sceneConfig.stableCount;
    } else {
        s_sceneStableCount = 1;
        s_lastScene = detected;
    }

    if (s_sceneStableCount >= g_sceneConfig.stableCount) {
        // P3-36: count the actual scene transition (prev -> new).
        if (detected != prevStable) g_sceneChangeCount++;
        g_currentScene.store((int)detected, std::memory_order_release);
        // LED-088 (R7-003): g_sceneState.currentScene is read by
        // writeStateJson() under g_stateDiagMutex; publish it under the SAME lock
        // (the atomic g_currentScene store above remains the lock-free fast path).
        {
            std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
            g_sceneState.currentScene = (int)detected;
        }
        // ML-01: the on-disk STATE_DIR/current_game_scene file is published ONLY by
        // the main-loop state republish tick (aatped_main.cpp). The two detectors
        // (this legacy rule-based detector and scenewire) must NOT write the file
        // directly, otherwise they race on the same path when routing flips between
        // them. In-memory state (g_currentScene atomic + g_sceneState.currentScene)
        // is the single source of truth; the file is a diagnostic mirror refreshed
        // at the periodic publish cadence.
    }
    return (GameScene)g_currentScene.load(std::memory_order_acquire);
}
