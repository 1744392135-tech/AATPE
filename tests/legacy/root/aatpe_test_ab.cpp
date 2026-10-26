// AATPE AB Test Suite — independent 5th test suite
// [LEGACY-TEST v66-v74] 历史遗留测试：部分金值断言(eBPF 字节码偏移/map 数量/文件 md5/硬编码版本号)可能已过时；其失败不一定代表生产 bug。
// 见 tests/README.md 中"测试失败分类指南"。请勿删除(有考古价值)。
// Mirrors the AB test implementation in aatped.cpp (fix94 Task10 / fix97 P5 / fix101 P6)
// Covers: frame time recording, winner determination, reset, config toggle,
//         data export, and edge cases.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <ctime>

// ============================================================================
// AB Test implementation (mirrored exactly from aatped.cpp)
// ============================================================================
struct ABTestStats {
    long long decisionsA = 0;
    long long decisionsB = 0;
    float avgFrameTimeA = 0;
    float avgFrameTimeB = 0;
    float varianceA = 0;
    float varianceB = 0;
    int sampleCountA = 0;
    int sampleCountB = 0;
    int jankCountA = 0;
    int jankCountB = 0;
    int migrationsA = 0;
    int migrationsB = 0;
    int winner = 0;       // 0=none, 1=A, 2=B
    bool active = false;
};

static ABTestStats g_abStats;
static bool g_abTestEnabled = true;  // config toggle

static int abTestAssignThread(int tid) {
    static unsigned int abSeed = 0;
    if (abSeed == 0) {
        abSeed = (unsigned int)time(NULL) ^ 0x5a5a5a5a;
    }
    unsigned int h = (unsigned int)tid * 2654435761u + abSeed;
    h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16;
    return h % 2;
}

// Regularized incomplete beta function I_x(a,b) — Lentz's algorithm
static double betaIncomplete(double x, double a, double b) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    double lbeta = lgamma(a) + lgamma(b) - lgamma(a + b);
    double front = exp(log(x) * a + log(1.0 - x) * b - lbeta) / a;
    double fpmin = 1e-30;
    double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0, d = 1.0 - qab * x / qap;
    if (fabs(d) < fpmin) d = fpmin;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= 200; m++) {
        int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < fpmin) d = fpmin;
        c = 1.0 + aa / c;
        if (fabs(c) < fpmin) c = fpmin;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < fpmin) d = fpmin;
        c = 1.0 + aa / c;
        if (fabs(c) < fpmin) c = fpmin;
        d = 1.0 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1.0) < 3e-7) break;
    }
    return front * h;
}

static double tDistCDF(double t, double df) {
    if (df <= 0.0) return 0.5;
    double x = df / (df + t * t);
    double ibeta = betaIncomplete(x, df / 2.0, 0.5);
    if (t > 0) return 1.0 - 0.5 * ibeta;
    return 0.5 * ibeta;
}

// Welch's t-test — returns two-tailed p-value
static double abTTest(const ABTestStats& a, const ABTestStats& b) {
    if (a.sampleCountA < 2 || b.sampleCountB < 2) return 1.0;
    double meanA = a.avgFrameTimeA;
    double meanB = b.avgFrameTimeB;
    double varA = (a.varianceA);
    double varB = (b.varianceB);
    if (varA < 0) varA = 0;
    if (varB < 0) varB = 0;
    double se = sqrt(varA / a.sampleCountA + varB / b.sampleCountB);
    if (se < 1e-9) return 1.0;
    double t = (meanA - meanB) / se;
    double df = (varA / a.sampleCountA + varB / b.sampleCountB) * (varA / a.sampleCountA + varB / b.sampleCountB)
              / ((varA / a.sampleCountA) * (varA / a.sampleCountA) / (a.sampleCountA - 1)
               + (varB / b.sampleCountB) * (varB / b.sampleCountB) / (b.sampleCountB - 1));
    if (df < 1.0) df = 1.0;
    double p = 2.0 * (1.0 - tDistCDF(fabs(t), df));
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    return p;
}

static int abTestDetermineWinner(void) {
    if (g_abStats.sampleCountA < 100 || g_abStats.sampleCountB < 100) return -1;
    double p = abTTest(g_abStats, g_abStats);
    if (p < 0.05) {
        return (g_abStats.avgFrameTimeA < g_abStats.avgFrameTimeB) ? 0 : 1;
    }
    return -1;
}

