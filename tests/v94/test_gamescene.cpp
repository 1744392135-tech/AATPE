#include <cstdio>
#include "aatped.h"
#include "aatped_gamescene.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== Gamescene: sceneToString ===\n");
    CHECK(std::string(sceneToString(GameScene::LOADING)) == "loading", "LOADING -> loading");
    CHECK(std::string(sceneToString(GameScene::COMBAT)) == "combat", "COMBAT -> combat");
    CHECK(std::string(sceneToString(GameScene::MENU)) == "menu", "MENU -> menu");
    CHECK(std::string(sceneToString(GameScene::UNKNOWN)) == "unknown", "UNKNOWN -> unknown");

    printf("=== Gamescene: hysteresis needs >=3 consecutive detections ===\n");
    g_gpuUtilization.store(90.0f);
    GameScene r1 = detectGameScene(90.0f, 5.0f, 16.0f);
    CHECK(r1 == GameScene::UNKNOWN, "1st COMBAT detection: hysteresis holds UNKNOWN");
    GameScene r2 = detectGameScene(90.0f, 5.0f, 16.0f);
    CHECK(r2 == GameScene::UNKNOWN, "2nd COMBAT detection: still holding (need 3)");
    GameScene r3 = detectGameScene(90.0f, 5.0f, 16.0f);
    CHECK(r3 == GameScene::COMBAT, "3rd consecutive -> COMBAT committed");
    CHECK((GameScene)g_currentScene.load() == GameScene::COMBAT, "g_currentScene now COMBAT");
    CHECK(g_sceneChangeCount >= 0, "scene change counter untouched/nonnegative");

    printf("=== Gamescene: MENU requires low cpu + low gpu ===\n");
    g_gpuUtilization.store(5.0f);
    detectGameScene(10.0f, 0.5f, 16.0f);
    detectGameScene(10.0f, 0.5f, 16.0f);
    GameScene rm = detectGameScene(10.0f, 0.5f, 16.0f);
    CHECK(rm == GameScene::MENU, "low cpu+gpu -> MENU after 3 consecutive");

    printf("=== Gamescene: partial conditions -> UNKNOWN ===\n");
    g_gpuUtilization.store(5.0f);
    detectGameScene(90.0f, 5.0f, 16.0f);
    detectGameScene(90.0f, 5.0f, 16.0f);
    GameScene rx = detectGameScene(90.0f, 5.0f, 16.0f);
    CHECK(rx == GameScene::UNKNOWN, "high cpu + high jitter + low gpu -> UNKNOWN");

    printf("=== Gamescene #103: non-game foreground never locks COMBAT ===\n");
    gamescene_test::setLegacyFgIsGame(false);
    g_gpuUtilization.store(90.0f);
    GameScene ng1 = detectGameScene(90.0f, 5.0f, 16.0f);
    GameScene ng2 = detectGameScene(90.0f, 5.0f, 16.0f);
    GameScene ng3 = detectGameScene(90.0f, 5.0f, 16.0f);
    CHECK(ng1 == GameScene::UNKNOWN, "#103: non-game fg, 1st tick -> UNKNOWN");
    CHECK(ng2 == GameScene::UNKNOWN, "#103: non-game fg, 2nd tick -> UNKNOWN");
    CHECK(ng3 == GameScene::UNKNOWN, "#103: non-game fg, 3rd tick -> still UNKNOWN (not COMBAT)");
    CHECK((GameScene)g_currentScene.load() == GameScene::UNKNOWN,
          "#103: g_currentScene stays UNKNOWN despite combat-shaped load");

    printf("=== Gamescene #103: returning to a real game re-arms combat ===\n");
    gamescene_test::setLegacyFgIsGame(true);
    (void)detectGameScene(90.0f, 5.0f, 16.0f);
    (void)detectGameScene(90.0f, 5.0f, 16.0f);
    GameScene g3 = detectGameScene(90.0f, 5.0f, 16.0f);
    CHECK(g3 == GameScene::COMBAT, "#103: real game fg, 3rd consecutive -> COMBAT");
    CHECK((GameScene)g_currentScene.load() == GameScene::COMBAT,
          "#103: g_currentScene back to COMBAT for the real game");

    printf("\n========================================\n"
           "Gamescene results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
