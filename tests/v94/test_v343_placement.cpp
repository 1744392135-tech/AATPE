// test_v343_placement.cpp — round-2 unit tests for placement:
//   #34 placementHysteresisGate: band + min-dwell cooldown; no A/B/A/B flapping
//   #82 scheduling oscillation: fixed-load fluctuating scores do not cause a
//       migration back-and-forth between two near-tied CPUs
//   #91/#92 settle-map lifecycle: record/invalidate keeps the map bounded
//   #93 upgrade compat: hysteresis gate accepts default config values
//
// Links placement.cpp + placement_deps.cpp (which provides g_cfg, snapshotConfig,
// topology globals, writeFile stubs). The hysteresis state lives in placement.cpp.
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

// Direct access to the hysteresis gate + bookkeeping defined in placement.cpp.
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

    // ------------------------------------------------------------------
    // #34 first observation anchors, does not reject
    // ------------------------------------------------------------------
    printf("=== #34 first observation anchors to current CPU ===\n");
    {
        Decision d;
        int tid = 1001;
        placementInvalidateSettle(tid);
        // First call: thread on cpu4, candidate cpu5 slightly better.
        bool reject = placementHysteresisGate(tid, 4, 5, 100, 103, cfg, d);
        CHECK(!reject, "first observation does not reject (anchors to cpu4)");
    }

    // ------------------------------------------------------------------
    // #82 oscillation: 3-point fluctuation between cpu4/cpu5 must NOT flap
    // ------------------------------------------------------------------
    printf("=== #82 no A/B/A/B flapping between near-tied CPUs ===\n");
    {
        int tid = 1002;
        placementInvalidateSettle(tid);
        Decision d;
        // anchor to cpu4, settled score 100
        placementHysteresisGate(tid, 4, 5, 100, 103, cfg, d); // anchor pass
        // Simulate 20 decision cycles where scores wobble by <=3% between
        // cpu4 (100) and cpu5 (101-103). With a 5% band these must all KEEP.
        int rejects = 0;
        for (int i = 0; i < 20; i++) {
            int bestScore = 100 + (i % 3) + 1;  // 101,102,103 wobble
            Decision dd;
            if (placementHysteresisGate(tid, 4, 5, 100, bestScore, cfg, dd)) rejects++;
        }
        CHECK(rejects == 20, "all 20 sub-5% wobbles rejected (no flapping)");
    }

    // ------------------------------------------------------------------
    // #34 a genuinely bigger improvement (>5%) DOES migrate
    // ------------------------------------------------------------------
    printf("=== #34 large improvement crosses the band ===\n");
    {
        int tid = 1003;
        placementInvalidateSettle(tid);
        Decision d;
        placementHysteresisGate(tid, 4, 5, 100, 104, cfg, d); // anchor to cpu4 @100
        // Wait out the 500ms cooldown so the only gate left is the band.
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
        bool reject = placementHysteresisGate(tid, 4, 5, 100, 110, cfg, d); // +10%
        CHECK(!reject, "10% improvement crosses 5% band -> migrate allowed");
    }

    // ------------------------------------------------------------------
    // #34 min-dwell cooldown blocks immediate re-migration
    // ------------------------------------------------------------------
    printf("=== #34 cooldown blocks immediate re-migration ===\n");
    {
        int tid = 1004;
        placementInvalidateSettle(tid);
        Decision d;
        placementHysteresisGate(tid, 4, 5, 100, 120, cfg, d); // anchor
        placementRecordMigration(tid, 5, 120);                 // just landed on 5
        // Immediately: candidate back to cpu4 with a big score. Must be blocked
        // by the 500ms cooldown even though the band would allow it.
        bool reject = placementHysteresisGate(tid, 5, 4, 120, 200, cfg, d);
        CHECK(reject, "re-migration within cooldown window blocked");
        CHECK(std::string(d.reason).find("SETTLE_COOLDOWN") == 0, "reason is SETTLE_COOLDOWN");
    }

    // ------------------------------------------------------------------
    // #91/#92 lifecycle: invalidate drops settle state (tid reuse safety)
    // ------------------------------------------------------------------
    printf("=== #91/#92 invalidate resets state (tid reuse) ===\n");
    {
        int tid = 1005;
        placementInvalidateSettle(tid);
        Decision d;
        placementHysteresisGate(tid, 0, 1, 50, 60, cfg, d); // anchor to 0
        placementInvalidateSettle(tid);
        // After invalidate, next call re-anchors (does NOT reject).
        bool reject = placementHysteresisGate(tid, 0, 1, 50, 60, cfg, d);
        CHECK(!reject, "after invalidate the gate re-anchors fresh");
    }

    printf("=== #91/#92 avg dwell is non-negative ===\n");
    {
        long long dwell = placementGetAvgSettleDwellMs();
        CHECK(dwell >= 0, "avg settle dwell >= 0");
    }

    // ------------------------------------------------------------------
    // #93 default config (hyst=0) disables the band -> no behavior change
    // ------------------------------------------------------------------
    printf("=== #93 hyst=0 disables band (back-compat) ===\n");
    {
        Config none = makeCfg(0, 0);
        int tid = 1006;
        placementInvalidateSettle(tid);
        Decision d;
        placementHysteresisGate(tid, 4, 5, 100, 101, none, d); // anchor
        bool reject = placementHysteresisGate(tid, 4, 5, 100, 101, none, d);
        CHECK(!reject, "hyst=0 -> band disabled, micro-gain allowed (back-compat)");
    }

    printf("\n========================================\n"
           "v3.4.3 placement results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