static void abTestRecordFrameTime(int group, float frameTime) {
    if (!g_abTestEnabled) return;
    if (group == 1) {
        g_abStats.decisionsA++;
        g_abStats.sampleCountA++;
        g_abStats.avgFrameTimeA += (frameTime - g_abStats.avgFrameTimeA) / g_abStats.decisionsA;
    } else if (group == 2) {
        g_abStats.decisionsB++;
        g_abStats.sampleCountB++;
        g_abStats.avgFrameTimeB += (frameTime - g_abStats.avgFrameTimeB) / g_abStats.decisionsB;
    }
}

static void abTestRecordJank(int group) {
    if (!g_abTestEnabled) return;
    if (group == 1) g_abStats.jankCountA++;
    else if (group == 2) g_abStats.jankCountB++;
}

static void abTestRecordMigration(int group) {
    if (!g_abTestEnabled) return;
    if (group == 1) g_abStats.migrationsA++;
    else if (group == 2) g_abStats.migrationsB++;
}

static void abTestReset(void) {
    g_abStats = ABTestStats();
}

// Data export: serialize stats to JSON string
static std::string abTestExportJson(void) {
    std::ostringstream oss;
    oss << "{\"ab_test\":{"
        << "\"group_a_avg\":" << g_abStats.avgFrameTimeA
        << ",\"group_b_avg\":" << g_abStats.avgFrameTimeB
        << ",\"group_a_samples\":" << g_abStats.sampleCountA
        << ",\"group_b_samples\":" << g_abStats.sampleCountB
        << ",\"group_a_jank\":" << g_abStats.jankCountA
        << ",\"group_b_jank\":" << g_abStats.jankCountB
        << ",\"group_a_migrations\":" << g_abStats.migrationsA
        << ",\"group_b_migrations\":" << g_abStats.migrationsB
        << ",\"winner\":" << g_abStats.winner
        << ",\"p_value\":" << abTTest(g_abStats, g_abStats)
        << "}}";
    return oss.str();
}

// Data export: write stats to state files
static bool abTestExportStateFiles(const std::string& dir) {
    auto writeInt = [&](const char* name, long long val) {
        std::ofstream f(dir + "/" + name);
        if (!f) return false;
        f << val;
        return true;
    };
    if (!writeInt("ab_test_group_a_avg", (long long)g_abStats.avgFrameTimeA)) return false;
    if (!writeInt("ab_test_group_b_avg", (long long)g_abStats.avgFrameTimeB)) return false;
    if (!writeInt("ab_test_group_a_samples", g_abStats.sampleCountA)) return false;
    if (!writeInt("ab_test_group_b_samples", g_abStats.sampleCountB)) return false;
    if (!writeInt("ab_test_winner", g_abStats.winner)) return false;
    std::ofstream pf(dir + "/ab_test_p_value");
    if (!pf) return false;
    pf << abTTest(g_abStats, g_abStats);
    return true;
}

// ============================================================================
// Test framework
// ============================================================================
static int g_testsRun = 0;
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_testsRun++; \
    if (cond) { g_testsPassed++; printf("  [PASS] %s\n", msg); } \
    else { g_testsFailed++; printf("  [FAIL] %s\n", msg); } \
} while(0)

#define TEST_ASSERT_FLOAT_NEAR(actual, expected, tol, msg) do { \
    g_testsRun++; \
    if (fabs((double)(actual) - (double)(expected)) <= (tol)) { \
        g_testsPassed++; printf("  [PASS] %s (got %.6f, expected %.6f)\n", msg, (double)(actual), (double)(expected)); \
    } else { \
        g_testsFailed++; printf("  [FAIL] %s (got %.6f, expected %.6f, diff %.6f)\n", msg, (double)(actual), (double)(expected), fabs((double)(actual)-(double)(expected))); \
    } \
} while(0)

