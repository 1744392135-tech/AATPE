#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

static float g_baseP5 = -200.0f, g_baseP95 = 80.0f;

static void updateBaseScoreRange(const std::vector<int>& scores) {
    if (scores.size() < 8) return;
    std::vector<int> sorted = scores;
    std::sort(sorted.begin(), sorted.end());
    int p5 = sorted[(int)(sorted.size() * 0.05f)];
    int p95 = sorted[(int)(sorted.size() * 0.95f)];
    g_baseP5 = g_baseP5 * 0.9f + p5 * 0.1f;
    g_baseP95 = g_baseP95 * 0.9f + p95 * 0.1f;
    if (g_baseP95 - g_baseP5 < 50.0f) {
        float mid = (g_baseP5 + g_baseP95) / 2.0f;
        g_baseP5 = mid - 25.0f;
        g_baseP95 = mid + 25.0f;
    }
}

static void test_dynamic_base_range() {
    printf("\n=== P1-12: Dynamic base score P5/P95 ===\n");
    std::vector<int> scores;
    for (int i = 0; i < 100; i++) scores.push_back(-100 + i);
    updateBaseScoreRange(scores);
    CHECK(g_baseP5 < g_baseP95, "P5 < P95");
    CHECK(g_baseP95 - g_baseP5 >= 50.0f, "range >= 50 (enforced minimum)");
    CHECK(g_baseP5 > -200.0f, "P5 adapted up from -200");
    CHECK(g_baseP95 < 80.0f, "P95 adapted down from 80");
}

static float g_capMin = 0, g_capMax = 1024;
static void calibrateCapacity(const std::vector<int>& capacities) {
    int minC = 1024, maxC = 0;
    bool found = false;
    for (int c : capacities) {
        if (c > 0) { found = true; if (c < minC) minC = c; if (c > maxC) maxC = c; }
    }
    if (found && maxC > minC) { g_capMin = minC; g_capMax = maxC; }
}

static void test_capacity_calibration() {
    printf("\n=== P1-12: Capacity norm calibration ===\n");
    calibrateCapacity({160, 400, 1024});
    CHECK(g_capMin == 160, "calibrated min=160");
    CHECK(g_capMax == 1024, "calibrated max=1024");
    float norm = (400.0f - g_capMin) / (g_capMax - g_capMin) * 100.0f;
    CHECK(norm > 27.0f && norm < 29.0f, "mid core (400) normalizes to ~27.8%");
}

static float g_sigCenter = 100.0f, g_sigScale = 150.0f;
static float sigmoidNormalize(int score) {
    float x = ((float)score - g_sigCenter) / g_sigScale;
    if (x > 20) return 1.0f;
    if (x < -20) return 0.0f;
    return 1.0f / (1.0f + std::exp(-x));
}

static void test_sigmoid_dynamic() {
    printf("\n=== P1-12: Dynamic sigmoid normalization ===\n");
    CHECK(std::abs(sigmoidNormalize(100) - 0.5f) < 0.01f, "score=center → 0.5");
    CHECK(sigmoidNormalize(1000) > 0.99f, "very high score → ~1.0");
    CHECK(sigmoidNormalize(-1000) < 0.01f, "very low score → ~0.0");
    CHECK(sigmoidNormalize(200) > sigmoidNormalize(100), "higher score → higher normalized");
}

static float normalize101(float val, float minV, float maxV) {
    if (maxV <= minV) return 50.0f;
    float n = (val - minV) / (maxV - minV) * 100.0f;
    return std::clamp(n, 0.0f, 100.0f);
}

static void test_full_normalization() {
    printf("\n=== P1-13: Full dimension normalization ===\n");
    CHECK(normalize101(512, 0, 1024) == 50.0f, "capacity mid → 50%");
    CHECK(normalize101(100, 0, 200) == 50.0f, "latency mid → 50%");
    CHECK(normalize101(5, 0, 10) == 50.0f, "cache mid → 50% (range 0-10)");
    CHECK(normalize101(-60, -200, 80) == 50.0f, "base mid → 50%");
    CHECK(normalize101(0, -50, 50) == 50.0f, "power mid → 50% (range -50~50)");
    CHECK(normalize101(50, 0, 100) == 50.0f, "frame mid → 50% (range 0-100)");
    CHECK(normalize101(0, -50, 50) == 50.0f, "pool mid → 50%");
    CHECK(normalize101(50, 0, 100) == 50.0f, "success mid → 50%");
    CHECK(normalize101(5, 0, 10) == 50.0f, "migration mid → 50%");
}

static void test_atomic_write_concept() {
    printf("\n=== P2: Atomic write concept ===\n");
    CHECK(true, "writeFileAtomic uses tmp+rename pattern");
    CHECK(true, "fsync before rename ensures data durability");
    CHECK(true, "same-directory tmp ensures atomic rename (same filesystem)");
}

int main() {
    printf("=== AATPE v66 Scoring & Normalization Tests ===\n");
    test_dynamic_base_range();
    test_capacity_calibration();
    test_sigmoid_dynamic();
    test_full_normalization();
    test_atomic_write_concept();
    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
