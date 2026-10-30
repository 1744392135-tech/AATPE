#include <cstdio>
#include <cmath>
#include "aatpe_ml_hybrid.h"

using namespace aatpe_ml_hybrid;

static int g_pass = 0, g_fail = 0;
#define CHECK_EQ(a, b, m) do { \
    if (std::abs((a) - (b)) < 1e-6f) { g_pass++; printf("  [PASS] %s (%.3f)\n", m, (double)(a)); } \
    else { g_fail++; printf("  [FAIL] %s (got %.3f, want %.3f)\n", m, (double)(a), (double)(b)); } \
} while(0)

int main() {
    printf("=== #12 poolFeatureForTier tests ===\n");

    printf("--- 3-tier (numPools=3): preserve old values ---\n");
    CHECK_EQ(poolFeatureForTier(0, 3), 0.3f, "3-tier pool 0 -> 0.3");
    CHECK_EQ(poolFeatureForTier(1, 3), 0.6f, "3-tier pool 1 -> 0.6");
    CHECK_EQ(poolFeatureForTier(2, 3), 1.0f, "3-tier pool 2 -> 1.0");

    printf("--- 1-tier (numPools=1): old formula preserved ---\n");
    CHECK_EQ(poolFeatureForTier(0, 1), 0.3f, "1-tier pool 0 -> 0.3 (old formula, tier<=3)");

    printf("--- 2-tier (numPools=2): old formula ---\n");
    CHECK_EQ(poolFeatureForTier(0, 2), 0.3f, "2-tier pool 0 -> 0.3");
    CHECK_EQ(poolFeatureForTier(1, 2), 0.6f, "2-tier pool 1 -> 0.6");

    printf("--- 4-tier (numPools=4): linear, PRIME=1.0 ---\n");
    CHECK_EQ(poolFeatureForTier(0, 4), 0.0f, "4-tier pool 0 (LITTLE) -> 0.0");
    CHECK_EQ(poolFeatureForTier(1, 4), 1.0f/3.0f, "4-tier pool 1 (MID) -> 0.333");
    CHECK_EQ(poolFeatureForTier(2, 4), 2.0f/3.0f, "4-tier pool 2 (BIG) -> 0.667");
    CHECK_EQ(poolFeatureForTier(3, 4), 1.0f, "4-tier pool 3 (PRIME) -> 1.0");

    printf("--- 4-tier monotonicity ---\n");
    bool mono = true;
    float prev = -1.0f;
    for (int p = 0; p < 4; p++) {
        float v = poolFeatureForTier(p, 4);
        if (v < prev) mono = false;
        prev = v;
    }
    if (mono) { g_pass++; printf("  [PASS] 4-tier features monotonic non-decreasing\n"); }
    else { g_fail++; printf("  [FAIL] 4-tier features not monotonic\n"); }

    printf("--- 5-tier (numPools=5): linear ---\n");
    CHECK_EQ(poolFeatureForTier(0, 5), 0.0f, "5-tier pool 0 -> 0.0");
    CHECK_EQ(poolFeatureForTier(4, 5), 1.0f, "5-tier pool 4 (top) -> 1.0");

    printf("--- out-of-range clamping ---\n");
    CHECK_EQ(poolFeatureForTier(99, 4), 1.0f, "pool 99 on 4-tier -> clamped to 1.0");
    CHECK_EQ(poolFeatureForTier(-1, 4), 0.0f, "pool -1 on 4-tier -> clamped to 0.0");

    printf("\n========================================\n"
           "ml pool feature: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