// ============================================================================
// Test 1: abTestRecordFrameTime — sampleCount and frameTime accumulation
// ============================================================================
static void testRecordFrameTime() {
    printf("\n=== Test 1: abTestRecordFrameTime Accumulation ===\n");
    abTestReset();

    // Record 10 frames for group A (16ms each)
    for (int i = 0; i < 10; i++) {
        abTestRecordFrameTime(1, 16.0f);
    }
    TEST_ASSERT(g_abStats.sampleCountA == 10, "Group A sampleCount == 10 after 10 records");
    TEST_ASSERT(g_abStats.decisionsA == 10, "Group A decisionsA == 10");
    TEST_ASSERT_FLOAT_NEAR(g_abStats.avgFrameTimeA, 16.0f, 0.01f, "Group A avgFrameTime == 16.0");

    // Record 10 frames for group B (20ms each)
    for (int i = 0; i < 10; i++) {
        abTestRecordFrameTime(2, 20.0f);
    }
    TEST_ASSERT(g_abStats.sampleCountB == 10, "Group B sampleCount == 10 after 10 records");
    TEST_ASSERT(g_abStats.decisionsB == 10, "Group B decisionsB == 10");
    TEST_ASSERT_FLOAT_NEAR(g_abStats.avgFrameTimeB, 20.0f, 0.01f, "Group B avgFrameTime == 20.0");

    // Welford online mean: varying values
    abTestReset();
    float valuesA[] = {16.0f, 17.0f, 15.0f, 18.0f, 14.0f};
    float sumA = 0;
    for (float v : valuesA) {
        abTestRecordFrameTime(1, v);
        sumA += v;
    }
    float expectedAvgA = sumA / 5.0f;
    TEST_ASSERT(g_abStats.sampleCountA == 5, "Group A sampleCount == 5 after varied records");
    TEST_ASSERT_FLOAT_NEAR(g_abStats.avgFrameTimeA, expectedAvgA, 0.01f,
                           "Group A Welford mean matches arithmetic mean");

    // Invalid group (0 or 3) should not affect counts
    int beforeA = g_abStats.sampleCountA;
    int beforeB = g_abStats.sampleCountB;
    abTestRecordFrameTime(0, 16.0f);
    abTestRecordFrameTime(3, 16.0f);
    TEST_ASSERT(g_abStats.sampleCountA == beforeA, "Invalid group 0 does not affect group A");
    TEST_ASSERT(g_abStats.sampleCountB == beforeB, "Invalid group 3 does not affect group B");

    // Large number of records (stress test accumulation)
    abTestReset();
    for (int i = 0; i < 10000; i++) {
        abTestRecordFrameTime(1, 16.666f);
        abTestRecordFrameTime(2, 16.666f);
    }
    TEST_ASSERT(g_abStats.sampleCountA == 10000, "Group A handles 10000 records");
    TEST_ASSERT(g_abStats.sampleCountB == 10000, "Group B handles 10000 records");
    TEST_ASSERT_FLOAT_NEAR(g_abStats.avgFrameTimeA, 16.666f, 0.1f, "Group A avg stable after 10000 records");
}

