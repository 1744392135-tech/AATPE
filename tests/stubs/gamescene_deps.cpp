#include "aatped.h"

const char* STATE_DIR = "/tmp/aatpe_v94_gamescene";
std::atomic<float> g_gpuUtilization{0.0f};
SceneState g_sceneState;
std::mutex g_stateDiagMutex;

#include <fstream>
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int) { return true; }
bool writeFile(const char*, float) { return true; }
bool writeFile(const char*, long long) { return true; }
