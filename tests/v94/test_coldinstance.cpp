#include <cstdio>
#include <string>
#include <vector>
#include <atomic>

#include "cold_start_rules.cpp"

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

std::atomic<int> g_logLevel{3};
void logFlush() {}
void logMsg(int, const char*, ...) {}
bool isPackageInGameList(const std::string& p) { return p == "com.test.game"; }

std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;

const char* STATE_DIR = "/tmp/coldinst_state";
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, long long) { return true; }
std::string readFile(const char*) { return std::string(); }
std::string aatpeExecArgv(const std::vector<std::string>&, const char*, int*, bool) { return ""; }
namespace aatpe { namespace frameaware {
bool jankBoostVoteActive() { return false; }
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
                         else { g_fail++; printf("  [FAIL] %s\n", m); } } while (0)

static long long s_clk = 1000000;
static long long clkFn() { return s_clk; }

static void seedGameThread() {
    g_threadsMutex.lock();
    g_threads.clear();
    ThreadInfo t; t.tid = 77; t.pid = 77; t.pname = "com.test.game"; t.tname = "main";
    g_threads[77] = t;
    g_threadsMutex.unlock();
}

int main() {
    using namespace aatpe;
    printf("=== F3.1: cold/hot start by process-instance identity ===\n");

    coldStartSetNowForTest(clkFn);
    seedGameThread();

    s_clk = 1000000;
    coldStartSetProcIdentityForTest("com.test.game", 100, 111u);
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "A: new instance -> armed");
    CHECK(coldStartIsColdForTest() == true, "A: first sighting classified COLD (full window)");
    s_clk += 6000;
    coldStartTick();
    CHECK(coldStartActive() == true, "A: at age=6000ms a COLD window is still active (>=15s)");
    coldStartOnForegroundSwitch("com.other.app");
    CHECK(coldStartActive() == false, "A: leave fg -> disarmed");

    s_clk += 60001;
    coldStartSetProcIdentityForTest("com.test.game", 100, 111u);
    s_clk += 2000;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "B: same-instance return -> armed");
    CHECK(coldStartIsColdForTest() == false, "B: SAME live instance classified WARM (no full 15-30s)");
    s_clk += 6000;
    coldStartTick();
    CHECK(coldStartActive() == false, "B: at age=6000ms a WARM window has already released (<=5s)");
    coldStartOnForegroundSwitch("com.other.app");

    s_clk += 60001;
    coldStartSetProcIdentityForTest("com.test.game", 100, 111u);
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "F: arm -> active");
    coldStartOnForegroundSwitch("com.other.app");
    CHECK(coldStartActive() == false, "F: disarmed");
    s_clk += 500;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == false, "F: <1s away-and-back is debounced (no re-arm)");
    s_clk += 1000;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "F: after debounce window, return -> armed(warm)");
    CHECK(coldStartIsColdForTest() == false, "F: still same instance -> warm");
    coldStartOnForegroundSwitch("com.other.app");

    s_clk += 60001;
    coldStartSetProcIdentityForTest("com.test.game", 100, 222u);
    s_clk += 2000;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "C: restart (pid same, new start_time) -> armed");
    CHECK(coldStartIsColdForTest() == true, "C: start_time changed => NEW instance => COLD");
    s_clk += 6000;
    coldStartTick();
    CHECK(coldStartActive() == true, "C: at age=6000ms cold window still active (>=15s)");
    coldStartOnForegroundSwitch("com.other.app");

    s_clk += 60001;
    coldStartSetProcIdentityForTest("com.test.game", 200, 222u);
    s_clk += 2000;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "D: restart (new pid) -> armed");
    CHECK(coldStartIsColdForTest() == true, "D: pid changed (start_time same) => NEW instance => COLD");
    coldStartOnForegroundSwitch("com.other.app");

    s_clk += 60001;
    coldStartSetProcIdentityForTest("com.test.game", 0, 0u);
    s_clk += 2000;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "E: unresolvable identity -> armed (conservative cold)");
    CHECK(coldStartIsColdForTest() == true, "E: unresolvable identity => COLD (no regression)");
    coldStartOnForegroundSwitch("com.other.app");

    s_clk += 60001;
    long long base = coldStartRejectedCountForTest();
    coldStartSetProcIdentityForTest("com.test.game", 300, 333u);
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true && coldStartIsColdForTest() == true, "G: arm1 cold");
    coldStartOnForegroundSwitch("com.other.app");
    s_clk += 1500;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true && coldStartIsColdForTest() == false, "G: arm2 warm (same instance)");
    coldStartOnForegroundSwitch("com.other.app");
    s_clk += 1500;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "G: arm3 accepted (3rd within 60s)");
    coldStartOnForegroundSwitch("com.other.app");
    s_clk += 1500;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == false, "G: arm4 within 60s refused by rate cap");
    CHECK(coldStartRejectedCountForTest() == base + 1, "G: refused warm arm bumps cold_start_rejected");

    coldStartSetNowForTest(nullptr);

    printf("\n========================================\n"
           "ColdInstance results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
