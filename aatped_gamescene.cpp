#include "aatped.h"

#ifndef AATPE_HOST_TEST
#include "aatped_config.h"
#include "aatped_state.h"
#endif

namespace {
#ifdef AATPE_HOST_TEST
std::atomic<bool> g_legacyFgIsGameForTest{true};
#else
bool legacyFgForegroundIsGame() {
    std::string pkg = aatpeGetForegroundPackage();
    return !pkg.empty() && isPackageInGameList(pkg);
}
#endif
}

#ifdef AATPE_HOST_TEST
namespace gamescene_test { void setLegacyFgIsGame(bool v) { g_legacyFgIsGameForTest.store(v, std::memory_order_relaxed); } }
#endif

std::atomic<int> g_currentScene{(int)GameScene::UNKNOWN};
std::atomic<int> g_sceneChangeCount{0};

const char* sceneToString(GameScene s) {
    switch(s) {
        case GameScene::LOADING: return "loading";
        case GameScene::COMBAT: return "combat";
        case GameScene::MENU: return "menu";
        default: return "unknown";
    }
}

struct SceneConfig { float combatCpu=60, combatGpu=50, combatJitter=2; float loadingCpu=70, loadingGpu=30, loadingJitter=1; float menuCpu=30, menuGpu=20; int stableCount=3; } g_sceneConfig;

GameScene detectGameScene(float cpuLoad, float frameTimeJitter, [[maybe_unused]] float avgFrameTime) {
#ifdef AATPE_HOST_TEST
    bool fgIsGame = g_legacyFgIsGameForTest.load(std::memory_order_relaxed);
#else
    bool fgIsGame = legacyFgForegroundIsGame();
#endif

    static GameScene s_lastScene = GameScene::UNKNOWN;
    static int s_sceneStableCount = 0;
    GameScene prevStable = (GameScene)g_currentScene.load(std::memory_order_acquire);

    if (!fgIsGame) {
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

    GameScene detected = GameScene::UNKNOWN;

    float gpuUtil = g_gpuUtilization.load(std::memory_order_relaxed);
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

    if (detected == s_lastScene) {
        s_sceneStableCount++;
        if (s_sceneStableCount > g_sceneConfig.stableCount)
            s_sceneStableCount = g_sceneConfig.stableCount;
    } else {
        s_sceneStableCount = 1;
        s_lastScene = detected;
    }

    if (s_sceneStableCount >= g_sceneConfig.stableCount) {
        if (detected != prevStable) g_sceneChangeCount++;
        g_currentScene.store((int)detected, std::memory_order_release);
        {
            std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
            g_sceneState.currentScene = (int)detected;
        }
    }
    return (GameScene)g_currentScene.load(std::memory_order_acquire);
}
