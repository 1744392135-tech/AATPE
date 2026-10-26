// applaunch_deps.cpp - host link deps for aatped_applaunch.cpp.
#include "aatped.h"

const char* STATE_DIR = "/tmp/aatpe_v94_applaunch";
int g_cpuCount = 8;
std::vector<int> g_midPool{3, 4, 5};
std::vector<int> g_bigPool{6, 7};
// v94 stub-integration: recordAppLaunch() snapshots g_midPool/g_bigPool under
// g_topologyRwMutex (real home placement.cpp:72). aatped_applaunch.cpp defines
// g_appLaunchHistoryMutex itself, so only this mutex is missing.
std::mutex g_topologyRwMutex;

float readCpuUtilizationSchedstat(int /*cpu*/) { return 20.0f; }

#include <fstream>
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int) { return true; }
bool writeFile(const char*, float) { return true; }
bool writeFile(const char*, long long) { return true; }
