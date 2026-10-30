#include <cstdio>
#include <cmath>
#include "aatped_abtest.h"
#include "aatped.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static long long g_fakeNowMs = 0;
static long long fakeNowMs() { return g_fakeNowMs; }

int main() {
    printf("=== ABTest: assignThread returns 0 or 1 only ===\n");
    bool saw0 = false, saw1 = false;
    for (int t = 1; t <= 200; t++) {
        int g = abTestAssignThread(t);
        if (g == 0) saw0 = true; else if (g == 1) saw1 = true;
        CHECK(g == 0 || g == 1, "assignThread in {0,1}");
    }
    CHECK(saw0 && saw1, "assignment covers both groups over many tids");

    printf("=== ABTest: recordFrameTime accumulates means ===\n");
    abTestSetClockForTest(fakeNowMs);
    g_fakeNowMs = 1000000;
    g_abStats = ABTestStats{};
    for (int i = 0; i < 100; i++) abTestRecordFrameTime(1, 10.0f + (float)((i % 3) - 1) * 1.0f);
    for (int i = 0; i < 100; i++) abTestRecordFrameTime(2, 14.0f + (float)((i % 3) - 1) * 1.0f);
    CHECK(g_abStats.sampleCountA == 100, "group A sample count == 100");
    CHECK(g_abStats.sampleCountB == 100, "group B sample count == 100");
    CHECK(std::fabs(g_abStats.avgFrameTimeA - 10.0f) < 0.5f, "group A mean ~10ms");
    CHECK(std::fabs(g_abStats.avgFrameTimeB - 14.0f) < 0.5f, "group B mean ~14ms");
    CHECK(g_abStats.varianceA > 0.0f, "group A sample variance > 0 (Welford fix)");
    CHECK(g_abStats.varianceB > 0.0f, "group B sample variance > 0 (Welford fix)");
    CHECK(g_abStats.startMonotonicMs == 1000000, "experiment start stamped on first sample");

    printf("=== ABTest: recordJank / recordMigration counters ===\n");
    for (int i = 0; i < 7; i++) abTestRecordJank(1);
    for (int i = 0; i < 3; i++) abTestRecordJank(2);
    abTestRecordMigration(1);
    abTestRecordMigration(2);
    abTestRecordMigration(2);
    CHECK(g_abStats.jankCountA == 7, "jank A counted");
    CHECK(g_abStats.jankCountB == 3, "jank B counted");
    CHECK(g_abStats.migrationsA == 1, "migration A counted");
    CHECK(g_abStats.migrationsB == 2, "migration B counted");

    printf("=== ABTest ML-04: duration gate blocks a premature winner ===\n");
    CHECK(abTestDetermineWinner() == -1, "<10 min elapsed -> no winner despite 100/100");
    g_fakeNowMs += 599000;
    CHECK(abTestDetermineWinner() == -1, "599 s elapsed -> still no winner (gate is 600 s)");
    g_fakeNowMs += 2000;
    int w = abTestDetermineWinner();
    CHECK(w == 0 || w == 1, "after 10-min gate: determineWinner returns a decided group");
    CHECK(w == 0, "A faster (~10ms) than B (~14ms) -> A wins (w==0)");

    printf("=== ABTest: insufficient samples -> no decision (-1) ===\n");
    g_abStats.sampleCountA = 5; g_abStats.sampleCountB = 5;
    CHECK(abTestDetermineWinner() == -1, "<100 samples -> no winner (-1)");

    printf("=== ABTest: unknown group is ignored ===\n");
    long long decBefore = g_abStats.decisionsA;
    abTestRecordFrameTime(99, 999.0f);
    abTestRecordJank(99);
    abTestRecordMigration(99);
    CHECK(g_abStats.decisionsA == decBefore, "unknown group id -> no counters touched");

    printf("=== ABTest G6-017: deterministic across 'restarts' ===\n");
    bool deterministic = true;
    for (int t = 1; t <= 500; t++) {
        if (abTestAssignThread(t) != abTestAssignThread(t)) { deterministic = false; break; }
    }
    CHECK(deterministic, "abTestAssignThread(tid) stable across restarts");

    printf("=== ABTest G6-017: byUid stable & bounded ===\n");
    bool uidStable = true;
    for (int u = 1000; u <= 1100; u++) {
        if (abTestAssignByUid(u) != abTestAssignByUid(u)) { uidStable = false; break; }
    }
    CHECK(uidStable, "abTestAssignByUid(uid) reproducible");
    bool bounded = true;
    for (int u = 1000; u < 2000; u++) {
        int g = abTestAssignByUid(u);
        if (g != 0 && g != 1) { bounded = false; break; }
    }
    CHECK(bounded, "abTestAssignByUid returns 0/1 only");

    printf("=== ABTest G6-017: ~50/50 split over a uid range ===\n");
    long long cnt0 = 0, cnt1 = 0;
    for (int u = 10000; u < 20000; u++) {
        if (abTestAssignByUid(u) == 0) cnt0++; else cnt1++;
    }
    double ratio = (double)cnt0 / (cnt0 + cnt1);
    CHECK(ratio > 0.40 && ratio < 0.60, "uid split near 50/50");

    printf("\n========================================\n"
           "ABTest results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
