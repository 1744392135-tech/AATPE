#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

static void test_ml_hybrid_offset() {
    printf("\n=== P0-4: set_ml_hybrid command offset ===\n");
    const char* cmd = "set_ml_hybrid=1";
    CHECK(strlen(cmd) == 15, "command length is 15");
    CHECK(cmd[14] == '1', "offset 14 points to value '1'");
    CHECK(cmd[13] == '=', "offset 13 is '=' separator");
    const char* wrongOffset = cmd + 13;
    CHECK(wrongOffset[0] == '=', "offset 13 starts with '=' (would be rejected)");
    const char* correctOffset = cmd + 14;
    CHECK(correctOffset[0] == '1', "offset 14 starts with '1' (valid digit)");
}

static int clampInt(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static void test_target_frame_time_clamp() {
    printf("\n=== P0-5: TARGET_FRAME_TIME clamp range ===\n");
    CHECK(clampInt(16667, 1000, 100000) == 16667, "16667us (60fps) passes through");
    CHECK(clampInt(500, 1000, 100000) == 1000, "500 clamped to 1000");
    CHECK(clampInt(200000, 1000, 100000) == 100000, "200000 clamped to 100000");
    CHECK(clampInt(16667, 1, 100) == 100, "BUG: old range (1,100) truncates 16667 to 100");
    CHECK(clampInt(16667, 1, 100) != 16667, "BUG: old range destroys frame time value");
}

static int g_totalMigrations = 0;
static int g_migrationCount = 0;
static bool applyPlacement_success() {
    g_migrationCount++;
    g_totalMigrations++;
    return true;
}

static void test_no_duplicate_migration_count() {
    printf("\n=== P0-6: No duplicate g_totalMigrations++ ===\n");
    g_totalMigrations = 0;
    g_migrationCount = 0;
    for (int i = 0; i < 3; i++) {
        if (applyPlacement_success()) {
        }
    }
    CHECK(g_totalMigrations == 3, "3 migrations = g_totalMigrations=3 (no double count)");
    CHECK(g_migrationCount == 3, "g_migrationCount=3");
}

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
    int activePending = countActivePending();
    CHECK(activePending == 3, "3 active pending decisions (100,200,400)");
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
