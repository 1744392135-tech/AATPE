#include "aatped.h"
#include <mutex>
#include <cstring>

std::atomic<int> g_currentPowerMw{0};
std::atomic<int> g_dischargePowerMw{0};
int g_batteryCurrentUa = 0;
int g_batteryVoltageUv = 0;

void readBatteryPower(void) {
    g_batteryCurrentUa = 0;
    g_batteryVoltageUv = 0;
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
    int signedMw = (int)((long long)g_batteryCurrentUa * g_batteryVoltageUv / 1000000000LL);
    g_currentPowerMw.store((signedMw < 0) ? -signedMw : signedMw, std::memory_order_relaxed);
    g_dischargePowerMw.store((signedMw < 0) ? -signedMw : 0, std::memory_order_relaxed);
}

int computePowerPenalty(int cpuId, int threadImportance) {
    CpuPool pool = getPoolForCpu(cpuId);
    int basePower = (pool == CpuPool::BIG) ? 80 : (pool == CpuPool::MID) ? 40 : 20;
    float factor = (g_dischargePowerMw.load(std::memory_order_relaxed) > 3000) ? 1.5f : 1.0f;
    int penalty = (int)(basePower * factor);
    if (threadImportance > 5) penalty = (penalty + 1) / 2;
    return penalty;
}

std::mutex g_batteryStateMutex;
int g_batteryHealthPercent = -1;
int g_batteryCycleCount = -1;
int g_batterySohPercent = -1;
bool g_batteryAged = false;
bool g_batteryOverheat = false;
static constexpr int kAgedSohMinCyclesToTrust = 200;

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
    int health = -1;
    int cycles = -1;
    int soh = -1;
    bool overheat = false;

    char hbuf[32] = {0};
    if (readBatteryHealthStrAcrossSupplies(hbuf, sizeof(hbuf))) {
        if (strstr(hbuf, "Good"))       health = 100;
        else if (strstr(hbuf, "Overheat")) { health = 60; overheat = true; }
        else if (strstr(hbuf, "Dead"))   health = 20;
        else                              health = 80;
    }

    long long ccycles = 0;
    if (readBatteryIntAcrossSupplies("cycle_count", ccycles)) cycles = (int)ccycles;

    long long chgFull = 0, chgDesign = 0;
    bool haveFull   = readBatteryIntAcrossSupplies("charge_full", chgFull);
    bool haveDesign = readBatteryIntAcrossSupplies("charge_full_design", chgDesign);
    if (haveFull && haveDesign && chgDesign > 0 && chgFull > 0) {
        long long sohLL = chgFull * 100LL / chgDesign;
        if (sohLL < 0)   sohLL = 0;
        if (sohLL > 100) sohLL = 100;
        soh = (int)sohLL;
    }

    bool aged = false;
    if (health == 20 )              aged = true;
    else if (cycles >= 0 && cycles > 500)    aged = true;
    else if (soh >= 0 && soh < 80 &&
             (cycles < 0 || cycles >= kAgedSohMinCyclesToTrust)) aged = true;

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

    int soh, cycles, health;
    {
        std::lock_guard<std::mutex> lk(g_batteryStateMutex);
        soh = g_batterySohPercent;
        cycles = g_batteryCycleCount;
        health = g_batteryHealthPercent;
    }

    float factor = 0.0f;
    bool sohCorroborated = (cycles < 0 || cycles >= kAgedSohMinCyclesToTrust);
    if (soh >= 0 && sohCorroborated) {
        factor = (80.0f - (float)soh) / 60.0f;
        if (factor < 0.0f) factor = 0.0f;
        if (factor > 1.0f) factor = 1.0f;
    } else if (cycles >= 0 && cycles > 500) {
        factor = ((float)cycles - 500.0f) / 500.0f;
        if (factor > 1.0f) factor = 1.0f;
    } else if (health == 20 ) {
        factor = 1.0f;
    }

    if (factor <= 0.0f) return 0;

    CpuPool pool = getPoolForCpu(cpuId);
    if (pool == CpuPool::BIG) return (int)(50.0f * factor);
    if (pool == CpuPool::MID) return (int)(20.0f * factor);
    return 0;
}

std::mutex g_arbBatteryMutex;
int g_arbBatteryCapacityPct = -1;
int g_arbBatteryCharging = 0;

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
        if (strstr(sbuf, "Charging") || strstr(sbuf, "Full")) charging = 1;
    } else {
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
