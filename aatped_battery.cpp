// AATPE - Battery / power monitoring (extracted from aatped_main.cpp)
#include "aatped.h"
#include <mutex>
#include <cstring>

// Power-aware scheduling implementation
// LED-094 (R7-009): g_currentPowerMw is written by readBatteryPower() on the
// main loop and read by computePowerPenalty() on the scoring/placement thread.
// Plain int was a cross-thread data race; make it atomic. (currentUa/voltageUv
// are fscanf targets consumed only within readBatteryPower() on the same thread,
// so they stay plain int.)
std::atomic<int> g_currentPowerMw{0};
// ROUND-014 P3: discharge-only magnitude mW (0 while charging). Used to gate the
// BIG-core penalty so charging (plentiful power) never raises it.
std::atomic<int> g_dischargePowerMw{0};
int g_batteryCurrentUa = 0;
int g_batteryVoltageUv = 0;

void readBatteryPower(void) {
    // Read current_now (microamps) and voltage_now (microvolts)
    // P2-15: check fscanf return value; on failure set to 0 so stale values
    // don't persist (the power calculation below would use garbage).
    g_batteryCurrentUa = 0;
    g_batteryVoltageUv = 0;
    // G6-007: try multiple power_supply names. Some devices (Exynos/Samsung/
    // custom kernels) expose current_now/voltage_now under "bms" or another supply
    // instead of "battery". First readable candidate wins.
    auto readBatteryNode = [](const char* name, int& out) -> bool {
        const char* supplies[] = { "battery", "bms", "battery/main" };
        for (const char* sup : supplies) {
            std::string p = std::string("/sys/class/power_supply/") + sup + "/" + name;
            FILE* fp = fopen(p.c_str(), "r");
            if (fp) {
                int v = 0;
                if (fscanf(fp, "%d", &v) == 1) { out = v; fclose(fp); return true; }
                fclose(fp);
            }
        }
        return false;
    };
    (void)readBatteryNode("current_now", g_batteryCurrentUa);
    (void)readBatteryNode("voltage_now", g_batteryVoltageUv);
    // Power = |I(A) * V(V)| in mW.
    // current_now: negative = discharging, positive = charging.
    // voltage_now: always positive.
    // Product is negative on discharge (I<0, V>0); take absolute value so the
    // power-consumption metric is always non-negative regardless of direction.
    // Conversion: I(uA)*V(uV) / 1e9 = mW.
    // ROUND-014 P3 (S14-11): preserve direction. signedMw>0 = charging, <0 = discharging.
    int signedMw = (int)((long long)g_batteryCurrentUa * g_batteryVoltageUv / 1000000000LL);
    g_currentPowerMw.store((signedMw < 0) ? -signedMw : signedMw, std::memory_order_relaxed);
    // discharge-only magnitude (0 while charging). Only heavy DISCHARGE justifies a
    // bigger-core penalty; charging means power is plentiful -> factor stays 1.0.
    g_dischargePowerMw.store((signedMw < 0) ? -signedMw : 0, std::memory_order_relaxed);
}

int computePowerPenalty(int cpuId, int threadImportance) {
    // Simple power model: BIG cores consume more power
    // P2-4: dynamic CPU pool instead of hardcoded cpuId>=6/>=4
    CpuPool pool = getPoolForCpu(cpuId);
    int basePower = (pool == CpuPool::BIG) ? 80 : (pool == CpuPool::MID) ? 40 : 20;
    // LED-094: atomic relaxed load (placement thread read of main-loop write).
    // ROUND-014 P3 (S14-11): only heavy DISCHARGE (not charging) raises the factor.
    float factor = (g_dischargePowerMw.load(std::memory_order_relaxed) > 3000) ? 1.5f : 1.0f;
    int penalty = (int)(basePower * factor);
    // Important threads get less penalty (performance优先).
    // THM-14: round half up instead of truncating odd penalties.
    if (threadImportance > 5) penalty = (penalty + 1) / 2;
    return penalty;
}

// Battery health implementation (v4.3.8 F2.1/F2.2/F2.3)
//
// -1 = unknown (read failed). WebUI shows "--" instead of defaulting to 100%.
// LED-093 (R7-008): g_batteryAged is read by getBatteryAgePenalty() on the
// scoring thread while readBatteryHealth() writes it on the main loop. The whole
// snapshot (health/cycle/soh/aged/overheat) is guarded by g_batteryStateMutex;
// readers snapshot under it.
std::mutex g_batteryStateMutex;
int g_batteryHealthPercent = -1;
int g_batteryCycleCount = -1;
int g_batterySohPercent = -1;       // F2.1: real SOH% from charge_full/design
bool g_batteryAged = false;          // F2.3: CHRONIC aging only (Overheat excluded)
bool g_batteryOverheat = false;      // F2.3: transient thermal state

