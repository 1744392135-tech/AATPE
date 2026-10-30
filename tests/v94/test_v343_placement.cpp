#include <cstdio>
#include <thread>
#include <chrono>
#include "aatped.h"
#include "aatped_types.h"

using namespace aatpe;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

extern bool placementHysteresisGate(int tid, int currentCpu, int bestCpu,
                                    int currentHeuristic, int bestHeuristic,
                                    const Config& cfg, Decision& d);
extern void placementRecordMigration(int tid, int cpu, int heuristicScore);
extern void placementInvalidateSettle(int tid);
extern long long placementGetAvgSettleDwellMs();

static Config makeCfg(int hystPct, int cooldownMs) {
    Config c;
    c.migrationHysteresisPercent = hystPct;
    c.migrationCooldownMs = cooldownMs;
    return c;
}

int main() {
    Config cfg = makeCfg(5, 500);

    printf("=== #34 first observation anchors to current CPU ===\n");
    {
        Decision d;
        int tid = 1001;
        placementInvalidateSettle(tid);
        bool reject = placementHysteresisGate(tid, 4, 5, 100, 103, cfg, d);
        CHECK(!reject, "first observation does not reject (anchors to cpu4)");
    }

    printf("=== #82 no A/B/A/B flapping between near-tied CPUs ===\n");
    {
        int tid = 1002;
        placementInvalidateSettle(tid);
        Decision d;
        placementHysteresisGate(tid, 4, 5, 100, 103, cfg, d);
        int rejects = 0;
        for (int i = 0; i < 20; i++) {
            int bestScore = 100 + (i % 3) + 1;
            Decision dd;
            if (placementHysteresisGate(tid, 4, 5, 100, bestScore, cfg, dd)) rejects++;
        }
        CHECK(rejects == 20, "all 20 sub-5% wobbles rejected (no flapping)");
    }

    printf("=== #34 large improvement crosses the band ===\n");
    {
        int tid = 1003;
        placementInvalidateSettle(tid);
        Decision d;
        placementHysteresisGate(tid, 4, 5, 100, 104, cfg, d);
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
        bool reject = placementHysteresisGate(tid, 4, 5, 100, 110, cfg, d);
        CHECK(!reject, "10% improvement crosses 5% band -> migrate allowed");
    }

    printf("=== #34 cooldown blocks immediate re-migration ===\n");
    {
        int tid = 1004;
        placementInvalidateSettle(tid);
        Decision d;
        placementHysteresisGate(tid, 4, 5, 100, 120, cfg, d);
        placementRecordMigration(tid, 5, 120);
        bool reject = placementHysteresisGate(tid, 5, 4, 120, 200, cfg, d);
        CHECK(reject, "re-migration within cooldown window blocked");
        CHECK(std::string(d.reason).find("SETTLE_COOLDOWN") == 0, "reason is SETTLE_COOLDOWN");
    }

    printf("=== #91/#92 invalidate resets state (tid reuse) ===\n");
    {
        int tid = 1005;
        placementInvalidateSettle(tid);
        Decision d;
        placementHysteresisGate(tid, 0, 1, 50, 60, cfg, d);
        placementInvalidateSettle(tid);
        bool reject = placementHysteresisGate(tid, 0, 1, 50, 60, cfg, d);
        CHECK(!reject, "after invalidate the gate re-anchors fresh");
    }

    printf("=== #91/#92 avg dwell is non-negative ===\n");
    {
        long long dwell = placementGetAvgSettleDwellMs();
        CHECK(dwell >= 0, "avg settle dwell >= 0");
    }

    printf("=== #93 hyst=0 disables band (back-compat) ===\n");
    {
        Config none = makeCfg(0, 0);
        int tid = 1006;
        placementInvalidateSettle(tid);
        Decision d;
        placementHysteresisGate(tid, 4, 5, 100, 101, none, d);
        bool reject = placementHysteresisGate(tid, 4, 5, 100, 101, none, d);
        CHECK(!reject, "hyst=0 -> band disabled, micro-gain allowed (back-compat)");
    }

    printf("\n========================================\n"
           "v3.4.3 placement results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
