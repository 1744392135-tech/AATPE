// F3.1 (v4.3.8) directed regression: cold/hot start distinction by process-instance
// identity. Includes cold_start_rules.cpp whole; stubs both priority entry points and
// drives the coldStartSetProcIdentityForTest() seam to simulate:
//   A. true cold start  (new process instance)          -> full 15-30s window
//   B. warm/hot resume  (same live instance, brief away) -> short <=5s window
//   C. process restart  (same PID, new start_time)      -> full cold window
//   D. process restart  (new PID, start_time unchanged)  -> full cold window
//   E. identity gone     (process killed / /proc miss)   -> conservative cold
//   F. sub-1s rapid switch -> debounce suppresses re-arm entirely
//   G. per-package 3/min rate cap ALSO counts warm arms
//
// Each major section starts with a clock jump >60s so the per-package arm-rate
// window is pruned and every section has a fresh 3-arm budget.
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

// thread table + lock.
std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;

// sysfs / state stubs.
const char* STATE_DIR = "/tmp/coldinst_state";
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, long long) { return true; }
// F3.1: resolveProcIdentity() references readFile; the identity seam short-circuits
// before it, so this stub is only a link-time provider (never exercised).
std::string readFile(const char*) { return std::string(); }
std::string aatpeExecArgv(const std::vector<std::string>&, const char*, int*, bool) { return ""; }
namespace aatpe { namespace frameaware {
bool jankBoostVoteActive() { return false; }   // no jank -> convergence streak grows
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
                         else { g_fail++; printf("  [FAIL] %s\n", m); } } while (0)

// injected clock (ms).
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

    // ---------- A. true cold start: first sighting of a new instance ----------
    s_clk = 1000000;
    coldStartSetProcIdentityForTest("com.test.game", 100, 111u);
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "A: new instance -> armed");
    CHECK(coldStartIsColdForTest() == true, "A: first sighting classified COLD (full window)");
    // Cold window must NOT close at age=6000ms (min is 15000ms).
    s_clk += 6000;
    coldStartTick();
    CHECK(coldStartActive() == true, "A: at age=6000ms a COLD window is still active (>=15s)");
    coldStartOnForegroundSwitch("com.other.app");
    CHECK(coldStartActive() == false, "A: leave fg -> disarmed");

    // ---------- B. warm/hot resume: SAME instance alive, brief away -----------
    s_clk += 60001;                                   // fresh 60s rate window
    coldStartSetProcIdentityForTest("com.test.game", 100, 111u);  // identity UNCHANGED
    s_clk += 2000;                                    // away >1s, process alive
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "B: same-instance return -> armed");
    CHECK(coldStartIsColdForTest() == false, "B: SAME live instance classified WARM (no full 15-30s)");
    // Decisive: at age=6000ms a WARM window must already have released (cap 5000ms).
    // A mis-classified cold window would still be active (<15000ms min).
    s_clk += 6000;
    coldStartTick();
    CHECK(coldStartActive() == false, "B: at age=6000ms a WARM window has already released (<=5s)");
    coldStartOnForegroundSwitch("com.other.app");

    // ---------- F. sub-1s rapid switch: debounce suppresses re-arm ------------
    s_clk += 60001;                                   // fresh rate window
    coldStartSetProcIdentityForTest("com.test.game", 100, 111u);
    coldStartOnForegroundSwitch("com.test.game");      // arm (warm; same instance)
    CHECK(coldStartActive() == true, "F: arm -> active");
    coldStartOnForegroundSwitch("com.other.app");      // disarm (records disarm time)
    CHECK(coldStartActive() == false, "F: disarmed");
    s_clk += 500;                                     // away only 500ms (<1s)
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == false, "F: <1s away-and-back is debounced (no re-arm)");
    s_clk += 1000;                                    // now 1500ms after disarm (>1s)
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "F: after debounce window, return -> armed(warm)");
    CHECK(coldStartIsColdForTest() == false, "F: still same instance -> warm");
    coldStartOnForegroundSwitch("com.other.app");

    // ---------- C. process restart: SAME pid, NEW start_time -------------------
    s_clk += 60001;                                   // fresh rate window
    coldStartSetProcIdentityForTest("com.test.game", 100, 222u);  // pid same, start_time changed
    s_clk += 2000;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "C: restart (pid same, new start_time) -> armed");
    CHECK(coldStartIsColdForTest() == true, "C: start_time changed => NEW instance => COLD");
    s_clk += 6000;
    coldStartTick();
    CHECK(coldStartActive() == true, "C: at age=6000ms cold window still active (>=15s)");
    coldStartOnForegroundSwitch("com.other.app");

    // ---------- D. process restart: NEW pid, start_time unchanged --------------
    s_clk += 60001;                                   // fresh rate window
    coldStartSetProcIdentityForTest("com.test.game", 200, 222u);  // pid changed
    s_clk += 2000;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "D: restart (new pid) -> armed");
    CHECK(coldStartIsColdForTest() == true, "D: pid changed (start_time same) => NEW instance => COLD");
    coldStartOnForegroundSwitch("com.other.app");

    // ---------- E. identity gone: process killed -> conservative cold ---------
    s_clk += 60001;                                   // fresh rate window
    coldStartSetProcIdentityForTest("com.test.game", 0, 0u);     // process gone
    s_clk += 2000;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "E: unresolvable identity -> armed (conservative cold)");
    CHECK(coldStartIsColdForTest() == true, "E: unresolvable identity => COLD (no regression)");
    coldStartOnForegroundSwitch("com.other.app");

    // ---------- G. rate cap counts warm arms (farm resistance) ---------------
    s_clk += 60001;                                   // fresh rate window
    long long base = coldStartRejectedCountForTest();
    // Arm #1 (cold: fresh instance)
    coldStartSetProcIdentityForTest("com.test.game", 300, 333u);
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true && coldStartIsColdForTest() == true, "G: arm1 cold");
    coldStartOnForegroundSwitch("com.other.app");
    // Arm #2 (warm: same instance back quickly)
    s_clk += 1500;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true && coldStartIsColdForTest() == false, "G: arm2 warm (same instance)");
    coldStartOnForegroundSwitch("com.other.app");
    // Arm #3 (warm again)
    s_clk += 1500;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "G: arm3 accepted (3rd within 60s)");
    coldStartOnForegroundSwitch("com.other.app");
    // Arm #4 within 60s -> REFUSED even though it would be a warm resume.
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
