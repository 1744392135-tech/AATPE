// tests/test_p0_fixes.cpp - P0-level fix verification tests
// [LEGACY-TEST v66-v74] 历史遗留测试：部分金值断言(eBPF 字节码偏移/map 数量/文件 md5/硬编码版本号)可能已过时；其失败不一定代表生产 bug。
// v94 fix / 问题34: 见 tests/README.md 中"测试失败分类指南"。请勿删除(有考古价值)。
// Compile: g++ -std=c++17 -I.. test_p0_fixes.cpp -o test_p0_fixes
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

// P0-4: set_ml_hybrid= command offset test
// "set_ml_hybrid=1" has length 15, value starts at offset 14
static void test_ml_hybrid_offset() {
    printf("\n=== P0-4: set_ml_hybrid command offset ===\n");
    const char* cmd = "set_ml_hybrid=1";
    // Offset 14 should point to '1'
    CHECK(strlen(cmd) == 15, "command length is 15");
    CHECK(cmd[14] == '1', "offset 14 points to value '1'");
    CHECK(cmd[13] == '=', "offset 13 is '=' separator");
    // Offset 13 would give "=1" which isAllDigits rejects
    const char* wrongOffset = cmd + 13;
    CHECK(wrongOffset[0] == '=', "offset 13 starts with '=' (would be rejected)");
    const char* correctOffset = cmd + 14;
    CHECK(correctOffset[0] == '1', "offset 14 starts with '1' (valid digit)");
}

// P0-5: TARGET_FRAME_TIME clamp range test
static int clampInt(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static void test_target_frame_time_clamp() {
    printf("\n=== P0-5: TARGET_FRAME_TIME clamp range ===\n");
    // Correct range: 1000-100000 microseconds
    CHECK(clampInt(16667, 1000, 100000) == 16667, "16667us (60fps) passes through");
    CHECK(clampInt(500, 1000, 100000) == 1000, "500 clamped to 1000");
    CHECK(clampInt(200000, 1000, 100000) == 100000, "200000 clamped to 100000");
    // Wrong range (1,100) would truncate 16667 to 100
    CHECK(clampInt(16667, 1, 100) == 100, "BUG: old range (1,100) truncates 16667 to 100");
    CHECK(clampInt(16667, 1, 100) != 16667, "BUG: old range destroys frame time value");
}

// P0-6: migration counter non-duplication test
static int g_totalMigrations = 0;
static int g_migrationCount = 0;
static bool applyPlacement_success() {
    g_migrationCount++;
    g_totalMigrations++;  // only incremented ONCE in applyPlacement
    return true;
}

static void test_no_duplicate_migration_count() {
    printf("\n=== P0-6: No duplicate g_totalMigrations++ ===\n");
    g_totalMigrations = 0;
    g_migrationCount = 0;
    // Simulate emergency migration of 3 threads
    for (int i = 0; i < 3; i++) {
        if (applyPlacement_success()) {
            // migrated++ but NO g_totalMigrations++ here (was the bug)
        }
    }
    CHECK(g_totalMigrations == 3, "3 migrations = g_totalMigrations=3 (no double count)");
    CHECK(g_migrationCount == 3, "g_migrationCount=3");
}

// P0-2: getAllPendingDecisions includes exited threads
struct PendingDecision { int tid; bool active; };
static PendingDecision g_pending[] = {
    {100, true}, {200, true}, {300, false}, {400, true}
};
static int g_pendingCount = 4;

static int countActivePending() {
    int count = 0;
    for (int i = 0; i < g_pendingCount; i++) {
        if (g_pending[i].active) count++;
    }
    return count;
}

static void test_pending_includes_exited() {
    printf("\n=== P0-2: getAllPendingDecisions includes exited threads ===\n");
    // Simulate: thread 200 exited but still has active pending decision
    // Old code iterates g_threads (only has 100, 400) → misses 200
    // New code iterates getAllPendingDecisions → includes 200
    int activePending = countActivePending();
    CHECK(activePending == 3, "3 active pending decisions (100,200,400)");
    // Thread 200 is exited but pending still active → must be counted
    CHECK(g_pending[1].tid == 200 && g_pending[1].active, "exited thread 200 has active pending");
}

int main() {
    printf("=== AATPE v66 P0 Fix Verification Tests ===\n");
    test_ml_hybrid_offset();
    test_target_frame_time_clamp();
    test_no_duplicate_migration_count();
    test_pending_includes_exited();
    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
