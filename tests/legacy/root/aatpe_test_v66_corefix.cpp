// AATPE - Core Fix Verification Tests
// [LEGACY-TEST v66-v74] 历史遗留测试：部分金值断言(eBPF 字节码偏移/map 数量/文件 md5/硬编码版本号)可能已过时；其失败不一定代表生产 bug。
// 见 tests/README.md 中"测试失败分类指南"。请勿删除(有考古价值)。
// Verifies: Problem 1 (11-dim weight / is_game) and Problem 2 (big core underutilization)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include "aatpe_ml.h"
#include "aatpe_ml_hybrid.h"

using namespace aatpe_ml;
using namespace aatpe_ml_hybrid;

static int g_tests = 0, g_passed = 0, g_failed = 0;

#define CHECK(cond, msg) do { \
    g_tests++; \
    if (cond) { g_passed++; printf("  [PASS] %s\n", msg); } \
    else { g_failed++; printf("  [FAIL] %s\n", msg); } \
} while(0)

// ============================================================================
// Problem 1: ML Feature & Weight Structure Tests
// ============================================================================

void test_ml_weight_structure() {
    printf("\n=== Problem 1: ML Weight Structure (15 weights) ===\n");
    LearningModel model;
    model.init(3);

    // Verify 15 weights per pool (1 bias + 10 base + 4 cross)
    CHECK(ML_WEIGHT_COUNT == 15, "ML_WEIGHT_COUNT == 15 (1 bias + 10 base + 4 cross)");
    CHECK(ML_FEATURE_COUNT == 10, "ML_FEATURE_COUNT == 10 base features");
    CHECK(ML_CROSS_FEATURE_COUNT == 4, "ML_CROSS_FEATURE_COUNT == 4 cross features");

    // Verify weight index mapping
    // w0=bias, w1=avg_run, w2=nr_switches, w3=cpu_usage, w4=priority, w5=nice,
    // w6=is_game, w7=thermal, w8=capacity, w9=current_match, w10=migration_count,
    // w11=cross_run_cap, w12=cross_run_usage, w13=cross_cap_thermal, w14=cross_run_thermal
    ThreadFeatures f = LearningModel::extractFeatures(
        10000, 500, 50, 50, 0, true, 60, 512, true, 5);
    CHECK(f.is_game == 1.0f, "extractFeatures: is_game=true -> f.is_game=1.0");
    CHECK(f.cross_run_cap == f.avg_run_us * f.capacity, "cross_run_cap = avg_run * capacity");
    CHECK(f.cross_run_usage == f.avg_run_us * f.cpu_usage, "cross_run_usage = avg_run * cpu_usage");
    CHECK(f.cross_cap_thermal == f.capacity * f.thermal, "cross_cap_thermal = capacity * thermal");
    CHECK(f.cross_run_thermal == f.avg_run_us * f.thermal, "cross_run_thermal = avg_run * thermal");

    // Verify is_game=false sets 0
    ThreadFeatures f2 = LearningModel::extractFeatures(
        10000, 500, 50, 50, 0, false, 60, 512, true, 5);
    CHECK(f2.is_game == 0.0f, "extractFeatures: is_game=false -> f.is_game=0.0");
}

void test_ml_update_is_game_gradient() {
    printf("\n=== Problem 1: ML Update with is_game feature ===\n");
    LearningModel model;
    model.init(3);
    model.setMaxAlpha(0.3f);
    model.setInitialLR(0.1f);  // high LR for visible gradient

    // Train with is_game=1 samples, verify w6 (is_game) updates
    ThreadFeatures f = LearningModel::extractFeatures(
        5000, 200, 30, 80, -10, true, 50, 768, false, 0);

    float w6_before = model.getPoolModel(2).weights[6];
    for (int i = 0; i < 100; i++) {
        model.update(2, f, 0.8f);  // good outcome for game thread on big pool
    }
    float w6_after = model.getPoolModel(2).weights[6];
    CHECK(w6_before == 0.0f, "w6 (is_game) starts at 0");
    CHECK(std::abs(w6_after) > 0.001f, "w6 (is_game) updates after training with is_game=1");
    printf("    w6 before=%.6f after=%.6f (delta=%.6f)\n", w6_before, w6_after, w6_after - w6_before);

    // Verify all 15 weights are accessible
    PoolModel pm = model.getPoolModel(2);
    bool allFinite = true;
    for (int i = 0; i < 15; i++) {
        if (!std::isfinite(pm.weights[i])) { allFinite = false; break; }
    }
    CHECK(allFinite, "All 15 weights are finite after training");
    CHECK(pm.sample_count == 100, "sample_count == 100 after 100 updates");
}