// ============================================================================
// Test 2: abTestDetermineWinner — statistical significance
// ============================================================================
static void testDetermineWinner() {
    printf("\n=== Test 2: abTestDetermineWinner Statistical Significance ===\n");

    // 2a: Insufficient samples (< 100) → always -1
    abTestReset();
    for (int i = 0; i < 50; i++) {
        abTestRecordFrameTime(1, 16.0f);
        abTestRecordFrameTime(2, 30.0f);
    }
    TEST_ASSERT(abTestDetermineWinner() == -1, "Winner == -1 with < 100 samples per group");

    // 2b: Exactly 99 samples → still -1
    abTestReset();
    for (int i = 0; i < 99; i++) {
        abTestRecordFrameTime(1, 16.0f);
        abTestRecordFrameTime(2, 30.0f);
    }
    TEST_ASSERT(abTestDetermineWinner() == -1, "Winner == -1 with exactly 99 samples");

    // 2c: 100+ samples but zero variance → p=1.0 → -1 (current implementation behavior)
    abTestReset();
    for (int i = 0; i < 150; i++) {
        abTestRecordFrameTime(1, 16.0f);
        abTestRecordFrameTime(2, 30.0f);
    }
    TEST_ASSERT(g_abStats.sampleCountA >= 100, "Group A has >= 100 samples");
    TEST_ASSERT(g_abStats.sampleCountB >= 100, "Group B has >= 100 samples");
    // Note: recordFrameTime does not update variance, so variance=0 → se=0 → p=1.0
    TEST_ASSERT(abTestDetermineWinner() == -1,
                "Winner == -1 with zero variance (se≈0 → p=1.0, current impl behavior)");

    // 2d: Manually set variance → t-test should detect significant difference
    abTestReset();
    for (int i = 0; i < 200; i++) {
        abTestRecordFrameTime(1, 16.0f);
        abTestRecordFrameTime(2, 25.0f);
    }
    g_abStats.varianceA = 4.0f;   // stddev=2
    g_abStats.varianceB = 4.0f;
    double p = abTTest(g_abStats, g_abStats);
    printf("    [INFO] p-value with variance=4, meanA=16, meanB=25, n=200: %.6f\n", p);
    TEST_ASSERT(p < 0.001, "p-value < 0.001 with clear mean difference and variance");
    int winner = abTestDetermineWinner();
    TEST_ASSERT(winner == 0, "Winner == 0 (A wins) when A has lower frame time and p<0.05");

    // 2e: B wins when B has lower frame time
    abTestReset();
    for (int i = 0; i < 200; i++) {
        abTestRecordFrameTime(1, 25.0f);
        abTestRecordFrameTime(2, 16.0f);
    }
    g_abStats.varianceA = 4.0f;
    g_abStats.varianceB = 4.0f;
    winner = abTestDetermineWinner();
    TEST_ASSERT(winner == 1, "Winner == 1 (B wins) when B has lower frame time and p<0.05");

    // 2f: No significant difference when means are close
    abTestReset();
    for (int i = 0; i < 200; i++) {
        abTestRecordFrameTime(1, 16.0f);
        abTestRecordFrameTime(2, 16.5f);
    }
    g_abStats.varianceA = 10.0f;  // high variance → high se
    g_abStats.varianceB = 10.0f;
    p = abTTest(g_abStats, g_abStats);
    printf("    [INFO] p-value with variance=10, meanA=16, meanB=16.5, n=200: %.6f\n", p);
    winner = abTestDetermineWinner();
    TEST_ASSERT(winner == -1, "Winner == -1 when means close relative to variance");

    // 2g: t-test with < 2 samples returns 1.0
    ABTestStats small;
    small.sampleCountA = 1;
    small.sampleCountB = 1;
    small.avgFrameTimeA = 16;
    small.avgFrameTimeB = 30;
    small.varianceA = 4;
    small.varianceB = 4;
    TEST_ASSERT(abTTest(small, small) == 1.0, "abTTest returns 1.0 with < 2 samples");
}

// ============================================================================
// Test 3: AB test state reset
// ============================================================================
static void testReset() {
    printf("\n=== Test 3: AB Test State Reset ===\n");
    abTestReset();

    // Populate with data
    for (int i = 0; i < 50; i++) {
        abTestRecordFrameTime(1, 16.0f);
        abTestRecordFrameTime(2, 20.0f);
        abTestRecordJank(1);
        abTestRecordMigration(2);
    }
    TEST_ASSERT(g_abStats.sampleCountA == 50, "Before reset: sampleCountA == 50");
    TEST_ASSERT(g_abStats.sampleCountB == 50, "Before reset: sampleCountB == 50");
    TEST_ASSERT(g_abStats.jankCountA == 50, "Before reset: jankCountA == 50");
    TEST_ASSERT(g_abStats.migrationsB == 50, "Before reset: migrationsB == 50");
    TEST_ASSERT(g_abStats.avgFrameTimeA > 0, "Before reset: avgFrameTimeA > 0");

    // Reset
    abTestReset();
    TEST_ASSERT(g_abStats.sampleCountA == 0, "After reset: sampleCountA == 0");
    TEST_ASSERT(g_abStats.sampleCountB == 0, "After reset: sampleCountB == 0");
    TEST_ASSERT(g_abStats.decisionsA == 0, "After reset: decisionsA == 0");
    TEST_ASSERT(g_abStats.decisionsB == 0, "After reset: decisionsB == 0");
    TEST_ASSERT(g_abStats.avgFrameTimeA == 0, "After reset: avgFrameTimeA == 0");
    TEST_ASSERT(g_abStats.avgFrameTimeB == 0, "After reset: avgFrameTimeB == 0");
    TEST_ASSERT(g_abStats.varianceA == 0, "After reset: varianceA == 0");
    TEST_ASSERT(g_abStats.varianceB == 0, "After reset: varianceB == 0");
    TEST_ASSERT(g_abStats.jankCountA == 0, "After reset: jankCountA == 0");
    TEST_ASSERT(g_abStats.jankCountB == 0, "After reset: jankCountB == 0");
    TEST_ASSERT(g_abStats.migrationsA == 0, "After reset: migrationsA == 0");
    TEST_ASSERT(g_abStats.migrationsB == 0, "After reset: migrationsB == 0");
    TEST_ASSERT(g_abStats.winner == 0, "After reset: winner == 0 (none)");
    TEST_ASSERT(g_abStats.active == false, "After reset: active == false");

    // Can record again after reset
    abTestRecordFrameTime(1, 16.0f);
    TEST_ASSERT(g_abStats.sampleCountA == 1, "After reset+record: sampleCountA == 1");
    TEST_ASSERT_FLOAT_NEAR(g_abStats.avgFrameTimeA, 16.0f, 0.01f, "After reset+record: avg correct");
}

