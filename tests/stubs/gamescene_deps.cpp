// gamescene_deps.cpp - host link deps for aatped_gamescene.cpp (NOT linked with
// v94_daemon_deps, which would multiply-define g_currentScene). Provides only
// the externs gamescene.cpp references but does not itself define.
#include "aatped.h"

const char* STATE_DIR = "/tmp/aatpe_v94_gamescene";
// P1-6: atomic<float> to match production type.
std::atomic<float> g_gpuUtilization{0.0f};
SceneState g_sceneState;
// v94 stub-integration: detectGameScene() publishes the scene under
// g_stateDiagMutex (real home aatped_state.cpp:82). The gamescene hermetic recipe
// links aatped_gamescene.o in isolation (no aatped_state.o), so provide it here.
std::mutex g_stateDiagMutex;

#include <fstream>
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int) { return true; }
bool writeFile(const char*, float) { return true; }
bool writeFile(const char*, long long) { return true; }
