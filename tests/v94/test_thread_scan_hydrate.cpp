#include <cstdio>
#include <unistd.h>
#include "aatped.h"
#include "aatped_types.h"

extern int threadScanRefreshForTest(ThreadInfo& ti, bool checkStarttime);

namespace {
constexpr int kRR_KEEP = 0;
constexpr int kRR_REUSE = 1;
}

static int g_fail = 0;
#define CHECK(c, m) do { if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); g_fail++; } } while(0)

int main() {
    printf("=== G4: CLONE_THREAD placeholder starttime hydration ===\n");

    {
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = getpid();
        ti.starttime = 0;
        ti.needsHydration = true;
        int r = threadScanRefreshForTest(ti, false);
        CHECK(r == kRR_REUSE, "placeholder (needsHydration) forces REUSE repopulate");
    }

    {
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = getpid();
        ti.starttime = 12345ULL;
        ti.needsHydration = false;
        int r = threadScanRefreshForTest(ti, false);
        CHECK(r != kRR_REUSE, "hydrated thread (cached starttime) not spuriously REUSE");
    }

    {
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = getpid();
        ti.starttime = 12345ULL;
        ti.needsHydration = false;
        int r = threadScanRefreshForTest(ti, true);
        CHECK(r != kRR_REUSE || ti.needsHydration == false,
              "hydrated thread not evicted on missing/zero starttime read");
    }

    {
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = getpid();
        ti.starttime = 0;
        ti.needsHydration = true;
        int r = threadScanRefreshForTest(ti, true);
        CHECK(r == kRR_REUSE, "placeholder hydration beats checkStarttime read");
    }

    {
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = 777777;
        ti.needsHydration = false;
        ti.starttime = 12345ULL;
        ti.idleRefreshes = 20;
        ti.longIdleRescan = 0;
        ti.switchesPerSec = 0.0;
        ti.nrSwitches = 1000;
        {
            std::lock_guard<std::mutex> lk(g_scoreCacheMutex);
            g_scoreCache[777777].lastSwitchMs = nowMs - 100;
        }
        int r = threadScanRefreshForTest(ti, false);
        CHECK(r == kRR_KEEP, "frozen thread refresh returns KEEP (schedstat unreadable)");
        CHECK(ti.idleRefreshes == 0,
              "fresh lastSwitchMs breaks skipSchedstat freeze: idleRefreshes reset to 0");
    }

    {
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = 888888;
        ti.needsHydration = false;
        ti.starttime = 12345ULL;
        ti.idleRefreshes = 20;
        ti.longIdleRescan = 0;
        ti.switchesPerSec = 0.0;
        ti.nrSwitches = 1000;
        {
            std::lock_guard<std::mutex> lk(g_scoreCacheMutex);
            g_scoreCache[888888].lastSwitchMs = nowMs - 60000;
        }
        int r = threadScanRefreshForTest(ti, false);
        CHECK(r == kRR_KEEP, "frozen thread with stale lastSwitchMs returns KEEP");
        CHECK(ti.idleRefreshes >= 6,
              "stale lastSwitchMs: freeze persists (idleRefreshes untouched)");
    }

    if (g_fail == 0) { printf("=== test_thread_scan_hydrate: ALL PASS ===\n"); return 0; }
    printf("=== test_thread_scan_hydrate: %d FAILURES ===\n", g_fail);
    return 1;
}
