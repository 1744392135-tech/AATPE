#ifndef AATPED_GAMESCENE_H
#define AATPED_GAMESCENE_H

#include <atomic>
enum class GameScene { UNKNOWN, LOADING, COMBAT, MENU };

extern std::atomic<int> g_currentScene;
extern std::atomic<int> g_sceneChangeCount;

const char* sceneToString(GameScene s);
GameScene detectGameScene(float cpuLoad, float frameTimeJitter, float avgFrameTime);

#ifdef AATPE_HOST_TEST
namespace gamescene_test { void setLegacyFgIsGame(bool v); }
#endif

#endif
