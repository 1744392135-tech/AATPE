#include "aatped.h"

const char* CONF_DIR = "/tmp/aatpe_v94_state";
const char* RUN_DIR  = "/tmp/aatpe_v94_run";
std::atomic<unsigned int> g_debugSampleCounter{0};
std::vector<ThreadRule> g_rules;

std::string aatpeDetectForegroundApp(bool) { return std::string(); }
int findPidByPackage(const std::string&) { return -1; }
bool isKernelThread(const std::string&, int) { return false; }
void mkdirIfNotExist(const char*, mode_t) {}
int readThreadCurrentCpu(int, int) { return -1; }
void placementInvalidateSettle(int) {}

void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}

#include "aatpe_recovery.h"
namespace aatpe_recovery { void recoveryErase(int ) {} }

namespace aatpe_mech { void aatpeUclampFloorReap(int ) {} }

std::unordered_map<int, ThreadScoreEntry> g_scoreCache;
std::mutex g_scoreCacheMutex;
const long long kInactiveMs = 10000;
