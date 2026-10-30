#include <cstdio>
#include <cstdint>
#include <atomic>
#include <string>
#include <set>
#include <thread>
#include <vector>

namespace aatpe_spillover {
void setVmGuestPnamesForTest(const std::set<std::string>& pkgs);
std::set<std::string> resolveVmGuestPnames(const std::string& containerPkg);
}

static std::atomic<bool> g_stop{false};
static std::atomic<int> g_iterations{0};

void readerThread() {
    while (!g_stop.load()) {
        std::set<std::string> guests = aatpe_spillover::resolveVmGuestPnames("com.vmos.pro");
        (void)guests;
        g_iterations++;
    }
}

int main() {
    printf("=== R1-1: g_vmGuestCache concurrent access (TSan) ===\n");

    aatpe_spillover::setVmGuestPnamesForTest({"org.cocos2dx.zmxy.mi", "com.tencent.sgame"});

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back(readerThread);
    }

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
