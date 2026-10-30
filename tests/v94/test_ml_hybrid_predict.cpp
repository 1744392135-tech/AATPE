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
    printf("=== HybridPredict: disabled/uninitialized -> neutral ===\n");
    HybridLearner fresh;
    float f[15] = {0}; f[0] = 1.0f;
    CHECK(fresh.predict(0, f) == 0.5f, "disabled learner -> predict 0.5");
    CHECK(fresh.getFineTuneAdjustment(0, f) == 0.0f, "disabled -> adjustment 0");

    printf("=== HybridPredict: cold-start gate before 50 samples ===\n");
    fresh.init(2); fresh.setEnabled(true);
    std::vector<uint64_t> cpuRt(8, 0);
    uint64_t runNs = 1000, ts = 1000;
    for (int i = 0; i < 5; i++) {
        runNs += 1000; ts += 15000000;
        ThreadCpuSnapshot s; s.tid = 3; s.cpu = 0; s.pool = 0;
        s.runTimeNs = runNs; s.timestamp = ts; s.priority = 50; s.nice = 0; s.role = 3;
        cpuRt[0] = runNs;
        fresh.tick({s}, cpuRt, 8);
    }
    CHECK(fresh.isPoolReady(0) == false, "5 updates < 50 -> not ready");
    CHECK(fresh.getFineTuneAdjustment(0, f) == 0.0f, "cold-start -> adjustment 0");
    CHECK(fresh.predict(0, f) == 0.5f, "cold-start -> predict neutral 0.5");

    printf("=== HybridPredict: predict output stays in [0,1] ===\n");
    for (int i = 0; i < 20; i++) f[i % 15] = (float)i * 0.1f;
    float p = fresh.predict(0, f);
    CHECK(p >= 0.0f && p <= 1.0f, "predict() within [0,1]");

    printf("=== HybridPredict: out-of-range pool -> 0 / 0.5 ===\n");
    CHECK(fresh.getFineTuneAdjustment(99, f) == 0.0f, "bad pool -> adjustment 0");
    CHECK(std::fabs(fresh.predict(99, f) - 0.5f) < 1e-6f, "bad pool -> predict 0.5");

    printf("\n========================================\n"
           "HybridPredict results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
