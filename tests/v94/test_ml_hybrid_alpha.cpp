// test_ml_hybrid_alpha.cpp - v94 hermetic unit tests for aatpe_ml_hy_alpha.cpp
// Links all 5 hybrid TUs. Focus: getAlpha() ramp schedule — 0 until the
// 50-sample cold-start gate, then linear ramp to kHybridMaxAlpha (0.1),
// clamped at 1.0, and forced to 0 when disabled.
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
    printf("=== HybridAlpha: disabled -> alpha 0 ===\n");
    HybridLearner L;
    L.init(1); L.setEnabled(false);
    CHECK(L.getAlpha() == 0.0f, "disabled learner -> alpha 0");

    printf("=== HybridAlpha: alpha 0 before cold-start gate ===\n");
    L.setEnabled(true);
    std::vector<uint64_t> cpuRt(8, 0);
    uint64_t runNs = 1000, ts = 1000;
    for (int i = 0; i < 10; i++) {
        runNs += 1000; ts += 15000000;
        ThreadCpuSnapshot s; s.tid = 4; s.cpu = 0; s.pool = 0;
        s.runTimeNs = runNs; s.timestamp = ts; s.priority = 50; s.nice = 0; s.role = 3;
        cpuRt[0] = runNs;
        L.tick({s}, cpuRt, 8);
    }
    CHECK(L.getAlpha() == 0.0f, "10 updates (<50) -> alpha still 0");

    printf("=== HybridAlpha: alpha ramps after cold-start ===\n");
    for (int i = 0; i < 60; i++) {
        runNs += 1000; ts += 15000000;
        ThreadCpuSnapshot s; s.tid = 4; s.cpu = 0; s.pool = 0;
        s.runTimeNs = runNs; s.timestamp = ts; s.priority = 50; s.nice = 0; s.role = 3;
        cpuRt[0] = runNs;
        L.tick({s}, cpuRt, 8);
    }
    float a = L.getAlpha();
    CHECK(a > 0.0f, "after >=50 updates -> alpha > 0");
    CHECK(a <= 0.1f + 1e-6f, "alpha never exceeds max 0.1");
    CHECK(a > 0.0f && a < 0.1f, "early ramp phase: 0 < alpha < 0.1");

    printf("=== HybridAlpha: reset drops alpha back to 0 ===\n");
    L.reset();
    CHECK(L.getAlpha() == 0.0f, "reset -> alpha 0");

    printf("\n========================================\n"
           "HybridAlpha results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