// ============================================================================
// Test 4: AB test config toggle and group assignment
// ============================================================================
static void testConfigToggle() {
    printf("\n=== Test 4: AB Test Config Toggle & Group Assignment ===\n");

    // 4a: Enabled toggle — when disabled, records are ignored
    abTestReset();
    g_abTestEnabled = true;
    abTestRecordFrameTime(1, 16.0f);
    TEST_ASSERT(g_abStats.sampleCountA == 1, "Enabled: record affects sampleCountA");

    g_abTestEnabled = false;
    abTestRecordFrameTime(1, 16.0f);
    abTestRecordFrameTime(2, 20.0f);
    abTestRecordJank(1);
    abTestRecordMigration(2);
    TEST_ASSERT(g_abStats.sampleCountA == 1, "Disabled: record does NOT affect sampleCountA");
    TEST_ASSERT(g_abStats.sampleCountB == 0, "Disabled: record does NOT affect sampleCountB");
    TEST_ASSERT(g_abStats.jankCountA == 0, "Disabled: jank not recorded");
    TEST_ASSERT(g_abStats.migrationsB == 0, "Disabled: migration not recorded");

    g_abTestEnabled = true;  // restore
    abTestRecordFrameTime(1, 16.0f);
    TEST_ASSERT(g_abStats.sampleCountA == 2, "Re-enabled: record affects sampleCountA again");

    // 4b: abTestAssignThread returns 0 or 1
    int group0 = abTestAssignThread(100);
    TEST_ASSERT(group0 == 0 || group0 == 1, "abTestAssignThread returns 0 or 1");

    // 4c: Same tid → same group (deterministic within a run)
    int group1 = abTestAssignThread(100);
    int group2 = abTestAssignThread(100);
    TEST_ASSERT(group1 == group2, "Same tid gets same group (deterministic)");

    // 4d: Different tids get potentially different groups; distribution ~50/50
    int countA = 0, countB = 0;
    for (int tid = 1; tid <= 1000; tid++) {
        int g = abTestAssignThread(tid * 7 + 13);  // spread tids
        if (g == 0) countA++;
        else countB++;
    }
    printf("    [INFO] Group distribution over 1000 tids: A=%d, B=%d (%.1f%%/%.1f%%)\n",
           countA, countB, countA / 10.0, countB / 10.0);
    TEST_ASSERT(countA > 200 && countA < 800, "Group A distribution in reasonable range (20-80%)");
    TEST_ASSERT(countB > 200 && countB < 800, "Group B distribution in reasonable range (20-80%)");

    // 4e: Conversion from assignThread (0/1) to record group (1/2) via +1
    int assigned = abTestAssignThread(200);
    int recordGroup = assigned + 1;
    TEST_ASSERT(recordGroup == 1 || recordGroup == 2, "assigned+1 gives valid record group (1 or 2)");

    // 4f: Jank and migration counters
    abTestReset();
    for (int i = 0; i < 5; i++) abTestRecordJank(1);
    for (int i = 0; i < 3; i++) abTestRecordJank(2);
    for (int i = 0; i < 10; i++) abTestRecordMigration(1);
    for (int i = 0; i < 7; i++) abTestRecordMigration(2);
    TEST_ASSERT(g_abStats.jankCountA == 5, "jankCountA == 5");
    TEST_ASSERT(g_abStats.jankCountB == 3, "jankCountB == 3");
    TEST_ASSERT(g_abStats.migrationsA == 10, "migrationsA == 10");
    TEST_ASSERT(g_abStats.migrationsB == 7, "migrationsB == 7");
}

