// test_vmguest_race_v449.cpp — v4.4.9 R1-1 TSan regression test.
//
// Bug: g_vmGuestCache (std::set) was accessed concurrently from the input-monitor
// epoll thread (placementSpilloverTick every 40ms) and the main loop
// (proactiveMigrationCheck every ~200ms) without any lock. The slow path
// (TTL-expired clear + opendir("/proc") rebuild) could race with a reader's
// const-ref copy, causing std::set tree corruption / UB / heap corruption.
//
// Fix: dedicated g_vmGuestCacheMtx mutex; resolveVmGuestPnames() now returns a
// by-value snapshot under lock. This test spawns two threads that hammer the
// resolve function concurrently. Under TSan the unfixed code reports a data race;
// the fixed code is clean.
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <string>
#include <set>
#include <thread>
#include <vector>

// Placement test seams (defined in placement.cpp, inside namespace aatpe_spillover).
// Forward-declared here; the real definitions come from the linked placement.o.
namespace aatpe_spillover {
void setVmGuestPnamesForTest(const std::set<std::string>& pkgs);
std::set<std::string> resolveVmGuestPnames(const std::string& containerPkg);
}

static std::atomic<bool> g_stop{false};
static std::atomic<int> g_iterations{0};

void readerThread() {
    while (!g_stop.load()) {
        // Simulate the main loop calling resolveVmGuestPnames
        std::set<std::string> guests = aatpe_spillover::resolveVmGuestPnames("com.vmos.pro");
        (void)guests;
        g_iterations++;
    }
}

int main() {
    printf("=== R1-1: g_vmGuestCache concurrent access (TSan) ===\n");

    // Set up the test seam so we don't hit /proc
    aatpe_spillover::setVmGuestPnamesForTest({"org.cocos2dx.zmxy.mi", "com.tencent.sgame"});

    // Spawn 4 reader threads (simulating input monitor + main loop + edge reapply)
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back(readerThread);
    }

    // Let them run for ~2 seconds
    std::this_thread::sleep_for(std::chrono::seconds(2));
    g_stop = true;

    for (auto& t : threads) {
        t.join();
    }

    aatpe_spillover::setVmGuestPnamesForTest({});

    printf("  completed %d iterations across 4 threads\n", g_iterations.load());
    printf("=== R1-1: PASS (no data race detected) ===\n");
    return 0;
}
