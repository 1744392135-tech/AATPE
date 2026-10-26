// AATPE - Hybrid Learning Unit Test (host-side)
// [LEGACY-TEST v66-v74] 历史遗留测试：部分金值断言(eBPF 字节码偏移/map 数量/文件 md5/硬编码版本号)可能已过时；其失败不一定代表生产 bug。
// 见 tests/README.md 中"测试失败分类指南"。请勿删除(有考古价值)。
// Tests: init/enable/disable/tick/save/load/fine-tune adjustment
#include "aatpe_ml_hybrid.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

using namespace aatpe_ml_hybrid;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); g_pass++; } \
    else { printf("  [FAIL] %s\n", msg); g_fail++; } \
} while(0)

int main() {
    printf("=== AATPE Hybrid Learning Unit Test ===\n\n");

    // Test 1: Init
    printf("=== Test: Init ===\n");
    HybridLearner hl;
    hl.init(3);
    CHECK(hl.getPoolCount() == 3, "init with 3 pools");
    CHECK(hl.isEnabled() == false, "default disabled after init");
    CHECK(hl.getTotalUpdates() == 0, "zero updates after init");

    // Test 2: Enable/Disable
    printf("\n=== Test: Enable/Disable ===\n");
    hl.setEnabled(true);
    CHECK(hl.isEnabled() == true, "enable works");
    hl.setEnabled(false);
    CHECK(hl.isEnabled() == false, "disable works");
    hl.setEnabled(true);

    // Test 3: Tick with snapshots
    printf("\n=== Test: Tick (SGD updates) ===\n");
    std::vector<ThreadCpuSnapshot> snaps;
    for (int i = 0; i < 10; i++) {
        ThreadCpuSnapshot s;
        s.tid = 1000 + i;
        s.cpu = i % 8;
        s.pool = (i % 8 >= 7) ? 2 : ((i % 8 >= 4) ? 1 : 0);
        s.runTimeNs = 1000000ULL * (i + 1);
        s.timestamp = 1000000000ULL;
        snaps.push_back(s);
    }
    std::vector<uint64_t> cpuRt(8, 500000000ULL);
    hl.tick(snaps, cpuRt, 8);
    // First tick: no previous snapshots, so no deltas, no updates
    CHECK(hl.getTotalUpdates() == 0, "first tick has no deltas (cold start)");

    // Second tick with increased runtimes -> deltas exist
    for (auto& s : snaps) {
        s.runTimeNs += 500000ULL;  // 0.5ms more
        s.timestamp += 500000000ULL;  // 500ms later
    }
    hl.tick(snaps, cpuRt, 8);
    CHECK(hl.getTotalUpdates() > 0, "second tick produces SGD updates");
    printf("  [INFO] total updates after 2 ticks: %d\n", hl.getTotalUpdates());

    // Test 4: Fine-tune adjustment
    printf("\n=== Test: Fine-tune adjustment ===\n");
    float feat[15] = {1, 0.5f, 0.3f, 0.7f, 0.5f, 0.5f, 0, 0.5f, 0.8f, 1, 0, 0.4f, 0.35f, 0.4f, 0.25f};
    float adj = hl.getFineTuneAdjustment(0, feat);
    CHECK(std::isfinite(adj), "adjustment is finite");
    CHECK(adj >= -1.0f && adj <= 1.0f, "adjustment in [-1, 1]");
    printf("  [INFO] adjustment pool0: %.4f\n", adj);

    // Test 5: Disabled returns zero
    printf("\n=== Test: Disabled zero overhead ===\n");
    hl.setEnabled(false);
    float adjDisabled = hl.getFineTuneAdjustment(0, feat);
    CHECK(adjDisabled == 0.0f, "disabled returns 0 adjustment");
    // Tick when disabled should be no-op
    int updatesBefore = hl.getTotalUpdates();
    hl.tick(snaps, cpuRt, 8);
    CHECK(hl.getTotalUpdates() == updatesBefore, "tick when disabled is no-op");

    // Test 6: Save/Load
    printf("\n=== Test: Save/Load ===\n");
    hl.setEnabled(true);
    const char* testPath = "/tmp/aatpe_hybrid_test.dat";
    bool saved = hl.save(testPath);
    CHECK(saved, "save succeeds");

    HybridLearner hl2;
    hl2.init(3);
    bool loaded = hl2.load(testPath);
    CHECK(loaded, "load succeeds");
    hl2.setEnabled(true);  // load only restores weights, enable separately
    CHECK(hl2.getTotalUpdates() == hl.getTotalUpdates(), "loaded updates match");
    float adj2 = hl2.getFineTuneAdjustment(0, feat);
    CHECK(std::abs(adj2 - adj) < 0.001f, "loaded model produces same adjustment");
    remove(testPath);

    // Test 7: Reset
    printf("\n=== Test: Reset ===\n");
    hl.reset();
    CHECK(hl.getTotalUpdates() == 0, "reset clears updates");
    float adjReset = hl.getFineTuneAdjustment(0, feat);
    CHECK(adjReset == 0.0f, "reset zeroes adjustments");

    // Test 8: Pool delta diagnostics
    printf("\n=== Test: Pool delta diagnostics ===\n");
    HybridPoolDelta pd = hl.getPoolDelta(0);
    CHECK(pd.updateCount >= 0, "pool delta has valid update count");
    bool allFinite = true;
    for (int i = 0; i < 15; i++) {
        if (!std::isfinite(pd.weights[i])) { allFinite = false; break; }
    }
    CHECK(allFinite, "all pool delta weights are finite");

    // Summary
    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("SOME TESTS FAILED\n");
    return 1;
}