// ============================================================================
// Test 5: AB test data export (JSON + state files)
// ============================================================================
static void testDataExport() {
    printf("\n=== Test 5: AB Test Data Export ===\n");
    abTestReset();

    // Populate
    for (int i = 0; i < 120; i++) {
        abTestRecordFrameTime(1, 16.5f);
        abTestRecordFrameTime(2, 18.0f);
    }
    for (int i = 0; i < 5; i++) abTestRecordJank(1);
    for (int i = 0; i < 8; i++) abTestRecordJank(2);
    for (int i = 0; i < 15; i++) abTestRecordMigration(1);
    for (int i = 0; i < 12; i++) abTestRecordMigration(2);
    g_abStats.varianceA = 2.0f;
    g_abStats.varianceB = 2.0f;
    g_abStats.winner = abTestDetermineWinner();

    // 5a: JSON export contains all fields
    std::string json = abTestExportJson();
    TEST_ASSERT(json.find("\"ab_test\"") != std::string::npos, "JSON contains ab_test key");
    TEST_ASSERT(json.find("group_a_avg") != std::string::npos, "JSON contains group_a_avg");
    TEST_ASSERT(json.find("group_b_avg") != std::string::npos, "JSON contains group_b_avg");
    TEST_ASSERT(json.find("group_a_samples") != std::string::npos, "JSON contains group_a_samples");
    TEST_ASSERT(json.find("group_b_samples") != std::string::npos, "JSON contains group_b_samples");
    TEST_ASSERT(json.find("group_a_jank") != std::string::npos, "JSON contains group_a_jank");
    TEST_ASSERT(json.find("group_b_jank") != std::string::npos, "JSON contains group_b_jank");
    TEST_ASSERT(json.find("p_value") != std::string::npos, "JSON contains p_value");
    TEST_ASSERT(json.find("winner") != std::string::npos, "JSON contains winner");
    printf("    [INFO] Exported JSON: %.120s...\n", json.c_str());

    // 5b: JSON values match stats
    TEST_ASSERT(json.find("120") != std::string::npos, "JSON contains sample count 120");

    // 5c: State file export
    std::string testDir = "/tmp/aatpe_ab_test_export";
    std::string mkdirCmd = "mkdir -p " + testDir;
    int _rc = system(mkdirCmd.c_str()); (void)_rc;
    bool exportOk = abTestExportStateFiles(testDir);
    TEST_ASSERT(exportOk, "State file export succeeds");

    // 5d: Verify state files exist and contain correct values
    auto readFile = [](const std::string& path) -> std::string {
        std::ifstream f(path);
        if (!f) return "";
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    std::string avgA = readFile(testDir + "/ab_test_group_a_avg");
    std::string avgB = readFile(testDir + "/ab_test_group_b_avg");
    std::string samplesA = readFile(testDir + "/ab_test_group_a_samples");
    std::string samplesB = readFile(testDir + "/ab_test_group_b_samples");
    std::string pval = readFile(testDir + "/ab_test_p_value");
    TEST_ASSERT(!avgA.empty(), "State file ab_test_group_a_avg exists and non-empty");
    TEST_ASSERT(!avgB.empty(), "State file ab_test_group_b_avg exists and non-empty");
    TEST_ASSERT(samplesA == "120", "State file group_a_samples == 120");
    TEST_ASSERT(samplesB == "120", "State file group_b_samples == 120");
    TEST_ASSERT(!pval.empty(), "State file ab_test_p_value exists and non-empty");
    printf("    [INFO] State files: avgA=%s avgB=%s samplesA=%s samplesB=%s p=%s\n",
           avgA.c_str(), avgB.c_str(), samplesA.c_str(), samplesB.c_str(), pval.c_str());

    // Cleanup
    std::string rmCmd = "rm -rf " + testDir;
    int _rc2 = system(rmCmd.c_str()); (void)_rc2;
}

// ============================================================================
// Test 6: Edge cases
// ============================================================================
static void testEdgeCases() {
    printf("\n=== Test 6: Edge Cases ===\n");

    // 6a: Extreme frame time values (0, negative, very large)
    abTestReset();
    abTestRecordFrameTime(1, 0.0f);
    TEST_ASSERT(g_abStats.sampleCountA == 1, "Zero frame time accepted");
    TEST_ASSERT_FLOAT_NEAR(g_abStats.avgFrameTimeA, 0.0f, 0.01f, "Zero frame time avg == 0");

    abTestRecordFrameTime(1, -5.0f);
    TEST_ASSERT(g_abStats.sampleCountA == 2, "Negative frame time accepted (no clamp)");

    abTestRecordFrameTime(1, 1e30f);
    TEST_ASSERT(g_abStats.sampleCountA == 3, "Very large frame time accepted");
    TEST_ASSERT(std::isfinite(g_abStats.avgFrameTimeA), "Avg remains finite after extreme values");

    // 6b: NaN and Inf handling
    abTestReset();
    abTestRecordFrameTime(1, NAN);
    TEST_ASSERT(g_abStats.sampleCountA == 1, "NaN frame time accepted (count increments)");

    abTestReset();
    abTestRecordFrameTime(1, INFINITY);
    TEST_ASSERT(g_abStats.sampleCountA == 1, "Inf frame time accepted (count increments)");

    // 6c: Empty stats → t-test returns 1.0
    abTestReset();
    TEST_ASSERT(abTTest(g_abStats, g_abStats) == 1.0, "abTTest returns 1.0 for empty stats");

    // 6d: determineWinner on empty stats → -1
    TEST_ASSERT(abTestDetermineWinner() == -1, "determineWinner returns -1 for empty stats");

    // 6e: Only one group has data
    abTestReset();
    for (int i = 0; i < 200; i++) abTestRecordFrameTime(1, 16.0f);
    TEST_ASSERT(abTestDetermineWinner() == -1, "determineWinner returns -1 when group B empty");

    // 6f: Large sample count overflow resilience
    abTestReset();
    // decisionsA is long long, sampleCountA is int — verify int max doesn't break
    for (int i = 0; i < 100000; i++) {
        abTestRecordFrameTime(1, 16.0f);
    }
    TEST_ASSERT(g_abStats.sampleCountA == 100000, "sampleCountA handles 100000 (int range)");
    TEST_ASSERT(g_abStats.decisionsA == 100000, "decisionsA handles 100000 (long long range)");
    TEST_ASSERT(std::isfinite(g_abStats.avgFrameTimeA), "Avg finite after 100000 records");

    // 6g: Variance clamping (negative variance → 0)
    ABTestStats negVar;
    negVar.sampleCountA = 100;
    negVar.sampleCountB = 100;
    negVar.avgFrameTimeA = 16;
    negVar.avgFrameTimeB = 20;
    negVar.varianceA = -5.0f;
    negVar.varianceB = -3.0f;
    double p = abTTest(negVar, negVar);
    TEST_ASSERT(p == 1.0, "Negative variance clamped to 0 → se=0 → p=1.0");

    // 6h: p-value always in [0, 1]
    abTestReset();
    for (int i = 0; i < 200; i++) {
        abTestRecordFrameTime(1, 16.0f + (i % 3));
        abTestRecordFrameTime(2, 20.0f + (i % 5));
    }
    g_abStats.varianceA = 1.0f;
    g_abStats.varianceB = 1.0f;
    p = abTTest(g_abStats, g_abStats);
    TEST_ASSERT(p >= 0.0 && p <= 1.0, "p-value in [0, 1] range");
}

// ============================================================================
// Test 7: t-distribution CDF and beta function correctness
// ============================================================================
static void testStatisticsMath() {
    printf("\n=== Test 7: Statistical Math Correctness ===\n");

    // 7a: tDistCDF(0, df) = 0.5 (symmetric)
    TEST_ASSERT_FLOAT_NEAR(tDistCDF(0.0, 10.0), 0.5, 0.001, "tDistCDF(0, 10) == 0.5");
    TEST_ASSERT_FLOAT_NEAR(tDistCDF(0.0, 100.0), 0.5, 0.001, "tDistCDF(0, 100) == 0.5");

    // 7b: tDistCDF positive t > 0.5
    TEST_ASSERT(tDistCDF(2.0, 10.0) > 0.5, "tDistCDF(2, 10) > 0.5");
    TEST_ASSERT(tDistCDF(2.0, 10.0) < 1.0, "tDistCDF(2, 10) < 1.0");

    // 7c: tDistCDF negative t < 0.5
    TEST_ASSERT(tDistCDF(-2.0, 10.0) < 0.5, "tDistCDF(-2, 10) < 0.5");

    // 7d: Symmetry: CDF(t) + CDF(-t) ≈ 1
    double cdfPos = tDistCDF(1.5, 20.0);
    double cdfNeg = tDistCDF(-1.5, 20.0);
    TEST_ASSERT_FLOAT_NEAR(cdfPos + cdfNeg, 1.0, 0.01, "tDistCDF symmetric: CDF(t)+CDF(-t)≈1");

    // 7e: Large t → CDF ≈ 1
    TEST_ASSERT(tDistCDF(10.0, 100.0) > 0.99, "tDistCDF(10, 100) > 0.99");

    // 7f: betaIncomplete boundary values
    TEST_ASSERT_FLOAT_NEAR(betaIncomplete(0.0, 1.0, 1.0), 0.0, 0.001, "betaIncomplete(0,...)==0");
    TEST_ASSERT_FLOAT_NEAR(betaIncomplete(1.0, 1.0, 1.0), 1.0, 0.001, "betaIncomplete(1,...)==1");

    // 7g: betaIncomplete(0.5, 1, 1) = 0.5 (uniform distribution)
    TEST_ASSERT_FLOAT_NEAR(betaIncomplete(0.5, 1.0, 1.0), 0.5, 0.01, "betaIncomplete(0.5,1,1)==0.5");

    // 7g2: betaIncomplete(0.5, 2, 2) = 0.5 (symmetric beta distribution)
    // I_0.5(2,2) = 6 * ∫₀^0.5 (t-t²)dt = 6 * (1/8 - 1/24) = 6 * 1/12 = 0.5
    TEST_ASSERT_FLOAT_NEAR(betaIncomplete(0.5, 2.0, 2.0), 0.5, 0.01, "betaIncomplete(0.5,2,2)==0.5");

    // 7g3: betaIncomplete(0.5, 1, 2) = 0.75
    // B(1,2)=1/2, ∫₀^0.5 (1-t)dt = 3/8, I = (3/8)/(1/2) = 0.75
    TEST_ASSERT_FLOAT_NEAR(betaIncomplete(0.5, 1.0, 2.0), 0.75, 0.01, "betaIncomplete(0.5,1,2)==0.75");

    // 7g4: betaIncomplete(0.5, 2, 1) = 0.25 (complement of 7g3)
    TEST_ASSERT_FLOAT_NEAR(betaIncomplete(0.5, 2.0, 1.0), 0.25, 0.01, "betaIncomplete(0.5,2,1)==0.25");

    // 7h: Known t-test value: t=1.962, df=60 → two-tailed p ≈ 0.05
    // Build stats that produce t≈1.96
    ABTestStats stats;
    stats.sampleCountA = 61;
    stats.sampleCountB = 61;
    stats.avgFrameTimeA = 17.0f;
    stats.avgFrameTimeB = 16.0f;
    stats.varianceA = 9.0f;   // se = sqrt(9/61 + 9/61) = sqrt(0.295) = 0.543
    stats.varianceB = 9.0f;   // t = 1.0 / 0.543 = 1.84
    double p = abTTest(stats, stats);
    printf("    [INFO] t≈1.84, df≈120 → two-tailed p=%.4f (expected ≈0.068)\n", p);
    TEST_ASSERT(p > 0.04 && p < 0.10, "p-value in expected range for t≈1.84");
}

// ============================================================================
// main
// ============================================================================
int main() {
    printf("===========================================================\n");
    printf("  AATPE AB Test Suite (独立第5测试套件)\n");
    printf("===========================================================\n");

    testRecordFrameTime();
    testDetermineWinner();
    testReset();
    testConfigToggle();
    testDataExport();
    testEdgeCases();
    testStatisticsMath();

    printf("\n===========================================================\n");
    printf("  AB TEST RESULTS: %d run, %d passed, %d failed\n",
           g_testsRun, g_testsPassed, g_testsFailed);
    printf("===========================================================\n");

    if (g_testsFailed > 0) {
        printf("  SOME TESTS FAILED!\n");
        return 1;
    }
    printf("  ALL AB TESTS PASSED\n");
    return 0;
}