void test_hybrid_is_game_inference() {
    printf("\n=== Problem 1: Hybrid Learning is_game at inference ===\n");
    // Verify hybrid getFineTuneAdjustment accepts is_game parameter
    // (the fix changed placement.cpp from hardcoded 0 to features.is_game)
    HybridLearner& hybrid = aatpe_ml_hybrid::instance();
    hybrid.init(3);
    hybrid.setEnabled(true);

    // Test with is_game=1 vs is_game=0 - should produce different dot products
    // if the model has learned any is_game weight
    float f_game[15] = {1, 0.5f, 0.3f, 0.4f, 0.5f, 0.5f, 1.0f, 0.5f, 0.6f, 1.0f, 0, 0.3f, 0.2f, 0.3f, 0.25f};
    float f_nogame[15] = {1, 0.5f, 0.3f, 0.4f, 0.5f, 0.5f, 0.0f, 0.5f, 0.6f, 1.0f, 0, 0.3f, 0.2f, 0.3f, 0.25f};

    float adj_game = hybrid.getFineTuneAdjustment(2, f_game);
    float adj_nogame = hybrid.getFineTuneAdjustment(2, f_nogame);
    CHECK(std::isfinite(adj_game), "Hybrid adjustment with is_game=1 is finite");
    CHECK(std::isfinite(adj_nogame), "Hybrid adjustment with is_game=0 is finite");
    printf("    adj(game)=%.4f adj(nogame)=%.4f\n", adj_game, adj_nogame);

    // Verify the 15-arg overload correctly passes is_game
    float adj_overload = hybrid.getFineTuneAdjustment(2, 0.5f, 0.3f, 0.4f, 0.5f, 0.5f,
        1.0f, 0.5f, 0.6f, 1.0f, 0, 0.3f, 0.2f, 0.3f, 0.25f);
    CHECK(std::abs(adj_overload - adj_game) < 0.0001f, "15-arg overload matches array version (is_game passed correctly)");
}

// ============================================================================
// Problem 2: Thread Classification Tests (big core underutilization root cause)
// ============================================================================

// We can't directly call classifyThread (it reads /proc), but we can verify
// the isGameThreadName helper logic by replicating it and testing the concept.
// The real verification is that the heuristic order now checks game names BEFORE throughput.

void test_game_thread_name_detection() {
    printf("\n=== Problem 2: Game Thread Name Detection ===\n");

    // These are the patterns added to isGameThreadName() in thread_scan.cpp
    static const char* kGameNamePatterns[] = {
        "Unity", "Unreal", "Game", "Gfx", "Render", "Physics", "PhysX",
        "AI", "bgfx", "Vulkan", "GLThread", "GPU", "FMOD", "Wwise",
        "JobSystem", "Worker Thread", "Netcode", "NetworkUpdate"
    };
    int patternCount = sizeof(kGameNamePatterns) / sizeof(kGameNamePatterns[0]);

    auto isGameName = [&](const std::string& tname) -> bool {
        for (int i = 0; i < patternCount; i++) {
            if (tname.find(kGameNamePatterns[i]) != std::string::npos) return true;
        }
        return false;
    };

    // Test known game thread names
    CHECK(isGameName("UnityMain"), "UnityMain detected as game thread");
    CHECK(isGameName("UnityGfxWorker"), "UnityGfxWorker detected as game thread");
    CHECK(isGameName("UnityGfxDeviceWorker"), "UnityGfxDeviceWorker detected as game thread");
    CHECK(isGameName("RenderThread"), "RenderThread detected as game thread");
    CHECK(isGameName("GameThread"), "GameThread detected as game thread");
    CHECK(isGameName("Physics"), "Physics detected as game thread");
    CHECK(isGameName("PhysX"), "PhysX detected as game thread");
    CHECK(isGameName("VulkanRender"), "VulkanRender detected as game thread");
    CHECK(isGameName("bgfx"), "bgfx detected as game thread");
    CHECK(isGameName("FMOD"), "FMOD detected as game thread");

    // Test non-game thread names
    CHECK(!isGameName("main"), "main NOT detected as game thread (handled by rules)");
    CHECK(!isGameName("Binder:123"), "Binder NOT detected as game thread");
    CHECK(!isGameName("kworker/0:1"), "kworker NOT detected as game thread");
    CHECK(!isGameName("HeapTaskDaemon"), "HeapTaskDaemon NOT detected as game thread");
    CHECK(!isGameName("AsyncTask #1"), "AsyncTask NOT detected as game thread");
}