// F2.1: probe a numeric sysfs node across candidate power_supply names.
// Some devices (Exynos/Samsung/custom kernels) expose charge_full/charge_full_design
// under "bms" or "battery/main" instead of "battery". First readable wins.
static bool readBatteryIntAcrossSupplies(const char* node, long long& out) {
    const char* supplies[] = { "battery", "bms", "battery/main" };
    for (const char* sup : supplies) {
        std::string p = std::string("/sys/class/power_supply/") + sup + "/" + node;
        FILE* fp = fopen(p.c_str(), "r");
        if (fp) {
            long long v = 0;
            if (fscanf(fp, "%lld", &v) == 1) { out = v; fclose(fp); return true; }
            fclose(fp);
        }
    }
    return false;
}

// F2.1: read the OEM health string across candidate supplies (same probe order as
// the numeric path so the health string and charge_full come from the same supply
// when both are readable; if a supply lacks the string we fall through).
static bool readBatteryHealthStrAcrossSupplies(char* buf, size_t bufsiz) {
    const char* supplies[] = { "battery", "bms", "battery/main" };
    for (const char* sup : supplies) {
        std::string p = std::string("/sys/class/power_supply/") + sup + "/health";
        FILE* fp = fopen(p.c_str(), "r");
        if (fp) {
            buf[0] = '\0';
            if (fgets(buf, bufsiz, fp) && buf[0] != '\0') { fclose(fp); return true; }
            fclose(fp);
        }
    }
    return false;
}

void readBatteryHealth(void) {
    // Build the fresh snapshot in locals, then publish under the lock so readers
    // never observe a partially-updated set (health/cycle/soh/aged/overheat).
    int health = -1;
    int cycles = -1;
    int soh = -1;
    bool overheat = false;

    // --- OEM health string (F2.3: kept for observability + Dead detection) ---
    char hbuf[32] = {0};
    if (readBatteryHealthStrAcrossSupplies(hbuf, sizeof(hbuf))) {
        if (strstr(hbuf, "Good"))       health = 100;
        else if (strstr(hbuf, "Overheat")) { health = 60; overheat = true; }
        else if (strstr(hbuf, "Dead"))   health = 20;
        else                              health = 80;
    }

    // --- cycle_count (numeric, same probe order) ---
    long long ccycles = 0;
    if (readBatteryIntAcrossSupplies("cycle_count", ccycles)) cycles = (int)ccycles;

    // --- F2.1: real State-of-Health from charge_full / charge_full_design ---
    long long chgFull = 0, chgDesign = 0;
    bool haveFull   = readBatteryIntAcrossSupplies("charge_full", chgFull);
    bool haveDesign = readBatteryIntAcrossSupplies("charge_full_design", chgDesign);
    if (haveFull && haveDesign && chgDesign > 0 && chgFull > 0) {
        // SOH% = charge_full / charge_full_design * 100. Both nodes are in the
        // same unit (µAh or µWh on a given device), so the ratio is unitless.
        long long sohLL = chgFull * 100LL / chgDesign;
        if (sohLL < 0)   sohLL = 0;
        if (sohLL > 100) sohLL = 100;  // calibration / new-battery overshoot
        soh = (int)sohLL;
    }

    // --- F2.3: chronic aging excludes transient Overheat ---
    // Aged = real SOH below the 80% EOL line, OR cycle_count above 500 (fallback
    // when SOH nodes are absent), OR OEM reports "Dead" (hard chronic failure).
    // Overheat alone does NOT set aged: it is a transient thermal state that the
    // thermal loop already handles; treating it as chronic wear would spuriously
    // raise the BIG-core penalty for the entire ~20s poll window.
    bool aged = false;
    if (soh >= 0 && soh < 80)               aged = true;
    else if (cycles >= 0 && cycles > 500)   aged = true;
    else if (health == 20 /*Dead*/)         aged = true;

    std::lock_guard<std::mutex> lk(g_batteryStateMutex);
    g_batteryHealthPercent = health;
    g_batteryCycleCount = cycles;
    g_batterySohPercent = soh;
    g_batteryAged = aged;
    g_batteryOverheat = overheat;
}

