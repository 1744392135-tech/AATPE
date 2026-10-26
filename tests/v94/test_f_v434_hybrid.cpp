// test_f_v434_hybrid.cpp — v4.3.4 F-group regression for F-002:
//   HybridLearner::tick() must pause SGD when g_mlTrainingPausedByThermal is
//   latched (mirroring LearningModel::update() SKIP_THERMAL). Old code had no
//   such gate, so under SEVERE thermal throttling the hybrid kept training on
//   the thermally-distorted runRatio signal.
//
// Links all 5 hybrid TUs + log_stub + hybrid_deps (which provides the weak
// aatpe_ml::g_mlTrainingPausedByThermal symbol for host tests).
#include <cstdio>
#include <vector>
#include "aatpe_ml_hybrid.h"
#include "aatpe_ml.h"   // for aatpe_ml::g_mlTrainingPausedByThermal

using namespace aatpe_ml_hybrid;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static std::vector<ThreadCpuSnapshot> snap(int tid, int cpu, int pool,
                                           uint64_t runNs, uint64_t ts) {
    ThreadCpuSnapshot s; s.tid = tid; s.cpu = cpu; s.pool = pool;
    s.runTimeNs = runNs; s.timestamp = ts; s.priority = 50; s.nice = 0; s.role = 3;
    s.nrSwitches = 0;
    return {s};
}

int main() {
    printf("=== F-002: hybrid tick() pauses under g_mlTrainingPausedByThermal ===\n");

    // Use a fresh (non-singleton) HybridLearner to avoid cross-test contamination.
    HybridLearner L;
    L.init(3);
    L.setEnabled(true);

    std::vector<uint64_t> cpuRt(8, 0);

    // Prime: first tick builds prevSnapshots but produces no update.
    L.tick(snap(100, 6, 2, 1000, 1000), cpuRt, 8);
    CHECK(L.getTotalUpdates() == 0, "pre: first tick (no prev) -> 0 updates");

    // Second tick (no thermal pause): should produce 1 update.
    cpuRt[6] = 500000;
    L.tick(snap(100, 6, 2, 2000, 15000000), cpuRt, 8);
    int baseline = L.getTotalUpdates();
    CHECK(baseline == 1, "pre: second tick -> 1 update");

    // --- Enable thermal pause gate ---
    aatpe_ml::g_mlTrainingPausedByThermal.store(true, std::memory_order_relaxed);

    // Advance CPU runtime and timestamp; if tick() were not gated, this would
    // produce another update. With the gate, it must NOT.
    cpuRt[6] = 1000000;
    L.tick(snap(100, 6, 2, 3000, 30000000), cpuRt, 8);
    CHECK(L.getTotalUpdates() == baseline,
          "F-002: thermal pause -> totalUpdates unchanged (no SGD)");

    // A second paused tick also must not update.
    cpuRt[6] = 1500000;
    L.tick(snap(100, 6, 2, 4000, 45000000), cpuRt, 8);
    CHECK(L.getTotalUpdates() == baseline,
          "F-002: second paused tick -> still no update");

    // --- Clear thermal pause; training must resume ---
    aatpe_ml::g_mlTrainingPausedByThermal.store(false, std::memory_order_relaxed);

    cpuRt[6] = 2000000;
    L.tick(snap(100, 6, 2, 5000, 60000000), cpuRt, 8);
    CHECK(L.getTotalUpdates() == baseline + 1,
          "F-002: thermal cleared -> training resumes (+1 update)");

    // Clean up: reset the global flag for any subsequent test in the same process.
    aatpe_ml::g_mlTrainingPausedByThermal.store(false, std::memory_order_relaxed);

    printf("\n========================================\n");
    printf("F v4.3.4 Hybrid results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}
