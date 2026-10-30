#include <cstdio>
#include <string>
#include "aatped.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== Control: installCrashHandlers ===\n");
    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.controlMode = "assist"; }
    installCrashHandlers();
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        CHECK(g_cfg.controlMode == "assist",
              "installCrashHandlers leaves controlMode unchanged (side-effect-free)");
    }

    printf("=== Control: applyHelperApkDetection on host ===\n");
    applyHelperApkDetection();
    CHECK(g_helperAutoSuppressed.load() == false,
          "no helper APK on host -> g_helperAutoSuppressed stays false");

    printf("=== Control: processControlCommands with no command ===\n");
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfg.controlMode = "assist";
    }
    processControlCommands();
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        CHECK(g_cfg.controlMode == "assist",
              "processControlCommands no-op leaves controlMode='assist' unchanged");
    }

    printf("=== Control G2-002: overflow set_thermal_limit_cpu rejected ===\n");
    {
        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.thermalLimitCpu = 50; }
        ControlResult r1 = executeControlLine("set_thermal_limit_cpu=9999999999");
        CHECK(r1.success == false, "set_thermal_limit_cpu=9999999999 rejected (no overflow collapse)");
        int after;
        { std::lock_guard<std::mutex> lk(g_cfgMutex); after = g_cfg.thermalLimitCpu; }
        CHECK(after == 50, "thermalLimitCpu unchanged after overflow rejection (still 50)");

        ControlResult r2 = executeControlLine("set_thermal_limit_cpu=120");
        CHECK(r2.success == true, "set_thermal_limit_cpu=120 accepted");
        { std::lock_guard<std::mutex> lk(g_cfgMutex); after = g_cfg.thermalLimitCpu; }
        CHECK(after == 120, "thermalLimitCpu==120 after valid set");
    }

    printf("=== Control G2-002: overflow on other lower-bound-0 commands ===\n");
    {
        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.confidenceThreshold = 30; }
        ControlResult r = executeControlLine("set_confidence_threshold=9999999999");
        CHECK(r.success == false, "set_confidence_threshold overflow rejected");
        int v; { std::lock_guard<std::mutex> lk(g_cfgMutex); v = g_cfg.confidenceThreshold; }
        CHECK(v == 30, "confidenceThreshold unchanged after overflow");

        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.migrationCostThreshold = 100; }
        r = executeControlLine("set_migration_cost_threshold=9999999999");
        CHECK(r.success == false, "set_migration_cost_threshold overflow rejected");
        { std::lock_guard<std::mutex> lk(g_cfgMutex); v = g_cfg.migrationCostThreshold; }
        CHECK(v == 100, "migrationCostThreshold unchanged after overflow");

        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.thermalLimitSkin = 40; g_cfg.thermalLimitBattery = 45; g_cfg.thermalEmergency = 120; }
        r = executeControlLine("set_thermal_limit_skin=9999999999");
        CHECK(r.success == false, "set_thermal_limit_skin overflow rejected");
        r = executeControlLine("set_thermal_limit_battery=9999999999");
        CHECK(r.success == false, "set_thermal_limit_battery overflow rejected");
        r = executeControlLine("set_thermal_emergency=9999999999");
        CHECK(r.success == false, "set_thermal_emergency overflow rejected");
        { std::lock_guard<std::mutex> lk(g_cfgMutex);
          CHECK(g_cfg.thermalLimitSkin == 40, "thermalLimitSkin unchanged");
          CHECK(g_cfg.thermalLimitBattery == 45, "thermalLimitBattery unchanged");
          CHECK(g_cfg.thermalEmergency == 120, "thermalEmergency unchanged after overflow"); }
    }

    printf("=== Control G2-003: boolean toggles reject overflow and non 0/1 ===\n");
    {
        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.mlHybridEnabled = false; }
        ControlResult r = executeControlLine("set_ml_hybrid=9999999999");
        CHECK(r.success == false, "set_ml_hybrid overflow rejected (not silent-disable)");
        bool b; { std::lock_guard<std::mutex> lk(g_cfgMutex); b = g_cfg.mlHybridEnabled; }
        CHECK(b == false, "mlHybridEnabled stays false after overflow rejection");

        r = executeControlLine("set_ml_hybrid=2");
        CHECK(r.success == false, "set_ml_hybrid=2 rejected (must be exactly 0/1)");

        r = executeControlLine("set_ml_hybrid=1");
        CHECK(r.success == true, "set_ml_hybrid=1 accepted");
        { std::lock_guard<std::mutex> lk(g_cfgMutex); b = g_cfg.mlHybridEnabled; }
        CHECK(b == true, "mlHybridEnabled==true after valid set");

        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.batteryAgingProtection = false; }
        r = executeControlLine("battery_aging=9999999999");
        CHECK(r.success == false, "battery_aging overflow rejected");
    }

    printf("=== Control CFG-39: set_ebpf handler accepts 0/1 and flips g_cfg ===\n");
    {
        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.ebpfEnabled = true; }
        ControlResult r0 = executeControlLine("set_ebpf=0");
        CHECK(r0.success == true, "set_ebpf=0 accepted");
        bool b;
        { std::lock_guard<std::mutex> lk(g_cfgMutex); b = g_cfg.ebpfEnabled; }
        CHECK(b == false, "g_cfg.ebpfEnabled==false after set_ebpf=0");

        ControlResult r1 = executeControlLine("set_ebpf=1");
        CHECK(r1.success == true, "set_ebpf=1 accepted");
        { std::lock_guard<std::mutex> lk(g_cfgMutex); b = g_cfg.ebpfEnabled; }
        CHECK(b == true, "g_cfg.ebpfEnabled==true after set_ebpf=1");

        ControlResult r2 = executeControlLine("set_ebpf=2");
        CHECK(r2.success == false, "set_ebpf=2 rejected (must be exactly 0/1)");
        ControlResult r3 = executeControlLine("set_ebpf=on");
        CHECK(r3.success == false, "set_ebpf=on rejected (not all digits)");
    }

    printf("\n========================================\n"
           "Control results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