int getBatteryAgePenalty(int cpuId) {
    Config cfg; snapshotConfig(cfg);
    if (!cfg.batteryAgingProtection) return 0;

    // LED-093: snapshot the battery snapshot under the battery-state mutex
    // (scoring thread read vs main-loop write).
    int soh, cycles, health;
    {
        std::lock_guard<std::mutex> lk(g_batteryStateMutex);
        soh = g_batterySohPercent;
        cycles = g_batteryCycleCount;
        health = g_batteryHealthPercent;
    }

    // F2.2: continuous aging factor in [0,1] instead of the old binary
    // aged/not-aged step. The factor ramps the BIG/MID penalty linearly with
    // wear severity so a mildly-worn 75% SOH battery gets a small nudge while a
    // severely-degraded 30% SOH battery approaches the full protection.
    //
    // Factor sources (first available wins):
    //   1. Real SOH (F2.1):   soh>=80 -> 0.0 ; soh<=20 -> 1.0 ; linear between.
    //   2. cycle_count fallback (when SOH nodes absent): cycles<=500 -> 0.0 ;
    //      cycles>=1000 -> 1.0 ; linear between.
    //   3. OEM "Dead" (health==20): hard 1.0.
    //   4. Otherwise: 0.0.
    //
    // F2.3: Overheat does NOT contribute to the factor. If Overheat coincides
    // with chronic wear (soh<80), the factor comes from SOH (chronic); if it is
    // the only signal, the penalty is 0.
    float factor = 0.0f;
    if (soh >= 0) {
        // 80% EOL threshold -> 0; 20% -> 1.0.
        factor = (80.0f - (float)soh) / 60.0f;
        if (factor < 0.0f) factor = 0.0f;
        if (factor > 1.0f) factor = 1.0f;
    } else if (cycles >= 0 && cycles > 500) {
        factor = ((float)cycles - 500.0f) / 500.0f;
        if (factor > 1.0f) factor = 1.0f;
    } else if (health == 20 /*Dead*/) {
        factor = 1.0f;
    }

    if (factor <= 0.0f) return 0;

    // Aged battery: penalize BIG cores more to reduce stress.
    // P2-4: dynamic CPU pool instead of hardcoded cpuId>=6/>=4.
    // F2.2: scale the base penalty by the continuous factor (rounded).
    CpuPool pool = getPoolForCpu(cpuId);
    if (pool == CpuPool::BIG) return (int)(50.0f * factor);
    if (pool == CpuPool::MID) return (int)(20.0f * factor);
    return 0;
}

// ============================================================================
// v4.3.9: arbitrator battery signals (audit4_pipeline F1).
//
// The arbitrator needs (a) real charge percent and (b) charging state to drive
// the context core (low-battery / charging bias) and device_profile_observe().
// These are sampled from /sys/class/power_supply/<battery>/{capacity,status}
// with the same multi-supply probe order as readBatteryHealth. Best-effort:
// missing nodes leave the snapshot at its previous value (we publish a fresh
// snapshot each call so a node vanishing degrades to unknown rather than
// freezing a stale reading).
// ============================================================================
std::mutex g_arbBatteryMutex;
int g_arbBatteryCapacityPct = -1;
int g_arbBatteryCharging = 0;

// Read a numeric node across candidate supplies; returns false if none readable.
static bool arbReadSupplyInt(const char* node, long long& out) {
    const char* supplies[] = { "battery", "bms", "battery/main" };
    for (const char* sup : supplies) {
        std::string p = std::string("/sys/class/power_supply/") + sup + "/" + node;
        FILE* fp = fopen(p.c_str(), "r");
        if (fp) {
            long long v = 0;
            if (fscanf(fp, "%lld", &v) == 1) { out = v; fclose(fp); return true; }
            fclose(fp);
        }
    }
    return false;
}

// Read the status string across supplies; returns false if none readable.
static bool arbReadSupplyStatus(char* buf, size_t bufsiz) {
    const char* supplies[] = { "battery", "bms", "battery/main" };
    for (const char* sup : supplies) {
        std::string p = std::string("/sys/class/power_supply/") + sup + "/status";
        FILE* fp = fopen(p.c_str(), "r");
        if (fp) {
            buf[0] = '\0';
            if (fgets(buf, bufsiz, fp) && buf[0] != '\0') { fclose(fp); return true; }
            fclose(fp);
        }
    }
    return false;
}

void readArbBatteryState(void) {
    int capacity = -1;
    int charging = 0;

    long long v = 0;
    if (arbReadSupplyInt("capacity", v)) {
        if (v < 0) v = 0; else if (v > 100) v = 100;
        capacity = (int)v;
    }

    char sbuf[32] = {0};
    if (arbReadSupplyStatus(sbuf, sizeof(sbuf))) {
        // "Charging" or "Full" => on charger. "Discharging"/"Not charging"/
        // "Unknown" => on battery.
        if (strstr(sbuf, "Charging") || strstr(sbuf, "Full")) charging = 1;
    } else {
        // Fallback: AC/USB online nodes (some devices omit status).
        // THM-05: ac/usb are INDEPENDENT power_supply entries
        // (/sys/class/power_supply/ac/online, /sys/class/power_supply/usb/online),
        // NOT sub-nodes of "battery". Previously this probed
        // {battery,bms,battery/main}/ac/online which never exists on real devices.
        auto readFixedOnline = [](const char* path) -> long long {
            FILE* fp = fopen(path, "r");
            if (!fp) return -1;
            long long v = 0;
            if (fscanf(fp, "%lld", &v) != 1) v = -1;
            fclose(fp);
            return v;
        };
        if (readFixedOnline("/sys/class/power_supply/ac/online") > 0) charging = 1;
        else if (readFixedOnline("/sys/class/power_supply/usb/online") > 0) charging = 1;
    }

    std::lock_guard<std::mutex> lk(g_arbBatteryMutex);
    g_arbBatteryCapacityPct = capacity;
    g_arbBatteryCharging = charging;
}

void getArbBatterySnapshot(int& capacityPct, int& charging) {
    std::lock_guard<std::mutex> lk(g_arbBatteryMutex);
    capacityPct = g_arbBatteryCapacityPct;
    charging = g_arbBatteryCharging;
}
