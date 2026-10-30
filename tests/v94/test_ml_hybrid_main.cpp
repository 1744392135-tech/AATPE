#include <cstdio>
#include <vector>
#include "aatpe_ml_hybrid.h"

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
    return {s};
}

int main() {
    printf("=== HybridMain: instance() singleton + disabled by default ===\n");
    HybridLearner& L = instance();
    L.reset();
    CHECK(L.isEnabled() == false, "learner disabled by default");
    CHECK(L.getPoolCount() == 0, "no pools before init");

    printf("=== HybridMain: init sets pool count ===\n");
    L.init(3);
    L.setEnabled(true);
    CHECK(L.getPoolCount() == 3, "init(3) -> 3 pools");
    CHECK(L.isEnabled() == true, "setEnabled(true)");

    printf("=== HybridMain: tick accumulates updates across two snapshots ===\n");
    std::vector<uint64_t> cpuRt(8, 0);
    L.tick(snap(100, 6, 2, 1000, 1000), cpuRt, 8);
    CHECK(L.getTotalUpdates() == 0, "first tick: no prev snapshot -> 0 updates");
    cpuRt[6] = 500000;
    L.tick(snap(100, 6, 2, 2000, 15000000), cpuRt, 8);
    CHECK(L.getTotalUpdates() == 1, "second tick with prev snapshot -> 1 update");

    printf("=== HybridMain: getPoolDelta returns delta for valid pool ===\n");
    HybridPoolDelta d = L.getPoolDelta(2);
    CHECK(d.updateCount >= 1, "pool 2 has an update recorded");
    HybridPoolDelta bad = L.getPoolDelta(99);
    CHECK(bad.updateCount == 0, "out-of-range pool -> empty delta");

    printf("=== HybridMain: getStatusString is non-empty ===\n");
    std::string st = L.getStatusString();
    CHECK(st.find("hybrid=") != std::string::npos, "status string describes learner");
    CHECK(st.find("pools=3") != std::string::npos, "status reports 3 pools");

    printf("=== HybridMain: reset clears updates ===\n");
    L.reset();
    CHECK(L.getTotalUpdates() == 0, "reset zeroes total updates");
    CHECK(L.getPoolCount() == 3, "reset keeps pool count");

    printf("=== HybridMain: disabled tick is a no-op ===\n");
    L.setEnabled(false);
    int before = L.getTotalUpdates();
    cpuRt[6] = 600000;
    L.tick(snap(100, 6, 2, 3000, 30000000), cpuRt, 8);
    CHECK(L.getTotalUpdates() == before, "disabled -> no updates");
    L.setEnabled(true);

    printf("=== ML-03: training sees BOTH current_cpu_match branches ===\n");
    L.reset();
    {
        std::vector<uint64_t> rt(8, 0);
        L.tick(snap(94, 6, 2, 1000, 1000), rt, 8);
        rt[6] = 500000;
        L.tick(snap(94, 6, 2, 2000, 15000000), rt, 8);
        HybridPoolDelta d0 = L.getPoolDelta(2);
        CHECK(d0.updateCount == 1, "ML-03: dropped-f9 sample trained the other weights");
        CHECK(d0.weights[9] == 0.0f, "ML-03: f9=0 sample -> w9 left exactly 0");
    }
    L.reset();
    {
        std::vector<uint64_t> rt(8, 0);
        L.tick(snap(100, 6, 2, 1000, 1000), rt, 8);
        rt[6] = 500000;
        L.tick(snap(100, 6, 2, 2000, 15000000), rt, 8);
        HybridPoolDelta d1 = L.getPoolDelta(2);
        CHECK(d1.updateCount == 1, "ML-03: f9=1 sample recorded");
        CHECK(d1.weights[9] != 0.0f, "ML-03: f9=1 sample -> w9 trained (in-distribution)");
    }

    printf("\n========================================\n"
           "HybridMain results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
