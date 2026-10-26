// test_fed_aggregate.cpp - v94 hermetic unit tests for aatpe_federated_aggregate.cpp
// Links all 4 federated TUs. Focus: aggregateModels sample-count weighted average
// (including dim mismatch padding) and runAggregation() gating (local immature
// exclusion, trusted-only averaging, no-trusted fallback to local).
#include <cstdio>
#include <cmath>
#include <vector>
#include "aatpe_federated.h"

using namespace aatpe_federated;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== FedAggregate: weighted average ===\n");
    // model A: all 10 (count 1), model B: all 20 (count 3) -> 0.25*10+0.75*20 = 17.5
    std::vector<float> r = aggregateModels({{10.f, 10.f}, {20.f, 20.f}}, {1, 3});
    CHECK(r.size() == 2, "result length = max len");
    CHECK(std::fabs(r[0] - 17.5f) < 0.01f, "weighted average 17.5");

    printf("=== FedAggregate: empty input -> empty output ===\n");
    std::vector<float> e = aggregateModels({}, {});
    CHECK(e.empty(), "empty models -> empty result");

    printf("=== FedAggregate: dim mismatch padded ===\n");
    std::vector<float> p = aggregateModels({{1.f, 2.f}, {10.f}}, {1, 1});
    CHECK(p.size() == 2, "max length drives result size");
    CHECK(p[1] == 2.0f, "short second model contributes only its dims");

    printf("=== FedAggregate: runAggregation with no imports is a no-op ===\n");
    FederatedLearner fl;
    fl.init("/tmp/aatpe_fed_agg");
    fl.setLocalModel({1.f, 2.f}, 0.5f, 5000);
    fl.runAggregation();
    CHECK(fl.getStats().rounds == 0, "no imported models -> no round");

    // ---- G7-004: per-contributor sample-weight cap (kMaxContributorWeight=100000). ----
    printf("=== FedAggregate G7-004: huge-sample contributor is capped ===\n");
    // local=0 (count 1), peer=10 (count 1e6). If uncapped the peer would pull the
    // result to ~9.99999; with the 100k cap it must behave exactly as if the peer
    // count were 100000.
    std::vector<float> capped  = aggregateModels({{0.f}, {10.f}}, {1, 1000000});
    std::vector<float> uncappedEquiv = aggregateModels({{0.f}, {10.f}}, {1, 100000});
    CHECK(std::fabs(capped[0] - uncappedEquiv[0]) < 1e-4f,
          "1e6-sample contributor treated as 100k (capped)");
    // Pullback: local=0 with 100k samples vs peer=10 with 1e6 samples. Without the
    // cap the peer dominates -> ~9.09; with the 100k cap both weigh 100k -> 5.0.
    std::vector<float> pull = aggregateModels({{0.f}, {10.f}}, {100000, 1000000});
    CHECK(std::fabs(pull[0] - 5.0f) < 0.01f,
          "capped contributors equalized -> 5.0 (pulled back from uncapped ~9.09)");

    printf("\n========================================\n"
           "FedAggregate results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
