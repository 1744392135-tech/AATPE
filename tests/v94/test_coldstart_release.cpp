// HOTFIX isolated test — #2: cold-start release goes through the ungated restore
// channel (aatpeRestoreThreadPriority), NOT aatpeSetThreadPriority(level=0).
// Includes cold_start_rules.cpp whole; stubs both priority entry points and records
// which one the release path calls.
#include <cstdio>
#include <string>
#include <vector>
#include <atomic>

#include "cold_start_rules.cpp"

// ---- record boost (setter) and release (restore) calls ----------------------
namespace aatpe_priority {
static std::vector<int> g_setterLevels;
bool aatpeSetThreadPriority(int, ThreadRole, int level) {
    g_setterLevels.push_back(level); return true;
}
static std::vector<int> g_restoredTids;
bool aatpeRestoreThreadPriority(int tid) {
    g_restoredTids.push_back(tid); return true;
}
}

// other cold_start_rules deps.
std::atomic<int> g_logLevel{3};
void logFlush() {}
void logMsg(int, const char*, ...) {}
bool isPackageInGameList(const std::string& p) { return p == "com.test.game"; }

// thread table + lock (normally provided by placement_deps in the v94 recipe).
std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;

// sysfs / state stubs.
const char* STATE_DIR = "/tmp/hotfix_state";
bool writeFile(const char*, const std::string&) { return true; }
// v4.2 C2: cold_start_rules now mirrors the cold_start_rejected counter via the
// long-long writeFile overload. Neutral no-op stub (this test does not inspect it).
bool writeFile(const char*, long long) { return true; }
// F3.1: resolveProcIdentity() (whole-TU) references readFile for /proc scans.
// Returning "" makes the production /proc scan find no package -> conservative
// cold classification, which is exactly the behaviour this release test pins.
std::string readFile(const char*) { return std::string(); }
std::string aatpeExecArgv(const std::vector<std::string>&, const char*, int*, bool) { return ""; }
namespace aatpe { namespace frameaware {
bool jankBoostVoteActive() { return false; }
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
                         else { g_fail++; printf("  [FAIL] %s\n", m); } } while (0)

int main() {
    printf("=== HOTFIX #2: coldstart release uses restore channel ===\n");

    using namespace aatpe;
    // seed a boostable thread for com.test.game
    g_threadsMutex.lock();
    g_threads.clear();
    ThreadInfo t; t.tid = 77; t.pid = 77; t.pname = "com.test.game"; t.tname = "main";
    g_threads[77] = t;
    g_threadsMutex.unlock();

    // arm + tick => boost via setter (level>0)
    coldStartOnForegroundSwitch("com.test.game");
    coldStartTick();
    bool sawBoost = false;
    for (int l : aatpe_priority::g_setterLevels) if (l > 0) sawBoost = true;
    CHECK(sawBoost, "arm+tick applies boost via setter (level>0)");

    // now trigger release (leave foreground). Must call restore, NOT setter(level=0).
    aatpe_priority::g_restoredTids.clear();
    aatpe_priority::g_setterLevels.clear();
    coldStartOnForegroundSwitch("com.other.app");   // leave fg -> disarm -> release
    bool restored77 = false;
    for (int tid : aatpe_priority::g_restoredTids) if (tid == 77) restored77 = true;
    CHECK(restored77, "release calls aatpeRestoreThreadPriority(77)");
    CHECK(aatpe_priority::g_setterLevels.empty(), "release does NOT call level=0 setter");

    printf("\n========================================\n"
           "Coldstart release results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
