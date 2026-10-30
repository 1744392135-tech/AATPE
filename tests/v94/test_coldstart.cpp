#include <cstdio>
#include <string>
#include <vector>
#include <atomic>
#include "aatped.h"
#include "cold_start_rules.h"

std::atomic<int> g_logLevel{3};
void logFlush() {}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

bool isPackageInGameList(const std::string& pkg) { return pkg == "com.test.game"; }

struct PrioCall { int tid; ThreadRole role; int level; };
static std::vector<PrioCall> g_prioCalls;

namespace aatpe_priority {
    bool aatpeSetThreadPriority(int tid, ThreadRole role, int level) {
        g_prioCalls.push_back({tid, role, level});
        return true;
    }
    std::vector<int> g_restoredTids;
    bool aatpeRestoreThreadPriority(int tid) {
        g_restoredTids.push_back(tid);
        return true;
    }
}

namespace aatpe {
    void coldStartSetNowForTest(long long (*fn)());
    void coldStartSetJankForTest(bool (*fn)());
    void coldStartSetDisplayedForTest(std::string (*fn)());
}
static long long s_clk = 1000000;
static long long clkFn() { return s_clk; }
static bool jankFn() { return false; }
static std::string displayedFn() { return std::string(""); }

int main() {
    using namespace aatpe;
    printf("=== coldstart: Displayed parse (pure) ===\n");
    CHECK(coldStartDisplayedPresent("") == false, "empty dumpsys -> not converged (fallback)");
    CHECK(coldStartDisplayedPresent("  mResumedActivity=ActivityRecord{}") == false,
          "no Displayed line -> window continues");
    CHECK(coldStartDisplayedPresent("Displayed com.game/.MainActivity: +1s234ms") == true,
          "Displayed line -> converged, hand off");
    CHECK(coldStartDisplayedPresent(" mDisplayed=0x1234 mCurrentFocus=...") == true,
          "non-zero mDisplayed timestamp -> converged");
    CHECK(coldStartDisplayedPresent(" mDisplayed=0x0 mCurrentFocus=...") == false,
          "mDisplayed=0 -> not yet rendered");

    printf("=== coldstart: gates present, no crash ===\n");
    CHECK(coldStartActive() == false, "initially not active");
    coldStartSetThermalFreeze(true);
    coldStartSetIoPressure(true);
    coldStartTick();
    CHECK(coldStartActive() == false, "frozen/io on inactive -> stays inactive");
    coldStartSetThermalFreeze(false);
    coldStartSetIoPressure(false);

    coldStartSetNowForTest(clkFn);
    coldStartSetJankForTest(jankFn);
    coldStartSetDisplayedForTest(displayedFn);

    printf("=== coldstart: F-8 arm rate limit + window recovery ===\n");
    s_clk = 1000000;

    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "F-8: arm 1 -> active");
    coldStartOnForegroundSwitch("com.not.game");
    CHECK(coldStartActive() == false, "F-8: disarm 1 -> inactive");
    s_clk += 1100;

    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "F-8: arm 2 -> active");
    coldStartOnForegroundSwitch("com.not.game");
    CHECK(coldStartActive() == false, "F-8: disarm 2 -> inactive");
    s_clk += 1100;

    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "F-8: arm 3 -> active");
    coldStartOnForegroundSwitch("com.not.game");
    CHECK(coldStartActive() == false, "F-8: disarm 3 -> inactive");
    s_clk += 1100;

    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == false, "F-8: arm 4 within 60s window -> refused");
    CHECK(coldStartRejectedCountForTest() == 1, "F-8: cold_start_rejected == 1 after 4th arm refused");

    s_clk += 60001;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "F-8: arm after 60s window -> active (history pruned)");
    CHECK(coldStartRejectedCountForTest() == 1, "F-8: accepted arm after window does not bump rejected");

    printf("=== F-11: convergence/elapse exit symmetrically releases RT ===\n");
    coldStartOnForegroundSwitch("com.other.app");
    g_threadsMutex.lock();
    g_threads.clear();
    ThreadInfo t50, t100, t999, t200;
    t50.tid = 50; t50.pid = 50; t50.pname = "com.test.game"; t50.tname = "main";
    t100.tid = 100; t100.pid = 50; t100.pname = "com.test.game"; t100.tname = "RenderThread";
    t999.tid = 999; t999.pid = 50; t999.pname = "com.test.game"; t999.tname = "olddead";
    t200.tid = 200; t200.pid = 9999; t200.pname = "com.other.app"; t200.tname = "BackgroundThread";
    g_threads[50] = t50;
    g_threads[100] = t100;
    g_threads[999] = t999;
    g_threads[200] = t200;
    g_threadsMutex.unlock();

    g_prioCalls.clear();
    aatpe_priority::g_restoredTids.clear();
    s_clk = 2000000;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "F-11: arm before release test -> active");

    coldStartTick();

    int lv50 = -1, lv100 = -1;
    for (auto& c : g_prioCalls) {
        if (c.tid == 50) lv50 = c.level;
        if (c.tid == 100) lv100 = c.level;
    }
    CHECK(lv50 == 3, "F-11: tick applied level=3 to main thread (tid 50)");
    CHECK(lv100 == 3, "F-11: tick applied level=3 to render thread (tid 100)");

    g_threadsMutex.lock();
    g_threads.erase(999);
    g_threadsMutex.unlock();

    g_prioCalls.clear();
    s_clk += 31000;
    coldStartTick();
    CHECK(coldStartActive() == false, "F-11: window elapsed -> disarmed (boost released)");

    bool rest50 = false, rest100 = false, noRelease999 = true, noRelease200 = true;
    for (int t : aatpe_priority::g_restoredTids) {
        if (t == 50) rest50 = true;
        if (t == 100) rest100 = true;
        if (t == 999) noRelease999 = false;
        if (t == 200) noRelease200 = false;
    }
    CHECK(rest50, "F-11: release restored main thread (tid 50) via ungated restore channel");
    CHECK(rest100, "F-11: release restored render thread (tid 100) via ungated restore channel");
    CHECK(g_prioCalls.empty(), "F-11: release no longer goes through level=0 setter");
    CHECK(noRelease999, "F-11: dead tid 999 NOT touched");
    CHECK(noRelease200, "F-11: other pkg tid 200 NOT touched");

    s_clk += 60001;
    coldStartOnForegroundSwitch("com.test.game");
    CHECK(coldStartActive() == true, "F-11: re-armed after window");
    g_prioCalls.clear();
    aatpe_priority::g_restoredTids.clear();
    coldStartOnForegroundSwitch("com.other.app");
    CHECK(coldStartActive() == false, "F-11: leave foreground -> disarmed");

    rest50 = false; rest100 = false;
    for (int t : aatpe_priority::g_restoredTids) {
        if (t == 50) rest50 = true;
        if (t == 100) rest100 = true;
    }
    CHECK(rest50, "F-11: disarm (leave fg) restored tid 50");
    CHECK(rest100, "F-11: disarm (leave fg) restored tid 100");

    g_threadsMutex.lock();
    g_threads.clear();
    g_threadsMutex.unlock();

    coldStartSetNowForTest(nullptr);

    printf("\n========================================\n"
           "ColdStart results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
