#include <cstdio>
#include <cmath>
#include <vector>
#include "aatpe_ml_hybrid.h"

using namespace aatpe_ml_hybrid;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== HybridTrain: repeated ticks raise updateCount ===\n");
    HybridLearner& L = instance();
    L.reset(); L.init(1); L.setEnabled(true);
    std::vector<uint64_t> cpuRt(8, 0);
    uint64_t runNs = 1000; uint64_t ts = 1000;
    for (int i = 0; i < 60; i++) {
        runNs += 1000; ts += 15000000;
        ThreadCpuSnapshot s; s.tid = 7; s.cpu = 0; s.pool = 0;
        s.runTimeNs = runNs; s.timestamp = ts; s.priority = 50; s.nice = 0; s.role = 3;
        cpuRt[0] = runNs;
        L.tick({s}, cpuRt, 8);
    }
    CHECK(L.getTotalUpdates() == 59, "60 ticks (1 warmup) -> 59 updates");
    HybridPoolDelta d = L.getPoolDelta(0);
    CHECK(d.updateCount == 59, "pool 0 updateCount == 59");

    printf("=== HybridTrain: learned weights stay within clamp band (+/-0.5) ===\n");
    bool inBand = true;
    for (int i = 0; i < 15; i++) {
        if (d.weights[i] > 0.5f + 1e-6f || d.weights[i] < -0.5f - 1e-6f) inBand = false;
    }
    CHECK(inBand, "all 15 weights clamped to [-0.5,0.5]");

    printf("=== HybridTrain: a constant signal moves at least one weight off zero ===\n");
    bool nonzero = false;
    for (int i = 0; i < 15; i++) if (std::fabs(d.weights[i]) > 1e-6f) { nonzero = true; break; }
    CHECK(nonzero, "SGD adjusted at least one weight away from 0");

    printf("=== HybridTrain: pool reaches ready threshold (>=50) ===\n");
    CHECK(L.isPoolReady(0) == true, "pool 0 ready after 60 updates");
    CHECK(L.isPoolReady(3) == false, "out-of-range pool not ready");

    printf("\n========================================\n"
           "HybridTrain results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
