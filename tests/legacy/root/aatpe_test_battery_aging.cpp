#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

enum class CpuPool {
    UNKNOWN = -1,
    LITTLE = 0,
    MID = 1,
    BIG = 2
};

struct Config {
    bool batteryAgingProtection = false;
};

Config g_cfg;
bool g_batteryAged = false;
std::vector<int> g_littlePool = {0, 1, 2, 3};
std::vector<int> g_midPool    = {4, 5, 6};
std::vector<int> g_bigPool    = {7};

CpuPool getPoolForCpu(int cpu) {
    if (cpu < 0) return CpuPool::UNKNOWN;
    if (std::find(g_littlePool.begin(), g_littlePool.end(), cpu) != g_littlePool.end())
        return CpuPool::LITTLE;
    if (std::find(g_midPool.begin(), g_midPool.end(), cpu) != g_midPool.end())
        return CpuPool::MID;
    if (std::find(g_bigPool.begin(), g_bigPool.end(), cpu) != g_bigPool.end())
        return CpuPool::BIG;
    return CpuPool::UNKNOWN;
}

int getBatteryAgePenalty(int cpuId) {
    if (!g_cfg.batteryAgingProtection) return 0;
    if (!g_batteryAged) return 0;
    CpuPool pool = getPoolForCpu(cpuId);
    if (pool == CpuPool::BIG) return 50;
    if (pool == CpuPool::MID) return 20;
    return 0;
}

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

#define ASSERT_EQ(actual, expected, msg) do { \
    int _a = (actual), _e = (expected); \
    if (_a == _e) { g_pass++; printf("  [PASS] %s (got %d)\n", msg, _a); } \
    else { g_fail++; printf("  [FAIL] %s (got %d, expected %d)\n", msg, _a, _e); } \
} while(0)

static void test_protection_disabled_aged_battery() {
    printf("\n=== Test: Protection DISABLED + Aged Battery (must return 0) ===\n");
    g_cfg.batteryAgingProtection = false;
    g_batteryAged = true;
    ASSERT_EQ(getBatteryAgePenalty(7), 0, "BIG core penalty = 0 when disabled");
    ASSERT_EQ(getBatteryAgePenalty(6), 0, "MID core penalty = 0 when disabled");
    ASSERT_EQ(getBatteryAgePenalty(4), 0, "MID core (cpu4) penalty = 0 when disabled");
    ASSERT_EQ(getBatteryAgePenalty(0), 0, "LITTLE core penalty = 0 when disabled");
    ASSERT_EQ(getBatteryAgePenalty(3), 0, "LITTLE core (cpu3) penalty = 0 when disabled");
    ASSERT_EQ(getBatteryAgePenalty(-1), 0, "Unknown cpu penalty = 0 when disabled");
}

static void test_protection_enabled_aged_battery() {
    printf("\n=== Test: Protection ENABLED + Aged Battery (correct penalties) ===\n");
    g_cfg.batteryAgingProtection = true;
    g_batteryAged = true;
    ASSERT_EQ(getBatteryAgePenalty(7), 50, "BIG core penalty = 50");
    ASSERT_EQ(getBatteryAgePenalty(6), 20, "MID core penalty = 20");
    ASSERT_EQ(getBatteryAgePenalty(5), 20, "MID core (cpu5) penalty = 20");
    ASSERT_EQ(getBatteryAgePenalty(4), 20, "MID core (cpu4) penalty = 20");
    ASSERT_EQ(getBatteryAgePenalty(0), 0, "LITTLE core penalty = 0");
    ASSERT_EQ(getBatteryAgePenalty(3), 0, "LITTLE core (cpu3) penalty = 0");
    ASSERT_EQ(getBatteryAgePenalty(-1), 0, "Unknown cpu penalty = 0");
}

static void test_protection_enabled_healthy_battery() {
    printf("\n=== Test: Protection ENABLED + Healthy Battery (must return 0) ===\n");
    g_cfg.batteryAgingProtection = true;
    g_batteryAged = false;
    ASSERT_EQ(getBatteryAgePenalty(7), 0, "BIG core penalty = 0 when healthy");
    ASSERT_EQ(getBatteryAgePenalty(6), 0, "MID core penalty = 0 when healthy");
    ASSERT_EQ(getBatteryAgePenalty(0), 0, "LITTLE core penalty = 0 when healthy");
}

static void test_protection_disabled_healthy_battery() {
    printf("\n=== Test: Protection DISABLED + Healthy Battery (must return 0) ===\n");
    g_cfg.batteryAgingProtection = false;
    g_batteryAged = false;
    ASSERT_EQ(getBatteryAgePenalty(7), 0, "BIG core penalty = 0 when disabled+healthy");
    ASSERT_EQ(getBatteryAgePenalty(6), 0, "MID core penalty = 0 when disabled+healthy");
    ASSERT_EQ(getBatteryAgePenalty(0), 0, "LITTLE core penalty = 0 when disabled+healthy");
}

static void test_toggle_transition() {
    printf("\n=== Test: Toggle Transition (enabled -> disabled immediate effect) ===\n");
    g_cfg.batteryAgingProtection = true;
    g_batteryAged = true;
    int before = getBatteryAgePenalty(7);
    ASSERT_EQ(before, 50, "BIG penalty = 50 before toggle off");
    g_cfg.batteryAgingProtection = false;
    int after = getBatteryAgePenalty(7);
    ASSERT_EQ(after, 0, "BIG penalty = 0 immediately after toggle off");
    g_cfg.batteryAgingProtection = true;
    int restored = getBatteryAgePenalty(7);
    ASSERT_EQ(restored, 50, "BIG penalty = 50 after toggle back on");
}

static void test_default_config_disabled() {
    printf("\n=== Test: Default Config (batteryAgingProtection = false) ===\n");
    Config defaultCfg;
    ASSERT(defaultCfg.batteryAgingProtection == false, "Default batteryAgingProtection is false (matches aatpe.conf BATTERY_AGING_PROTECTION=0)");
}

int main() {
    printf("===========================================================\n");
    printf("  AATPE Battery Aging Protection Unit Test\n");
    printf("===========================================================\n");

    test_default_config_disabled();
    test_protection_disabled_aged_battery();
    test_protection_enabled_aged_battery();
    test_protection_enabled_healthy_battery();
    test_protection_disabled_healthy_battery();
    test_toggle_transition();

    printf("\n===========================================================\n");
    printf("  RESULTS: %d passed, %d failed\n", g_pass, g_fail);
    printf("===========================================================\n");
    return (g_fail == 0) ? 0 : 1;
}
