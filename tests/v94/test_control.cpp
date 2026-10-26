// test_control.cpp - v94 hermetic unit tests for aatped_control.cpp
// Links aatped_control.cpp + aatpe_ml with v94_daemon_deps + log_stub.
// Covers: installCrashHandlers (installs without crash), applyHelperApkDetection
// (host: no helper apk -> graceful), processControlCommands (empty command path
// must not crash / must not block).
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
    // Snapshot config before install; installCrashHandlers() must be side-effect-free
    // on the running config (it only wires signal handlers). Assert it leaves the
    // observable controlMode untouched instead of a tautological CHECK(true).
    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.controlMode = "assist"; }
    installCrashHandlers();
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        CHECK(g_cfg.controlMode == "assist",
              "installCrashHandlers leaves controlMode unchanged (side-effect-free)");
    }

    printf("=== Control: applyHelperApkDetection on host ===\n");
    applyHelperApkDetection();  // no pm on host -> graceful
    // g_helperAutoSuppressed is an atomic<bool> the control TU consults; on the
    // host there is no helper APK so detection must leave it false (not crash,
    // not spuriously suppress).
    CHECK(g_helperAutoSuppressed.load() == false,
          "no helper APK on host -> g_helperAutoSuppressed stays false");

    printf("=== Control: processControlCommands with no command ===\n");
    // Snapshot the default control mode, run the command processor with no
    // control FIFO present, and verify the mode is UNCHANGED (a no-op must not
    // mutate g_cfg).
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfg.controlMode = "assist";
    }
    processControlCommands();  // no control fifo on host -> graceful no-op
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        CHECK(g_cfg.controlMode == "assist",
              "processControlCommands no-op leaves controlMode='assist' unchanged");
    }

    // ------------------------------------------------------------------
    // G2-002 / G2-003: overflowing / out-of-domain set_* values must be
    // REJECTED (res.success==false, g_cfg untouched) instead of collapsing to 0.
    // Before the fix, "9999999999" (10 digits, passes isAllDigits) was parsed by
    // parseIntSafe() into 0, which then passed the 0<=t<=RANGE check and wrote 0
    // (e.g. thermalLimitCpu=0 -> permanent throttling).
    // ------------------------------------------------------------------
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
        // confidence_threshold
        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.confidenceThreshold = 30; }
        ControlResult r = executeControlLine("set_confidence_threshold=9999999999");
        CHECK(r.success == false, "set_confidence_threshold overflow rejected");
        int v; { std::lock_guard<std::mutex> lk(g_cfgMutex); v = g_cfg.confidenceThreshold; }
        CHECK(v == 30, "confidenceThreshold unchanged after overflow");

        // migration_cost_threshold
        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.migrationCostThreshold = 100; }
        r = executeControlLine("set_migration_cost_threshold=9999999999");
        CHECK(r.success == false, "set_migration_cost_threshold overflow rejected");
        { std::lock_guard<std::mutex> lk(g_cfgMutex); v = g_cfg.migrationCostThreshold; }
        CHECK(v == 100, "migrationCostThreshold unchanged after overflow");

        // thermal_limit_skin / battery / emergency
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

        // "2" used to be accepted as true; now must be rejected.
        r = executeControlLine("set_ml_hybrid=2");
        CHECK(r.success == false, "set_ml_hybrid=2 rejected (must be exactly 0/1)");

        r = executeControlLine("set_ml_hybrid=1");
        CHECK(r.success == true, "set_ml_hybrid=1 accepted");
        { std::lock_guard<std::mutex> lk(g_cfgMutex); b = g_cfg.mlHybridEnabled; }
        CHECK(b == true, "mlHybridEnabled==true after valid set");

        // battery_aging overflow
        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.batteryAgingProtection = false; }
        r = executeControlLine("battery_aging=9999999999");
        CHECK(r.success == false, "battery_aging overflow rejected");
    }

    // ------------------------------------------------------------------
    // CFG-39: set_ebpf= handler. After the fix the handler persists BOTH
    // EBPF_EVENT_DRIVEN and EBPF_ENABLED to the same value so the two keys
    // cannot diverge on the next hot-reload. The control test links the stub
    // of persistConfigKeyValue (no-op), so we verify the handler accepts the
    // value and flips g_cfg.ebpfEnabled; the conf-file dual-key sync is
    // verified end-to-end in test_config.cpp (real persistConfigKeyValue).
    // ------------------------------------------------------------------
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

        // Invalid values rejected.
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
