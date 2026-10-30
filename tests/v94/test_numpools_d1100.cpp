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
    printf("=== Task2: D1100 sparse-tier (0/2/3) must use distinct-cluster numPools=3 ===\n");

    const int kDistinctClusters = 3;

    CHECK_EQ(poolFeatureForTier(0, kDistinctClusters), 0.3f, "D1100 LITTLE (tier0) -> 0.3");
    CHECK_EQ(poolFeatureForTier(2, kDistinctClusters), 1.0f, "D1100 BIG    (tier2) -> 1.0");
    CHECK_EQ(poolFeatureForTier(3, kDistinctClusters), 0.3f, "D1100 PRIME  (tier3) -> 0.3 (else, matches v4.1.0)");

    printf("--- regression guard: OLD behaviour (numPools=maxTier+1=4) is what broke it ---\n");
    float wrongLittle = poolFeatureForTier(0, 4);
    float wrongBig    = poolFeatureForTier(2, 4);
    float wrongPrime  = poolFeatureForTier(3, 4);
    CHECK_EQ(wrongLittle, 0.0f, "old numPools=4 LITTLE would be 0.0 (WRONG)");
    CHECK_EQ(wrongBig,    2.0f/3.0f, "old numPools=4 BIG would be 0.667 (WRONG)");
    CHECK_EQ(wrongPrime,  1.0f, "old numPools=4 PRIME would be 1.0 (WRONG)");
    if (std::abs(poolFeatureForTier(0,3) - wrongLittle) > 1e-6f) { g_pass++; printf("  [PASS] LITTLE fixed away from 0.0\n"); }
    else { g_fail++; printf("  [FAIL] LITTLE not fixed\n"); }
    if (std::abs(poolFeatureForTier(2,3) - wrongBig) > 1e-6f) { g_pass++; printf("  [PASS] BIG fixed away from 0.667\n"); }
    else { g_fail++; printf("  [FAIL] BIG not fixed\n"); }

    printf("--- a genuine 4-cluster box STILL uses the linear branch (F3 preserved) ---\n");
    CHECK_EQ(poolFeatureForTier(0, 4), 0.0f, "4-cluster LITTLE -> 0.0 (linear)");
    CHECK_EQ(poolFeatureForTier(3, 4), 1.0f, "4-cluster PRIME -> 1.0 (linear)");

    printf("\n========================================\n"
           "D1100 numPools: %d passed, %d failed\n========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
