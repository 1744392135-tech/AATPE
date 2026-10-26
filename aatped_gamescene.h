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

#endif // AATPED_GAMESCENE_H
