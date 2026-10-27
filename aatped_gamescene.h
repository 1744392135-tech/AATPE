// AATPE - Game scene detection sub-header
#ifndef AATPED_GAMESCENE_H
#define AATPED_GAMESCENE_H

// Game scene recognition
#include <atomic>
enum class GameScene { UNKNOWN, LOADING, COMBAT, MENU };

// atomic<int> storing the GameScene cast to int. Written by
// detectGameScene() (main loop), read by thermal.cpp and detectGameScene itself.
// Release-on-store / acquire-on-load.
extern std::atomic<int> g_currentScene;
// R2-L2: atomic so the WebUI/state-export read is race-free against the main-loop
// increment.
extern std::atomic<int> g_sceneChangeCount;

const char* sceneToString(GameScene s);
GameScene detectGameScene(float cpuLoad, float frameTimeJitter, float avgFrameTime);

#ifdef AATPE_HOST_TEST
// #103 (rel15) test seam: inject whether the foreground counts as a real game.
// Production derives this from the live foreground package + game list; the host
// test default is "true" so the historical load-only hysteresis tests are
// unaffected, and the new regression test flips it to false for a non-game fg.
namespace gamescene_test { void setLegacyFgIsGame(bool v); }
#endif

#endif // AATPED_GAMESCENE_H