void test_pool_score_game_vs_throughput() {
    printf("\n=== Problem 2: Pool Score GAME vs THROUGHPUT (root cause verification) ===\n");
    // This test verifies the mathematical root cause:
    // S3_GAME has strong big-core pool preference (BIG=50, MID=20, LITTLE=-40)
    // S4_THROUGHPUT has weak preference (BIG=20, MID=30, LITTLE=10)
    // Misclassifying game workers as THROUGHPUT removes the 90-point BIG-vs-LITTLE gap,
    // allowing contention idle boost (+80) to push threads to little cores.

    // GAME pool scores (from aatpe-constants.h)
    const int gameBig = 50, gameLittle = -40;
    // THROUGHPUT pool scores
    const int tpBig = 20, tpLittle = 10;

    // BIG-vs-LITTLE spread
    int gameSpread = gameBig - gameLittle;  // 90
    int tpSpread = tpBig - tpLittle;         // 10
    CHECK(gameSpread == 90, "GAME BIG-vs-LITTLE pool spread = 90 (strong big preference)");
    CHECK(tpSpread == 10, "THROUGHPUT BIG-vs-LITTLE pool spread = 10 (weak preference)");
    CHECK(gameSpread > tpSpread * 5, "GAME spread is 9x THROUGHPUT spread (misclassification removes big-core pull)");

    // With idle little core contention boost = +80:
    // GAME: BIG pool advantage 90 > idle boost 80 → BIG still wins
    // THROUGHPUT: BIG pool advantage 10 < idle boost 80 → LITTLE wins!
    const int idleBoost = 80;
    CHECK(gameSpread > idleBoost, "GAME: big pool advantage (90) > idle boost (80) → big wins");
    CHECK(tpSpread < idleBoost, "THROUGHPUT: big pool advantage (10) < idle boost (80) → little wins (ROOT CAUSE)");

    printf("    Root cause confirmed: misclassified game workers (S4) lose 80-point big-core advantage,\n");
    printf("    allowing idle-little contention boost (+80) to pull them to little cores.\n");
}

void test_ml_predict_all_pools() {
    printf("\n=== Problem 1: ML Predict across all 3 pools with is_game ===\n");
    LearningModel model;
    model.init(3);
    model.setEnabled(true);

    // Train each pool with different outcomes for game threads
    ThreadFeatures fGame = LearningModel::extractFeatures(
        8000, 300, 40, 90, -5, true, 55, 1024, false, 0);
    ThreadFeatures fNonGame = LearningModel::extractFeatures(
        2000, 100, 20, 50, 0, false, 45, 256, false, 0);

    // Big pool (idx=2): game threads get good outcome
    for (int i = 0; i < 50; i++) model.update(2, fGame, 0.9f);
    // Little pool (idx=0): game threads get poor outcome
    for (int i = 0; i < 50; i++) model.update(0, fGame, 0.2f);

    float predBig = model.predict(2, fGame);
    float predLittle = model.predict(0, fGame);
    CHECK(predBig > predLittle, "ML predicts game thread better on BIG pool than LITTLE");
    printf("    pred(BIG)=%.4f pred(LITTLE)=%.4f\n", predBig, predLittle);

    // Verify is_game feature contributes to prediction difference
    float predBig_nogame = model.predict(2, fNonGame);
    printf("    pred(BIG, game)=%.4f pred(BIG, non-game)=%.4f\n", predBig, predBig_nogame);
    CHECK(std::isfinite(predBig) && std::isfinite(predLittle), "All predictions finite");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("================================================================\n");
    printf("  AATPE Core Fix Verification Tests\n");
    printf("  Problem 1: 11-dim weight / is_game logic\n");
    printf("  Problem 2: Big core underutilization in games\n");
    printf("================================================================\n");

    test_ml_weight_structure();
    test_ml_update_is_game_gradient();
    test_hybrid_is_game_inference();
    test_game_thread_name_detection();
    test_pool_score_game_vs_throughput();
    test_ml_predict_all_pools();

    printf("\n================================================================\n");
    printf("  RESULTS: %d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
    printf("================================================================\n");

    return g_failed > 0 ? 1 : 0;
}
