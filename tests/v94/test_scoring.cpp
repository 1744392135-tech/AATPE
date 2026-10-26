// test_scoring.cpp - v94 hermetic unit tests for scoring.cpp core math.
// Links scoring.cpp + aatpe_ml.cpp with scoring_deps stubs (see build.sh recipe).
// Exercises: initScoringArrays, load prediction (linear regression),
// predictedLoadPenalty, prediction confidence/MAE, capacityScore, thermalScore,
// powerScore (profile-dependent), frameScore (frameAware+frameCritical gate).
#include <cstdio>
#include <cmath>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "aatped.h"
using namespace aatpe; using namespace aatpe_ml;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== Scoring: initScoringArrays ===\n");
    initScoringArrays(4);
    // After init, every cpu's load history must be empty -> neutral prediction.
    bool allNeutral = true;
    for (int c = 0; c < 4; c++) if (predictCpuLoad(c) != 50.0f) allNeutral = false;
    CHECK(allNeutral, "initScoringArrays(4) resets load history to neutral 50");

    printf("=== Scoring: load prediction (linear regression) ===\n");
    // Feed a steadily rising load curve: 10..19 over 10 samples.
    for (int v = 10; v <= 19; v++) recordCpuLoadSample(0, v);
    float pred = predictCpuLoad(0);
    CHECK(pred > 15.0f && pred < 30.0f, "rising trend predicts future load >15");
    // Empty history -> neutral 50
    float predEmpty = predictCpuLoad(2);
    CHECK(predEmpty == 50.0f, "empty history predicts neutral 50");
    // Out-of-range cpu -> 50
    float predBad = predictCpuLoad(999);
    CHECK(predBad == 50.0f, "out-of-range cpuId returns 50");

    printf("=== Scoring: predictedLoadPenalty ===\n");
    // High load (>80) -> positive penalty
    recordCpuLoadSample(1, 90);
    recordCpuLoadSample(1, 92);
    recordCpuLoadSample(1, 94);
    int penHi = predictedLoadPenalty(1);
    CHECK(penHi > 0, "high predicted load -> positive penalty");
    // G7 R004-B2 (M3): idle/low load (<30) is now NEUTRAL (0). The old behaviour
    // returned (30-predicted) which baseScore subtracted, actively punishing idle
    // cores and suppressing wake-up of the prime cluster. Only saturated cores
    // (>80%) are penalized now.
    recordCpuLoadSample(2, 10);
    recordCpuLoadSample(2, 12);
    recordCpuLoadSample(2, 14);
    int penLo = predictedLoadPenalty(2);
    CHECK(penLo == 0, "idle predicted load -> neutral (no idle punishment)");

    printf("=== Scoring: prediction confidence & MAE ===\n");
    // No errors yet -> default neutral 0.5
    float conf0 = getPredictionConfidence(0);
    CHECK(conf0 == 0.5f, "no errors -> neutral confidence 0.5");
    // Record small errors -> high confidence
    for (int i = 0; i < 5; i++) recordPredictionError(0, 50.0f, 51.0f);
    float conf1 = getPredictionConfidence(0);
    CHECK(conf1 > 0.9f, "tiny MAE -> confidence near 1.0");
    // Record large errors -> low confidence
    for (int i = 0; i < 5; i++) recordPredictionError(1, 50.0f, 90.0f);
    float conf2 = getPredictionConfidence(1);
    CHECK(conf2 < 0.5f, "large MAE -> confidence < 0.5");
    float mae = getAveragePredictionMAE();
    CHECK(mae >= 0.0f, "average MAE is non-negative");

    printf("=== Scoring: capacityScore ===\n");
    {
        std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
        g_cpuMetrics[3].capacity = 512;
    }
    int capGame = capacityScore(3, ROLE_S3_GAME);
    CHECK(capGame == 512, "game thread gets raw capacity");
    int capSys = capacityScore(3, ROLE_S0_SYSTEM);
    CHECK(capSys == 0, "system thread gets 0 capacity score");
    int capMiss = capacityScore(99, ROLE_S3_GAME);
    CHECK(capMiss == 0, "unknown cpu -> 0");

    printf("=== Scoring: thermalScore ===\n");
    {
        std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
        g_cpuMetrics[3].temp = 0;  // uninitialized
    }
    int ts0 = thermalScore(3);
    CHECK(ts0 == 0, "temp==0 (uninitialized) -> 0, no false boost");
    {
        std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
        g_cpuMetrics[3].temp = 90;  // hot
    }
    int tsHot = thermalScore(3);
    CHECK(tsHot < 0, "hot temp -> negative (penalty)");
    {
        std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
        g_cpuMetrics[3].temp = 10;  // cold
    }
    int tsCold = thermalScore(3);
    CHECK(tsCold > 0, "cold temp -> positive (boost)");

    printf("=== Scoring: powerScore (profile-dependent) ===\n");
    g_currentProfileEnum = ProfileType::BATTERY;
    int pBg = powerScore(0, ROLE_S5_BACKGROUND);
    CHECK(pBg != 0, "battery profile gives background score");
    g_currentProfileEnum = ProfileType::GAMING;
    int pGame = powerScore(0, ROLE_S3_GAME);
    CHECK(pGame != 0, "gaming profile gives game score");
    g_currentProfileEnum = ProfileType::BALANCED;
    int pBal = powerScore(0, ROLE_S3_GAME);
    CHECK(pBal == 0, "balanced profile -> 0 power score");

    printf("=== Scoring: frameScore gate ===\n");
    g_cfg.frameAware = true;
    g_frameCritical = true;
    int fsGame = frameScore(0, ROLE_S3_GAME);
    CHECK(fsGame > 0, "frameCritical+frameAware -> game thread boost");
    int fsBg = frameScore(0, ROLE_S5_BACKGROUND);
    CHECK(fsBg == 0, "background thread gets 0 frame boost");
    g_frameCritical = false;
    int fsNoCrit = frameScore(0, ROLE_S3_GAME);
    CHECK(fsNoCrit == 0, "not frame-critical -> 0 frame boost");

    printf("=== G7 R004-B2: poolScore tier monotonicity (H1) ===\n");
    // Stub topology: cpu0-3 LITTLE, cpu4-5 MID, cpu6 BIG, cpu7 PRIME.
    extern int poolScore(int cpuId, ThreadRole role);
    // UI (S1): PRIME(7) > BIG(6) > MID(5) > LITTLE(0)
    CHECK(poolScore(7, ROLE_S1_UI) > poolScore(6, ROLE_S1_UI),
          "UI: PRIME > BIG");
    CHECK(poolScore(6, ROLE_S1_UI) > poolScore(5, ROLE_S1_UI),
          "UI: BIG > MID (was the kUiBig<kUiMid inversion)");
    CHECK(poolScore(5, ROLE_S1_UI) > poolScore(0, ROLE_S1_UI),
          "UI: MID > LITTLE");
    // Game (S3): same ordering
    CHECK(poolScore(7, ROLE_S3_GAME) > poolScore(6, ROLE_S3_GAME) &&
          poolScore(6, ROLE_S3_GAME) > poolScore(5, ROLE_S3_GAME) &&
          poolScore(5, ROLE_S3_GAME) > poolScore(0, ROLE_S3_GAME),
          "Game: PRIME>BIG>MID>LITTLE monotonic");
    // Render (S2)
    CHECK(poolScore(7, ROLE_S2_RENDER) > poolScore(6, ROLE_S2_RENDER) &&
          poolScore(6, ROLE_S2_RENDER) > poolScore(5, ROLE_S2_RENDER),
          "Render: PRIME>BIG>MID");
    // Throughput (S4): was kThroughputBig(20)<kThroughputMid(30) inversion.
    CHECK(poolScore(6, ROLE_S4_THROUGHPUT) > poolScore(5, ROLE_S4_THROUGHPUT),
          "Throughput: BIG > MID (was inverted)");
    // Background (S5): prefers LITTLE, avoids PRIME.
    CHECK(poolScore(0, ROLE_S5_BACKGROUND) > poolScore(6, ROLE_S5_BACKGROUND) &&
          poolScore(6, ROLE_S5_BACKGROUND) > poolScore(7, ROLE_S5_BACKGROUND),
          "Background: LITTLE>BIG>PRIME (avoids super-big)");

    printf("\n=== ML-10: history sweep drops stale/foreign-boot tid files ===\n");
    {
        // RUN_DIR comes from scoring_deps.cpp = /tmp/aatpe_v94_scoring.
        g_startTime.store((time_t)1700000100);  // "this boot"
        std::string dir = std::string(RUN_DIR) + "/history";
        ::mkdir(RUN_DIR, 0700);
        ::mkdir(dir.c_str(), 0700);
        // legacy file from a pre-stamp daemon (no daemon_boot_ts line)
        FILE* f1 = fopen((dir + "/4242").c_str(), "w");
        CHECK(f1 != nullptr, "ML-10: seed legacy history file");
        if (f1) { fprintf(f1, "preferred_cpu=0\nplacement_success_rate=80\nsamples=10\nlast_update=100\n"); fclose(f1); }
        // foreign boot (previous daemon instance, tid recycled)
        FILE* f2 = fopen((dir + "/4343").c_str(), "w");
        CHECK(f2 != nullptr, "ML-10: seed foreign-boot history file");
        if (f2) { fprintf(f2, "daemon_boot_ts=1600000000\nplacement_success_rate=50\n"); fclose(f2); }
        // stamped with THIS boot -> must survive the sweep
        FILE* f3 = fopen((dir + "/4444").c_str(), "w");
        CHECK(f3 != nullptr, "ML-10: seed current-boot history file");
        if (f3) { fprintf(f3, "daemon_boot_ts=1700000100\nplacement_success_rate=50\n"); fclose(f3); }
        // non-numeric file must be left alone
        FILE* f4 = fopen((dir + "/README").c_str(), "w");
        CHECK(f4 != nullptr, "ML-10: seed non-tid file");
        if (f4) { fprintf(f4, "ignore me\n"); fclose(f4); }

        scoringHistorySweepStale();

        struct stat st;
        CHECK(::stat((dir + "/4242").c_str(), &st) != 0, "ML-10: legacy (no stamp) tid file swept");
        CHECK(::stat((dir + "/4343").c_str(), &st) != 0, "ML-10: foreign-boot tid file swept");
        CHECK(::stat((dir + "/4444").c_str(), &st) == 0, "ML-10: current-boot tid file kept");
        CHECK(::stat((dir + "/README").c_str(), &st) == 0, "ML-10: non-numeric file untouched");
        ::unlink((dir + "/4444").c_str());
        ::unlink((dir + "/README").c_str());
    }

    printf("\n========================================\nScoring results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
