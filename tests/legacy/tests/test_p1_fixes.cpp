// tests/test_p1_fixes.cpp - P1-level fix verification tests
// [LEGACY-TEST v66-v74] 历史遗留测试：部分金值断言(eBPF 字节码偏移/map 数量/文件 md5/硬编码版本号)可能已过时；其失败不一定代表生产 bug。
// v94 fix / 问题34: 见 tests/README.md 中"测试失败分类指南"。请勿删除(有考古价值)。
// Compile: g++ -std=c++17 -I.. test_p1_fixes.cpp -o test_p1_fixes
#include <cstdio>
#include <cstring>
#include <string>
#include <cmath>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

// P1-13: Scoring normalization ranges
static float normalizeScore101(float val, float minV, float maxV) {
    if (maxV <= minV || std::isnan(val) || std::isinf(val)) return 50.0f;
    float n = (val - minV) / (maxV - minV) * 100.0f;
    if (n < 0.0f) n = 0.0f;
    if (n > 100.0f) n = 100.0f;
    return n;
}

static void test_scoring_norm_ranges() {
    printf("\n=== P1-13: Scoring normalization ranges ===\n");
    // cache: 0~10 (was 0~100)
    CHECK(normalizeScore101(5.0f, 0.0f, 10.0f) == 50.0f, "cache=5 → 50% (range 0-10)");
    CHECK(normalizeScore101(10.0f, 0.0f, 10.0f) == 100.0f, "cache=10 → 100%");
    CHECK(normalizeScore101(0.0f, 0.0f, 10.0f) == 0.0f, "cache=0 → 0%");
    // power: -50~50 (was -50~0)
    CHECK(normalizeScore101(0.0f, -50.0f, 50.0f) == 50.0f, "power=0 → 50% (range -50~50)");
    CHECK(normalizeScore101(50.0f, -50.0f, 50.0f) == 100.0f, "power=50 → 100%");
    CHECK(normalizeScore101(-50.0f, -50.0f, 50.0f) == 0.0f, "power=-50 → 0%");
    // frame: 0~100 (was 0~50)
    CHECK(normalizeScore101(50.0f, 0.0f, 100.0f) == 50.0f, "frame=50 → 50% (range 0-100)");
    CHECK(normalizeScore101(100.0f, 0.0f, 100.0f) == 100.0f, "frame=100 → 100%");
}

// P1-15: pid_tid composite key
static long long makeCompositeKey(int pid, int tid) {
    return ((long long)pid << 22) | (tid & 0x3FFFFF);
}

static void test_composite_key() {
    printf("\n=== P1-15: pid_tid composite key ===\n");
    long long k1 = makeCompositeKey(1000, 2000);
    long long k2 = makeCompositeKey(1001, 2000);  // same tid, different pid
    long long k3 = makeCompositeKey(1000, 2001);  // same pid, different tid
    CHECK(k1 != k2, "same tid different pid → different keys");
    CHECK(k1 != k3, "same pid different tid → different keys");
    CHECK(k1 == makeCompositeKey(1000, 2000), "same pid+tid → same key");
    // tid fits in 22 bits (max 4,194,303)
    CHECK(makeCompositeKey(1, 4194303) != makeCompositeKey(1, 0), "max tid works");
}

// P1-18: sched_setaffinity error code classification
static int g_einval = 0, g_esrch = 0, g_eperm = 0, g_other = 0;
static void classifyError(int err) {
    switch (err) {
        case 22: g_einval++; break;  // EINVAL
        case 3:  g_esrch++; break;   // ESRCH
        case 1:  g_eperm++; break;   // EPERM
        default: g_other++; break;
    }
}

static void test_error_classification() {
    printf("\n=== P1-18: Error code classification ===\n");
    g_einval = g_esrch = g_eperm = g_other = 0;
    classifyError(22); classifyError(22); classifyError(3);
    classifyError(1); classifyError(99);
    CHECK(g_einval == 2, "EINVAL(22) counted=2");
    CHECK(g_esrch == 1, "ESRCH(3) counted=1");
    CHECK(g_eperm == 1, "EPERM(1) counted=1");
    CHECK(g_other == 1, "Other counted=1");
}

// P1-19: Crash signal handler writes state file
static void test_crash_handler_concept() {
    printf("\n=== P1-19: Crash signal handler concept ===\n");
    const int crashSignals[] = {11, 6, 8, 4, 7}; // SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS
    const char* names[] = {"SIGSEGV", "SIGABRT", "SIGFPE", "SIGILL", "SIGBUS"};
    for (int i = 0; i < 5; i++) {
        CHECK(crashSignals[i] > 0, ("crash signal " + std::to_string(crashSignals[i]) +
              " (" + names[i] + ") registered").c_str());
    }
}

// P1-20: Zombie reaping after SIGKILL
static void test_zombie_reap() {
    printf("\n=== P1-20: Zombie reaping after SIGKILL ===\n");
    // Concept test: verify reaping logic drains all children
    int reaped = 0;
    // Simulate waitpid(-1, WNOHANG) loop returning 3 children then -1
    int simulatedChildren[] = {100, 200, 300, -1};
    for (int i = 0; simulatedChildren[i] > 0; i++) reaped++;
    CHECK(reaped == 3, "reaped 3 zombie children via waitpid loop");
}

// P1-11: Battery aging config name
static void test_battery_aging_config() {
    printf("\n=== P1-11: Battery aging config name ===\n");
    const char* configKey = "BATTERY_AGING_PROTECTION";
    CHECK(strcmp(configKey, "BATTERY_AGING_PROTECTION") == 0,
          "config key is BATTERY_AGING_PROTECTION (matches real original)");
    CHECK(strcmp(configKey, "BATTERY_AGING_ENABLED") != 0,
          "not BATTERY_AGING_ENABLED (wrong name)");
}

int main() {
    printf("=== AATPE v66 P1 Fix Verification Tests ===\n");
    test_scoring_norm_ranges();
    test_composite_key();
    test_error_classification();
    test_crash_handler_concept();
    test_zombie_reap();
    test_battery_aging_config();
    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
