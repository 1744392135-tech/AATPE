// test_gamescene.cpp - v94 hermetic unit tests for aatped_gamescene.cpp
// Links aatped_gamescene.cpp + gamescene_deps (see build.sh recipe).
// Covers: sceneToString mapping, detectGameScene classification (COMBAT/LOADING/
// MENU/UNKNOWN) and the >=3-consecutive hysteresis gate.
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
    // Set GPU util high (combat needs cpu>60, gpu>50, jitter>2).
    g_gpuUtilization.store(90.0f);
    // 1st detection: not yet stable, g_currentScene stays UNKNOWN
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
    // Need to re-stabilize MENU: low cpu + low gpu, low jitter.
    detectGameScene(10.0f, 0.5f, 16.0f);
    detectGameScene(10.0f, 0.5f, 16.0f);
    GameScene rm = detectGameScene(10.0f, 0.5f, 16.0f);
    CHECK(rm == GameScene::MENU, "low cpu+gpu -> MENU after 3 consecutive");

    printf("=== Gamescene: partial conditions -> UNKNOWN ===\n");
    g_gpuUtilization.store(5.0f);  // low gpu blocks combat
    detectGameScene(90.0f, 5.0f, 16.0f);
    detectGameScene(90.0f, 5.0f, 16.0f);
    GameScene rx = detectGameScene(90.0f, 5.0f, 16.0f);
    // high cpu + high jitter but LOW gpu -> no COMBAT; high cpu blocks MENU;
    // high cpu blocks LOADING only if gpu low AND jitter low (jitter=5 high) -> UNKNOWN
    CHECK(rx == GameScene::UNKNOWN, "high cpu + high jitter + low gpu -> UNKNOWN");

    // ------------------------------------------------------------------
    // #103 (rel15): a NON-game foreground must never be classified as a game
    // scene. The audit device had MT manager (bin.mt.plus) foreground yet the
    // legacy load-based path locked current_game_scene="combat". With the
    // foreground-game gate closed, identical high-CPU+jitter load must stay UNKNOWN.
    // ------------------------------------------------------------------
    printf("=== Gamescene #103: non-game foreground never locks COMBAT ===\n");
    gamescene_test::setLegacyFgIsGame(false);   // e.g. MT manager bin.mt.plus
    g_gpuUtilization.store(90.0f);              // would otherwise be combat
    GameScene ng1 = detectGameScene(90.0f, 5.0f, 16.0f);
    GameScene ng2 = detectGameScene(90.0f, 5.0f, 16.0f);
    GameScene ng3 = detectGameScene(90.0f, 5.0f, 16.0f);
    CHECK(ng1 == GameScene::UNKNOWN, "#103: non-game fg, 1st tick -> UNKNOWN");
    CHECK(ng2 == GameScene::UNKNOWN, "#103: non-game fg, 2nd tick -> UNKNOWN");
    CHECK(ng3 == GameScene::UNKNOWN, "#103: non-game fg, 3rd tick -> still UNKNOWN (not COMBAT)");
    CHECK((GameScene)g_currentScene.load() == GameScene::UNKNOWN,
          "#103: g_currentScene stays UNKNOWN despite combat-shaped load");

    // Re-open a real game: the gate re-arms and combat locks after 3 ticks again.
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
