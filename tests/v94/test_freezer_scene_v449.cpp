// test_freezer_scene_v449.cpp — R2-M1 regression: c4FreezerStateJson() must be a
// thread-safe snapshot of the slow-thread bookkeeping written by c4FreezerTick().
//
// This test deliberately compiles c4_freezer.cpp in PRODUCTION mode (i.e. WITHOUT
// -DAATPE_HOST_TEST) so the real tick/stateJson/statics are linked. All external
// deps of the production glue are stubbed here (test-only symbols; release nm
// never sees them). Two threads hammer tick() and StateJson() concurrently; under
// TSan the pre-fix code reports data races on the non-locked std::string bookkeep
// (s_targetPkg / s_lastAction / s_frozenCgDir). Post-fix (dedicated mutex) TSan is
// clean and StateJson always returns well-formed JSON.
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

#include "c4_freezer.h"
#include "aatpe_managers.h"

using namespace aatpe;
using namespace aatpe::c4freezer;

// ---- test-only stubs for the production glue's external deps ----------------
// (These override the real definitions; only linked into this test binary.)

// ForegroundManager: return an alternating package that crosses SSO so the
// bookkeeping strings churn heap across the race window.
static std::atomic<int> s_fgCounter{0};
namespace {
std::string g_fgA = "com.tencent.longgg.hellogame.0123456789";
std::string g_fgB = "short";
}
ForegroundManager& ForegroundManager::instance() {
    static ForegroundManager inst;
    return inst;
}
std::string ForegroundManager::getPackage() const {
    int c = s_fgCounter.fetch_add(1, std::memory_order_relaxed);
    return (c & 1) ? g_fgA : g_fgB;
}
void ForegroundManager::setPackage(const std::string&) {}
time_t ForegroundManager::startTime() const { return 0; }

// exec stub: empty -> media probe treats audio as "unknown" (do-not-freeze),
// uid resolution returns 0 (no real app cgroup found on host).
std::string aatpeExec(const std::string&, const char*) { return ""; }

// state-file stub: no persisted frozen record.
std::string readFile(const char*) { return ""; }
bool writeFileAtomic(const char*, const std::string&) { return true; }

// STATE_DIR root (v6.0.0: c4 state paths are now resolved under STATE_DIR; the
// production glue references this extern — host tests must supply it).
const char* STATE_DIR = "/tmp/aatpe_c4freezer_scene_state";

// gamelist stub: treat any foreground package as a manual game so the tick path
// exercises c4ChooseTarget/s_lastGame/s_targetPkg writes.
namespace aatpe { namespace gamelist {
bool isManualGame(const std::string&) { return true; }
}}

// v6.0.1: C4 now also consults the config game list (global namespace).
bool isPackageInGameList(const std::string&) { return false; }

// v6.0.1: this test does NOT define C4_FREEZER_TEST_SEAM, so the real
// fgHasGameEngineThreads() #else branch calls aatpe::isGameEngineThread.
// This test models the "manual list says game" path; engine threads are a
// no-op here (return false).
namespace aatpe {
bool isGameEngineThread(const char*, const char*) { return false; }
}

// ---- tiny CHECK harness -------------------------------------------------------
static int s_pass = 0, s_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

int main() {
    printf("=== R2-M1 c4FreezerStateJson vs c4FreezerTick concurrency ===\n");

    // Enable the freezer so tick actually runs the state machine and mutates the
    // exported bookkeeping strings.
    setC4FreezerEnabled(true);
    CHECK(c4FreezerEnabled(), "freezer enabled for the race test");

    std::atomic<bool> stop{false};

    // Writer thread: the slow freezer tick.
    std::thread ticker([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            c4FreezerTick();
        }
    });

    // Reader thread: the WebUI/control-thread JSON export.
    std::thread reader([&]() {
        std::string sink;
        while (!stop.load(std::memory_order_relaxed)) {
            sink = c4FreezerStateJson();
            // Touch the result so the compiler can't elide the read.
            if (sink.empty()) { printf("  [WARN] empty json\n"); }
        }
    });

    // Let them race for a bounded window (TSan makes this slow; keep it short).
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop.store(true, std::memory_order_relaxed);
    ticker.join();
    reader.join();

    // After the race, a fresh export must still be well-formed (no tear/corruption).
    std::string j = c4FreezerStateJson();
    printf("  snapshot: %s\n", j.c_str());
    CHECK(j.find("\"enabled\":true") != std::string::npos, "JSON reports enabled:true");
    CHECK(j.find("\"target\":") != std::string::npos, "JSON has target field");
    CHECK(j.find("\"frozen\":") != std::string::npos, "JSON has frozen field");
    CHECK(j.find("\"cgDir\":") != std::string::npos, "JSON has cgDir field");
    CHECK(j.find("\"lastAction\":") != std::string::npos, "JSON has lastAction field");
    CHECK(j.front() == '{' && j.back() == '}', "JSON is a closed object");

    // Sanity: tick still runs without crashing after the race window.
    c4FreezerTick();
    CHECK(true, "tick post-race did not crash");

    printf("=== R2-M1 result: pass=%d fail=%d ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
