// thread_deps.cpp - host link deps for thread_scan.cpp.
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
// #34: settle-state invalidation lives in placement.cpp; neutral host stub so
// thread_scan tests that call invalidateAuxThreadState link without placement.o.
void placementInvalidateSettle(int) {}

void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}

// P1-8: recovery erase hook (no-op in test env)
#include "aatpe_recovery.h"
namespace aatpe_recovery { void recoveryErase(int /*tid*/) {} }

// RC6 (B2): per-tid uclamp floor ledger reap. Real impl in aatpe_placement_mech.cpp;
// neutral host stub so thread_scan tests link without the mech object.
namespace aatpe_mech { void aatpeUclampFloorReap(int /*tid*/) {} }

// rel15 大小核颠倒: refreshTrackedThread() now consults g_scoreCache[tid] and
// kInactiveMs to break the skipSchedstat freeze when lastSwitchMs is fresh. Provide
// the same symbols the production aatped_main.cpp defines, so thread_scan.o links
// under host test. The map is normally empty in these tests (no onSwitch wired).
std::unordered_map<int, ThreadScoreEntry> g_scoreCache;
std::mutex g_scoreCacheMutex;
// Defined in aatped_main.cpp; host targets that link thread_scan.o without
// aatped_main.o need the definition here. Must match kInactiveMs = 10000.
const long long kInactiveMs = 10000;
