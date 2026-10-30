#include "aatped.h"
#include <climits>
#include <deque>
#include "cpu_zone_selector.h"
#include "thermal_ff/thermal_ff.h"
#include "thermal_ff/thermal_ff_adapter.h"
#include "thermal_headroom.h"
using namespace aatpe;
using namespace aatpe_ml;

static time_t g_emergencyStartTime = 0;

static bool s_emergencyObserved = false;

static std::mutex g_thermalMutex;

struct ThermalLevelState {
    bool active = false;
    time_t enteredAt = 0;
    time_t exitedAt = 0;
    int streak = 0;
};

static ThermalLevelState s_warningState;
static ThermalLevelState s_criticalState;

static ThermFfConfig s_thermFfCfg;
static ThermFfState s_thermFfState;
static bool s_thermFfReady = false;

static std::string g_thermalTestRoot;
#ifdef AATPE_HOST_TEST
void thermalSetTestRoot(const std::string& r) { g_thermalTestRoot = r; }
#endif

std::atomic<int> g_thermalTripC{0};
static std::function<unsigned long long()> s_testClockFn;
#ifdef AATPE_HOST_TEST
void thermalSetClockForTest(std::function<unsigned long long()> fn) { s_testClockFn = std::move(fn); }
#endif
static std::string tSys(const std::string& sub) { return g_thermalTestRoot.empty() ? sub : (g_thermalTestRoot + sub); }

static std::string zoneRecToTempPath(const std::string& recName) {
    if (recName.rfind("thermal_zone", 0) == 0) {
        return "/sys/class/thermal/" + recName + "/temp";
    }
    if (recName.rfind("hwmon", 0) == 0) {
        return "/sys/class/hwmon/" + recName + "_input";
    }
    return "/sys/class/thermal/" + recName + "/temp";
}

static std::string zoneBaseFromTempPath(const std::string& tempPath) {
    if (tempPath.size() > 5 && tempPath.compare(tempPath.size()-5, 5, "/temp") == 0) {
        return tempPath.substr(0, tempPath.size()-5);
    }
    auto slash = tempPath.rfind('/');
    if (slash != std::string::npos) return tempPath.substr(0, slash);
    return tempPath;
}

static bool anyCpuScalingMaxThrottled() {
    for (int cpuId : g_allCpus) {
        std::string base = tSys("/sys/devices/system/cpu/cpu" + std::to_string(cpuId));
        int online = readInt((base + "/online").c_str(), 1);
        if (online == 0) continue;
        int smax = readInt((base + "/cpufreq/scaling_max_freq").c_str(), -1);
        int pmax = readInt((base + "/cpufreq/cpuinfo_max_freq").c_str(), -1);
        if (smax > 0 && pmax > 0 && smax < pmax) return true;
    }
    if (g_allCpus.empty()) {
        int a = readInt(tSys("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq").c_str(), -1);
        int b = readInt(tSys("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq").c_str(), -1);
        if (a > 0 && b > 0 && a < b) return true;
    }
    return false;
}

static bool anyCpufreqCoolingActive() {
    DIR* dir = opendir(tSys("/sys/class/thermal").c_str());
    if (!dir) {
        int cs0 = readInt(tSys("/sys/class/thermal/cooling_device0/cur_state").c_str(), -1);
        return cs0 > 0;
    }
    bool any = false;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        std::string name = de->d_name;
        if (name.rfind("cooling_device", 0) != 0) continue;
        std::string base = tSys("/sys/class/thermal/") + name;
        std::string type = readFirstLine((base + "/type").c_str());
        bool credit = true;
        if (!type.empty()) {
            bool isCpu    = (type.find("cpufreq") != std::string::npos) ||
                            (type.find("cpu") != std::string::npos);
            bool isNonCpu = (type.find("gpu") != std::string::npos) ||
                            (type.find("ddr") != std::string::npos) ||
                            (type.find("dram") != std::string::npos);
            credit = isCpu && !isNonCpu;
        }
        int cs = readInt((base + "/cur_state").c_str(), -1);
        if (cs > 0 && credit) { any = true; break; }
    }
    closedir(dir);
    return any;
}

#ifdef AATPE_HOST_TEST
namespace aatpe {
bool thermalAnyCpuScalingMaxThrottledForTest() { return anyCpuScalingMaxThrottled(); }
bool thermalAnyCpufreqCoolingActiveForTest() { return anyCpufreqCoolingActive(); }
std::string thermalZoneRecToTempPathForTest(const std::string& recName) { return zoneRecToTempPath(recName); }
std::string thermalZoneBaseFromTempPathForTest(const std::string& p) { return zoneBaseFromTempPath(p); }
}
#endif

int thermalBusyPctHook() {
    static long s_prevBusy = -1, s_prevTotal = -1;
    FILE* f = fopen(tSys("/proc/stat").c_str(), "r");
    if (!f) return -1;
    long user=0, nice=0, system=0, idle=0, iowait=0, irq=0, sirq=0;
    if (fscanf(f, " cpu %ld %ld %ld %ld %ld %ld %ld",
               &user, &nice, &system, &idle, &iowait, &irq, &sirq) != 7) {
        fclose(f); return -1;
    }
    fclose(f);
    long busy = user + nice + system + irq + sirq;
    long total = busy + idle + iowait;
    if (s_prevTotal >= 0) {
        long db = busy - s_prevBusy, dt = total - s_prevTotal;
        if (dt <= 0 || dt > 100000000L) {
            s_prevBusy = busy; s_prevTotal = total;
            return -1;
        }
        if (db < 0) db += (long)4294967296UL;
        if (db < 0 || db > dt) db = dt;
        s_prevBusy = busy; s_prevTotal = total;
        return (int)(db * 100 / dt);
    }
    s_prevBusy = busy; s_prevTotal = total;
    return -1;
}

struct ThermalSample {
    uint64_t timestampMs = 0;
    int cpuTemp = 0;
    int skinTemp = -1;
    int batteryTemp = 0;
};
static std::deque<ThermalSample> s_thermalHistory;
static constexpr size_t kMaxThermalHistorySamples = 30;

static inline int effWarningThresholdC(const Config& c) {
    return c.thermalWarningThreshold > 0 ? c.thermalWarningThreshold
                                         : c.thermalProfileThreshold;
}
static inline int effCriticalThresholdC(const Config& c) {
    return c.thermalCriticalThreshold > 0 ? c.thermalCriticalThreshold
                                         : c.thermalLimitCpu;
}
static inline int effEmergencyThresholdC(const Config& c) {
    return c.thermalEmergencyThreshold > 0 ? c.thermalEmergencyThreshold
                                           : c.thermalEmergency;
}

static unsigned long long g_thermalCacheLastMs = 0;

static std::map<std::string, int> s_lastZoneTemp;
static int s_overTempStreak = 0;

static int s_lastGoodMaxTemp = -1;
static int s_consecutiveReadFails = 0;
static std::atomic<bool> g_thDegNoValidSensor{false};
static std::atomic<bool> g_thDegScanStuck{false};
static inline bool thermalDegradedAny() {
    return g_thDegNoValidSensor.load(std::memory_order_acquire)
        || g_thDegScanStuck.load(std::memory_order_acquire);
}

static std::atomic<int> g_sysfsCpuMaxTempC{0};
static std::atomic<bool> g_mlFreezeHysteresis{false};
static std::atomic<const char*> s_mlFreezeReason{"none"};
#ifdef AATPE_HOST_TEST
bool thermalDegradedForTest() { return thermalDegradedAny(); }
#endif
static int s_consecutiveFreqAllFails = 0;

#ifdef AATPE_HOST_TEST
void thermal_resetRuntimeStateForTest() {
    std::lock_guard<std::mutex> lock(g_thermalMutex);
    s_lastZoneTemp.clear();
    s_overTempStreak = 0;
    g_thermalCacheLastMs = 0;
    g_emergencyStartTime = 0;
    s_emergencyObserved = false;
    s_lastGoodMaxTemp = -1;
    s_consecutiveReadFails = 0;
    g_thDegNoValidSensor.store(false, std::memory_order_release);
    g_thDegScanStuck.store(false, std::memory_order_release);
    s_consecutiveFreqAllFails = 0;
}

#ifdef AATPE_HOST_TEST
time_t thermalEmergencyStartTimeForTest() { return g_emergencyStartTime; }
void thermalSetEmergencyStartTimeForTest(time_t t) { g_emergencyStartTime = t; }
bool thermalObserveEmergencyEdgeForTest(SystemState cur) {
    bool inEmergencyNow = (cur == SystemState::EMERGENCY);
    bool edge = inEmergencyNow && !s_emergencyObserved;
    if (edge) g_emergencyStartTime = time(nullptr);
    s_emergencyObserved = inEmergencyNow;
    return edge;
}
#endif
#endif

std::string aatpe_getThermalCpuZonePath() {
    std::lock_guard<std::mutex> lock(g_thermalMutex);
    return g_thermalCpuZonePath;
}

static bool thermalIsCpuDieType(const std::string& typeLower) {
    return thermzone::isCpuDieType(typeLower);
}

std::string thermalApplyEmergencyModeRecovery() {
    std::string restoredControlMode;
    bool restoredAutoProfile = false;
    bool userOverride = g_emergencyUserOverride.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        if (userOverride) {
            restoredControlMode = g_cfg.controlMode;
        } else {
            g_cfg.controlMode = g_preEmergencyCfg.controlMode;
            restoredControlMode = g_preEmergencyCfg.controlMode;
        }
        g_cfg.autoProfile = g_preEmergencyAutoProfile;
        restoredAutoProfile = g_cfg.autoProfile;
    }
    if (userOverride) {
        g_emergencyUserOverride.store(false, std::memory_order_relaxed);
    }
    writeFileAtomic((std::string(STATE_DIR) + "/control_mode").c_str(), restoredControlMode.c_str());
    writeFileAtomic((std::string(STATE_DIR) + "/auto_profile").c_str(), restoredAutoProfile ? "1" : "0");
    if (getSystemState() != SystemState::EMERGENCY) {
        atomicWriteFile((std::string(STATE_DIR) + "/control_mode_effective").c_str(),
                        restoredControlMode.c_str());
    }
    return restoredControlMode;
}

bool thermalEmergencyShouldExit(SystemState cur, int emergencyTempC,
                                int emergencyWallC, long secSinceEmergencyEntry) {
    if (cur != SystemState::EMERGENCY) return false;
    if (emergencyTempC >= emergencyWallC - thermal::kEmergencyHysteresis) return false;
    if (secSinceEmergencyEntry < (long)kEmergencyRecoverySec) return false;
    return true;
}

void readThermal() {
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        if (g_cfg.thermalEmergency <= 0) {
            LOG_WARN("readThermal: thermalEmergency=%d (<=0), resetting to config default 120", g_cfg.thermalEmergency);
            g_cfg.thermalEmergency = 120;
        }
    }
    Config cfgSnap;
    snapshotConfig(cfgSnap);

    std::unique_lock<std::mutex> tLock(g_thermalMutex);
    unsigned long long nowMs;
    if (s_testClockFn) nowMs = s_testClockFn();
    else nowMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_thermalCacheLastMs > 0 && (nowMs - g_thermalCacheLastMs) < aatpe::thermal_runtime::kCacheTtlMs) {
        return;
    }

    time_t now = time(nullptr);
    int maxTemp = 0, skinTemp = -1, batteryTemp = 0;
    bool throttle = false;
    bool foundSkinZone = false;
    bool batterySensorKnown = false;
    int cpuMaxTemp = 0;
    bool foundCpuZone = false;
    static std::string s_primaryCpuZoneName;
    static std::string s_cpuMaxZoneName;
    int validZoneReads = 0;
    int primaryCpuTemp = 0;
    bool foundPrimaryCpu = false;
    std::string localPrimaryCpuPath;
    std::string localCpuFallbackPath;

    std::set<std::string> currentZones;

    struct ZoneRec { std::string name, type; int t; int tRawMilli; bool invalid; bool isCpu; };
    std::vector<ZoneRec> zoneRecs;
    static std::map<std::string,int> s_prevScanTemp;

    DIR* tzDir = opendir(tSys("/sys/class/thermal").c_str());
    if (tzDir) {
        struct dirent* e;
        while ((e = readdir(tzDir)) != nullptr) {
            if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
            std::string zoneName = e->d_name;
            currentZones.insert(zoneName);

            std::string basePath = tSys("/sys/class/thermal/") + e->d_name;
            std::string typePath = basePath + "/type";
            std::string typeStr = readFile(typePath.c_str());
            std::string tempPath = basePath + "/temp";
            int tRaw = readInt(tempPath.c_str(), INT_MIN);
            if (tRaw == INT_MIN) continue;

            int t;
            int tRawMilli;
            int tAbs = tRaw < 0 ? -tRaw : tRaw;
            if (tAbs > 1500) {
                t = tRaw / 1000;
                tRawMilli = tRaw;
            } else if (tAbs > 150) {
                t = tRaw / 10;
                tRawMilli = tRaw * 100;
            } else {
                t = tRaw;
                tRawMilli = tRaw * 1000;
            }
            if (t > 150 || t < -50) continue;

            if (t > -1 && t < 1) {
                continue;
            }

            validZoneReads++;

            {
                auto lastIt = s_lastZoneTemp.find(zoneName);
                if (lastIt != s_lastZoneTemp.end() && lastIt->second > 0) {
                    int jump = abs(t - lastIt->second);
                    int jumpThreshold = aatpe::thermal_runtime::kMaxTempJumpC;
                    if ((GameScene)g_currentScene.load(std::memory_order_acquire) == GameScene::COMBAT ||
                        (GameScene)g_currentScene.load(std::memory_order_acquire) == GameScene::LOADING) {
                        jumpThreshold = aatpe::thermal_runtime::kMaxTempJumpGameC;
                    }
                    if (jump > jumpThreshold) continue;
                    s_lastZoneTemp[zoneName] = t;
                } else if (lastIt != s_lastZoneTemp.end()) {
                    s_lastZoneTemp[zoneName] = t;
                } else {
                    if (t <= 100) {
                        s_lastZoneTemp[zoneName] = t;
                    }
                }
            }

            std::string typeLower = typeStr;
            std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            bool isSkinZone = (typeLower.find("skin") != std::string::npos ||
                               typeLower.find("pmic_therm") != std::string::npos);
            if (isSkinZone) {
                foundSkinZone = true;
                if (t > skinTemp) skinTemp = t;
                continue;
            }
            {
                bool isTrip = false;
                for (int tp = 0; tp <= 10; tp++) {
                    std::string tripPath = basePath + "/trip_point_" + std::to_string(tp) + "_temp";
                    int tripRaw = readInt(tripPath.c_str(), INT_MIN);
                    if (tripRaw == INT_MIN) continue;
                    int tripT = (tripRaw > 1500) ? tripRaw/1000 : (tripRaw > 150 ? tripRaw/10 : tripRaw);
                    if (abs(t - tripT) <= 1) { isTrip = true; break; }
                }
                if (isTrip) continue;
            }

            bool isPrimaryCpuZone = (typeLower.find("mtktscpu") != std::string::npos);
            if (isPrimaryCpuZone) {
                foundPrimaryCpu = true;
                if (t > primaryCpuTemp) { primaryCpuTemp = t; s_primaryCpuZoneName = zoneName; }
                localPrimaryCpuPath = tempPath;
            }
            bool isCpuZone = thermalIsCpuDieType(typeLower);
            if (isCpuZone) {
                foundCpuZone = true;
                if (t > cpuMaxTemp) { cpuMaxTemp = t; s_cpuMaxZoneName = zoneName; }
                if (localCpuFallbackPath.empty()) {
                    localCpuFallbackPath = tempPath;
                }
            }
            if (t > maxTemp) maxTemp = t;
            zoneRecs.push_back({zoneName, typeStr, t, tRawMilli, false, isCpuZone});
        }
        closedir(tzDir);
    }

    {
        DIR* hw = opendir(tSys("/sys/class/hwmon").c_str());
        if (hw) {
            struct dirent* he;
            while ((he = readdir(hw)) != nullptr) {
                if (strncmp(he->d_name, "hwmon", 5) != 0) continue;
                std::string bp = tSys("/sys/class/hwmon/") + he->d_name;
                std::string name = readFile((bp + "/name").c_str());
                for (int ch = 1; ch <= 10; ++ch) {
                    std::string inPath = bp + "/temp" + std::to_string(ch) + "_input";
                    int tRaw = readInt(inPath.c_str(), INT_MIN);
                    if (tRaw == INT_MIN) continue;
                    int t = (tRaw > 1500) ? tRaw/1000 : (tRaw > 150 ? tRaw/10 : tRaw);
                    if (t < 15 || t > 150) continue;
                    validZoneReads++;
                    int tAbsH = tRaw < 0 ? -tRaw : tRaw;
                    int tRawMilli = (tAbsH > 1500) ? tRaw : (tAbsH > 150 ? tRaw*100 : tRaw*1000);
                    std::string label = readFile((bp + "/temp" + std::to_string(ch) + "_label").c_str());
                    std::string combined = name + " " + label;
                    std::transform(combined.begin(), combined.end(), combined.begin(),
                                   [](unsigned char c){ return (char)std::tolower(c); });
                    bool isCpu = thermalIsCpuDieType(combined);
                    std::string recName = std::string(he->d_name) + "/temp" + std::to_string(ch);
                    zoneRecs.push_back({recName, combined, t, tRawMilli, false, isCpu});
                    if (isCpu && t > cpuMaxTemp) { foundCpuZone = true; cpuMaxTemp = t; }
                    if (t > maxTemp) maxTemp = t;
                }
            }
            closedir(hw);
        }
    }

    {
        static constexpr int kClampMilliDeadband = 50;
        bool anyNonCpuMoved = false;
        for (auto& z : zoneRecs) {
            if (z.isCpu) continue;
            auto it = s_prevScanTemp.find(z.name);
            if (it != s_prevScanTemp.end() &&
                abs(it->second - z.tRawMilli) > kClampMilliDeadband) anyNonCpuMoved = true;
        }
        if (anyNonCpuMoved) {
            for (auto& z : zoneRecs) {
                if (!z.isCpu) continue;
                auto it = s_prevScanTemp.find(z.name);
                if (it != s_prevScanTemp.end() &&
                    abs(it->second - z.tRawMilli) <= kClampMilliDeadband) {
                    z.invalid = true;
                    if (z.name == s_primaryCpuZoneName) { foundPrimaryCpu = false; primaryCpuTemp = 0; }
                    if (z.name == s_cpuMaxZoneName) { foundCpuZone = false; cpuMaxTemp = 0; }
                }
            }
        }
        s_prevScanTemp.clear();
        for (auto& z : zoneRecs) s_prevScanTemp[z.name] = z.tRawMilli;

        {
            std::string selectedPath;
            bool primaryValid = !localPrimaryCpuPath.empty();
            if (primaryValid) {
                for (const auto& z : zoneRecs) {
                    if (z.name.find("mtktscpu") != std::string::npos && z.invalid) {
                        primaryValid = false;
                        break;
                    }
                }
            }
            if (primaryValid && foundPrimaryCpu && primaryCpuTemp > 0) {
                selectedPath = localPrimaryCpuPath;
            } else if (foundCpuZone && cpuMaxTemp > 0) {
                for (const auto& z : zoneRecs) {
                    if (z.isCpu && !z.invalid) {
                        selectedPath = zoneRecToTempPath(z.name);
                        break;
                    }
                }
            }
            g_thermalCpuZonePath = selectedPath;
        }
        {
            std::string zf;
            for (size_t i = 0; i < zoneRecs.size(); i++) {
                if (i) zf += ";";
                zf += zoneRecs[i].name.substr(12) + ":" + zoneRecs[i].type + ":" +
                      std::to_string(zoneRecs[i].t) + ":" + (zoneRecs[i].invalid ? "1" : "0");
            }
            writeFileAtomic((std::string(STATE_DIR) + "/thermal_zones").c_str(), zf);
        }
    }

    for (auto it = s_lastZoneTemp.begin(); it != s_lastZoneTemp.end(); ) {
        if (currentZones.find(it->first) == currentZones.end()) {
            it = s_lastZoneTemp.erase(it);
        } else {
            ++it;
        }
    }

    if (foundPrimaryCpu && primaryCpuTemp > 0) {
        maxTemp = std::max({maxTemp, primaryCpuTemp, cpuMaxTemp});
    } else if (foundCpuZone && cpuMaxTemp > 0) {
        maxTemp = std::max(maxTemp, cpuMaxTemp);
    }

    if (validZoneReads == 0) {
        s_consecutiveReadFails++;
        g_thDegNoValidSensor.store(true, std::memory_order_release);
        if (s_lastGoodMaxTemp >= 0) {
            maxTemp = s_lastGoodMaxTemp;
        } else {
            int consCritical = effCriticalThresholdC(cfgSnap);
            maxTemp = (consCritical > 0) ? consCritical : 90;
        }
        LOG_WARN("readThermal: zero valid thermal zone reads (%d consecutive), "
                 "fail-safe: holding conservative temp=%dC (no boost)",
                 s_consecutiveReadFails, maxTemp);
    } else {
        s_consecutiveReadFails = 0;
        s_lastGoodMaxTemp = maxTemp;
        g_thDegNoValidSensor.store(false, std::memory_order_release);
    }

    int cpuDieTemp;
    if (foundPrimaryCpu && primaryCpuTemp > 0)      cpuDieTemp = primaryCpuTemp;
    else if (foundCpuZone && cpuMaxTemp > 0)        cpuDieTemp = cpuMaxTemp;
    else                                            cpuDieTemp = -1;
    g_thermalCpu.store(cpuDieTemp, std::memory_order_relaxed);
    LOG_INFO("readThermal: CPU die temp=%d°C source=%s", cpuDieTemp, g_thermalCpuZonePath.c_str());

    if (!g_thermalCpuZonePath.empty()) {
        std::string zoneBase = zoneBaseFromTempPath(g_thermalCpuZonePath);
        int tripC = 0;
        for (int tp = 0; tp <= 2 && tripC == 0; tp++) {
            std::string tripPath = zoneBase + "/trip_point_" +
                                   std::to_string(tp) + "_temp";
            int raw = readInt(tripPath.c_str(), 0);
            if (raw > 1000)            tripC = raw / 1000;
            else if (raw > 150 && raw <= 1000) tripC = raw / 10;
            else if (raw > 0 && raw <= 150)    tripC = raw;
        }
        g_thermalTripC.store(tripC, std::memory_order_relaxed);
    }

    if (g_thermalCpuZonePath.empty()) {
        DIR* fbDir = opendir(tSys("/sys/class/thermal").c_str());
        if (fbDir) {
            struct dirent* fe;
            while ((fe = readdir(fbDir)) != nullptr) {
                if (strncmp(fe->d_name, "thermal_zone", 12) != 0) continue;
                std::string fbBase = tSys("/sys/class/thermal/") + fe->d_name;
                std::string fbType = readFile((fbBase + "/type").c_str());
                std::string fbTypeL = fbType;
                std::transform(fbTypeL.begin(), fbTypeL.end(), fbTypeL.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (!thermalIsCpuDieType(fbTypeL)) continue;
                std::string fbTempPath = fbBase + "/temp";
                int fbRaw = readInt(fbTempPath.c_str(), -999999);
                if (fbRaw <= -999999) continue;
                int fbT;
                if (fbRaw > 1500)      fbT = fbRaw / 1000;
                else if (fbRaw > 150)  fbT = fbRaw / 10;
                else                   fbT = fbRaw;
                if (fbT >= -50 && fbT <= 150 && !(fbT > -1 && fbT < 1)) {
                    g_thermalCpuZonePath = fbTempPath;
                    LOG_WARN("readThermal: no primary CPU zone, fallback to %s (%s, temp=%d°C)",
                             fe->d_name, fbType.c_str(), fbT);
                    break;
                }
            }
            closedir(fbDir);
        }
        if (g_thermalCpuZonePath.empty()) {
            LOG_WARN("readThermal: no readable CPU/AP/SoC thermal zone — temperature unknown, thermal policy not based on fake data");
        }
    }

    {
        static const char* kBatterySupplies[] = {"battery", "bms", "battery/main"};
        int batteryRaw = INT_MIN;
        for (const char* sup : kBatterySupplies) {
            std::string p = tSys(std::string("/sys/class/power_supply/") + sup + "/temp");
            int v = readInt(p.c_str(), INT_MIN);
            if (v != INT_MIN) { batteryRaw = v; break; }
        }
        bool batteryKnown = (batteryRaw != INT_MIN);
        batterySensorKnown = batteryKnown;
        if (batteryRaw >= 200 && batteryRaw <= 1500) {
            batteryTemp = batteryRaw / 10;
        } else if (batteryRaw >= 20 && batteryRaw <= 80) {
            batteryTemp = batteryRaw;
        } else if (batteryRaw > 1500) {
            batteryTemp = batteryRaw / 1000;
        } else if (batteryKnown) {
            batterySensorKnown = false;
            batteryTemp = -1;
        } else {
            batteryTemp = -1;
        }
        if (batteryTemp > 100) batteryTemp = 100;
    }

    extern int thermalBusyPctHook();
    {
        static std::map<std::string, std::deque<int>> s_zoneTempWin;
        static std::deque<int> s_busyWin;
        int busy = thermalBusyPctHook();
        if (busy >= 0) s_busyWin.push_back(busy);
        const size_t kW = 30;
        if (s_busyWin.size() > kW) s_busyWin.pop_front();
        int busyMin = 100, busyMax = 0;
        for (int b : s_busyWin) { busyMin = std::min(busyMin, b); busyMax = std::max(busyMax, b); }
        bool windowFull = (s_busyWin.size() >= kW);
        bool loadSwing = (windowFull && (busyMax - busyMin) >= 20);
        bool gateActive = (s_busyWin.size() >= 2);

        if (gateActive) {
            for (auto& z : zoneRecs) {
                if (!z.isCpu) continue;
                auto& w = s_zoneTempWin[z.name];
                w.push_back(z.tRawMilli);
                if (w.size() > kW) w.pop_front();
                if (!windowFull) continue;
                int rminMilli = INT_MAX, rmaxMilli = INT_MIN;
                for (int v : w) { rminMilli = std::min(rminMilli, v); rmaxMilli = std::max(rmaxMilli, v); }
                int rngMilli = rmaxMilli - rminMilli;
                bool loadClamp = (loadSwing && rngMilli < 1000);
                bool constantClamp = (windowFull && rngMilli < 50);
                bool scalingMaxThrottled = anyCpuScalingMaxThrottled();
                bool coolingActive = anyCpufreqCoolingActive();
                bool otherZoneVarying = false;
                for (auto& o : zoneRecs) {
                    if (&o == &z || !o.isCpu) continue;
                    auto& ow = s_zoneTempWin[o.name];
                    if (ow.size() >= 2 && abs(ow.back() - ow.front()) > 50) otherZoneVarying = true;
                }
                bool clampConfirmed = constantClamp && !scalingMaxThrottled &&
                                      !coolingActive && otherZoneVarying;
                if (loadClamp || clampConfirmed) {
                    z.invalid = true;
                    LOG_WARN("readThermal: zone %s temp constant %dC -- invalid",
                             z.name.c_str(), z.t);
                }
            }
            foundPrimaryCpu = false; primaryCpuTemp = 0;
            foundCpuZone = false; cpuMaxTemp = 0;
            for (auto& z : zoneRecs) {
                if (!z.isCpu || z.invalid) continue;
                foundCpuZone = true;
                if (z.t > cpuMaxTemp) cpuMaxTemp = z.t;
            }
            if (foundCpuZone) { foundPrimaryCpu = true; primaryCpuTemp = cpuMaxTemp; }
            {
                std::string revised;
                for (const auto& z : zoneRecs) {
                    if (z.isCpu && !z.invalid &&
                        z.name.find("mtktscpu") != std::string::npos) {
                        revised = "/sys/class/thermal/" + z.name + "/temp";
                        break;
                    }
                }
                if (revised.empty()) {
                    for (const auto& z : zoneRecs) {
                        if (z.isCpu && !z.invalid) {
                            revised = zoneRecToTempPath(z.name);
                            break;
                        }
                    }
                }
                g_thermalCpuZonePath = revised;
            }
        }
        if (foundPrimaryCpu && primaryCpuTemp > 0) cpuDieTemp = primaryCpuTemp;
        else if (foundCpuZone && cpuMaxTemp > 0)  cpuDieTemp = cpuMaxTemp;
        else                                      cpuDieTemp = -1;
        g_thermalCpu.store(cpuDieTemp, std::memory_order_relaxed);
        writeFileAtomic((std::string(STATE_DIR) + "/thermal_source").c_str(),
                        g_thermalCpuZonePath);
    }

    int decisionTemp  = maxTemp;
    int emergencyTemp = maxTemp;
    {
        std::vector<thermzone::ZoneReading> tz;
        tz.reserve(zoneRecs.size());
        for (const auto& z : zoneRecs) {
            std::string tl = z.type;
            std::transform(tl.begin(), tl.end(), tl.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            thermzone::ZoneReading r;
            r.name      = z.name;
            r.typeLower = tl;
            bool plausible = (z.t > -50 && z.t < 150);
            r.valid    = (!z.invalid && plausible);
            r.tempC    = r.valid ? z.t : INT_MIN;
            tz.push_back(r);
        }
        static std::map<std::string, thermzone::ZoneHist> s_tzHist;
        thermzone::SelectResult sel = thermzone::select(tz, s_tzHist);
        if (sel.anyCpuTempC > 0) {
            decisionTemp  = sel.trustedCpuTempC;
            emergencyTemp = sel.anyCpuTempC;
        }
        g_sysfsCpuMaxTempC.store(decisionTemp, std::memory_order_relaxed);
        if (cfgSnap.logThermal) {
            std::string rn;
            for (const auto& n : sel.rogueNames) { if (!rn.empty()) rn += ","; rn += n; }
            LOG_INFO("readThermal: tz trusted=%dC anyCpu=%dC rogue=[%s] cpuZones=%zu",
                     decisionTemp, emergencyTemp, rn.c_str(), sel.cpuNames.size());
        }
    }

    int onlineCores = 0, freqReadOk = 0;
    for (int cpuId : g_allCpus) {
        std::string base = tSys("/sys/devices/system/cpu/cpu" + std::to_string(cpuId));
        std::string onlinePath = base + "/online";
        int onlineVal = readInt(onlinePath.c_str(), 1);
        if (onlineVal == 0) continue;
        onlineCores++;
        int cur = readInt((base + "/cpufreq/scaling_cur_freq").c_str(), -1);
        int maxf = readInt((base + "/cpufreq/scaling_max_freq").c_str(), -1);
        if (cur < 0 || maxf <= 0) {
            LOG_WARN("readThermal: CPU%d freq read failed (cur=%d max=%d) -- skipping as throttle evidence",
                     cpuId, cur, maxf);
            continue;
        }
        freqReadOk++;
        if (cur * 100 / maxf < thermal::kThrottleFreqPct) throttle = true;
    }
    if (onlineCores > 0 && freqReadOk == 0) {
        s_consecutiveFreqAllFails++;
        LOG_WARN("readThermal: all %d online cores freq-unreadable (%d consecutive) -- UNKNOWN, not forcing throttle",
                 onlineCores, s_consecutiveFreqAllFails);
    } else {
        s_consecutiveFreqAllFails = 0;
    }

    if (anyCpufreqCoolingActive()) throttle = true;
    {
        DIR* dv = opendir(tSys("/sys/class/devfreq").c_str());
        if (dv) {
            struct dirent* de;
            while ((de = readdir(dv)) != nullptr) {
                if (strncmp(de->d_name, "devfreq", 7) != 0) continue;
                std::string dp = tSys("/sys/class/devfreq/") + de->d_name;
                std::string nm = readFile((dp + "/name").c_str());
                if (nm.find("gpu") == std::string::npos) continue;
                int gc = readInt((dp + "/cur_freq").c_str(), -1);
                int gm = readInt((dp + "/max_freq").c_str(), -1);
                if (gc > 0 && gm > 0 && gc * 100 / gm < 80) { throttle = true; break; }
            }
            closedir(dv);
        }
    }

    int criticalWall = effCriticalThresholdC(cfgSnap);
    int warningWallRaw   = effWarningThresholdC(cfgSnap);
    int emergencyWallRaw = effEmergencyThresholdC(cfgSnap);
    const int kEmergencyHardCeilingC = 120;
    if (emergencyWallRaw > kEmergencyHardCeilingC) emergencyWallRaw = kEmergencyHardCeilingC;
    if (criticalWall > emergencyWallRaw) criticalWall = emergencyWallRaw - 1;
    if (warningWallRaw >= criticalWall)   warningWallRaw = criticalWall - 1;
    int warningWall    = warningWallRaw;
    int emergencyWall   = emergencyWallRaw;

    int headroom = criticalWall - decisionTemp;
    if (headroom < 0) headroom = 0;
    int headroomPct = criticalWall > 0 ? headroom * 100 / criticalWall : 0;

    g_thermalCacheLastMs = nowMs;
    writeFile((std::string(STATE_DIR) + "/thermal_cpu_max").c_str(), decisionTemp);
    if (foundSkinZone) {
        writeFile((std::string(STATE_DIR) + "/thermal_skin").c_str(), skinTemp);
    } else {
        writeFile((std::string(STATE_DIR) + "/thermal_skin").c_str(), "");
    }
    if (batterySensorKnown) {
        writeFile((std::string(STATE_DIR) + "/thermal_battery").c_str(), batteryTemp);
    } else {
        writeFile((std::string(STATE_DIR) + "/thermal_battery").c_str(), "");
    }
    g_thermalSkin.store(foundSkinZone ? skinTemp : -1, std::memory_order_relaxed);
    g_thermalBattery.store(batterySensorKnown ? batteryTemp : -1, std::memory_order_relaxed);
    writeFile((std::string(STATE_DIR) + "/thermal_headroom").c_str(), headroom);
    writeFile((std::string(STATE_DIR) + "/thermal_headroom_pct").c_str(), headroomPct);
    g_thermalHeadroomPct.store(headroomPct, std::memory_order_relaxed);
    writeFile((std::string(STATE_DIR) + "/thermal_throttle").c_str(), throttle ? 1 : 0);
    writeFileAtomic((std::string(STATE_DIR) + "/thermal_emergency").c_str(), g_thermalEmergency ? "1" : "0");

    bool inBootGrace = ((now - g_startTime.load()) < thermal_runtime::kEmergencyBootGraceSec);

    tLock.unlock();

    {
        bool inEmergencyNow = (getSystemState() == SystemState::EMERGENCY);
        if (inEmergencyNow && !s_emergencyObserved) {
            g_emergencyStartTime = now;
        }
        s_emergencyObserved = inEmergencyNow;
    }

    if (aatpeThermalEmergencyHardWall(emergencyTemp, emergencyWall)) {
        s_overTempStreak++;
        if (getSystemState() != SystemState::EMERGENCY &&
            !inBootGrace &&
            s_overTempStreak >= thermal_runtime::kEmergencyDebounceReads) {
            g_emergencyStartTime = now;
            transitionToState(SystemState::EMERGENCY, "thermal_emergency");
        }
    } else {
        s_overTempStreak = 0;
    }
    writeFileAtomic((std::string(STATE_DIR) + "/thermal_emergency").c_str(),
                    g_thermalEmergency ? "1" : "0");
    long secSinceEmergencyEntry = (g_emergencyStartTime > 0)
                                      ? (long)(now - g_emergencyStartTime) : 0;
    if (thermalEmergencyShouldExit(getSystemState(), emergencyTemp, emergencyWall,
                                    secSinceEmergencyEntry)) {
        transitionToState(SystemState::DEGRADED, "thermal_recovery_30s");
        thermalApplyEmergencyModeRecovery();
    }

    int warningExitWall = warningWall - thermal_runtime::kHysteresisDeltaC;
    if (cfgSnap.autoProfile && !g_thermalEmergency && !g_profileLock.load()) {
        if (decisionTemp >= warningWall) {
            s_warningState.streak++;
            if (!s_warningState.active && s_warningState.streak >= 2) {
                s_warningState.active = true;
                s_warningState.enteredAt = now;
                aatpe_profile::profileRequest("thermal", aatpe_profile::ProfileSource::THERMAL);
                if (cfgSnap.logThermal) {
                    LOG_INFO("Thermal WARNING: temp=%dC >= %dC, entering thermal profile",
                             decisionTemp, warningWall);
                }
            }
        } else if (decisionTemp <= warningExitWall) {
            s_warningState.streak = 0;
            if (s_warningState.active) {
                bool minHoldMet = (now - s_warningState.enteredAt) >= thermal_runtime::kMinHoldSec;
                if (minHoldMet) {
                    s_warningState.active = false;
                    s_warningState.exitedAt = now;
                    if (cfgSnap.logThermal) {
                        LOG_INFO("Thermal cooled down below warning exit wall (%dC); "
                                 "releasing THERMAL vote, arbiter will re-pick effective profile",
                                 warningExitWall);
                    }
                    aatpe_profile::profileRelease(aatpe_profile::ProfileSource::THERMAL);
                }
            }
        } else {
            s_warningState.streak = 0;
        }
    } else {
        if (s_warningState.active) {
            s_warningState.active = false;
            s_warningState.exitedAt = now;
            aatpe_profile::profileRelease(aatpe_profile::ProfileSource::THERMAL);
        }
        s_warningState.streak = 0;
    }

    {
        uint64_t nowSampleMs = nowMs;
        ThermalSample sample;
        sample.timestampMs = nowSampleMs;
        sample.cpuTemp = decisionTemp;
        sample.skinTemp = foundSkinZone ? skinTemp : -1;
        sample.batteryTemp = batteryTemp;
        s_thermalHistory.push_back(sample);
        while (s_thermalHistory.size() > kMaxThermalHistorySamples) {
            s_thermalHistory.pop_front();
        }
    }

    int cpuSlopeCps = 0;
    if (s_thermalHistory.size() >= 3) {
        const auto& oldest = s_thermalHistory.front();
        const auto& newest = s_thermalHistory.back();
        uint64_t dtMs = newest.timestampMs - oldest.timestampMs;
        if (dtMs > 0) {
            double dtSec = (double)dtMs / 1000.0;
            cpuSlopeCps = (int)round((double)(newest.cpuTemp - oldest.cpuTemp) / dtSec);
        }
    }

    int durationAboveCriticalSec = 0;
    int criticalWallLocal = criticalWall;
    if (criticalWallLocal > 0 && s_thermalHistory.size() >= 2) {
        auto it = s_thermalHistory.rbegin();
        if (it->cpuTemp >= criticalWallLocal) {
            uint64_t accMs = 0;
            auto prev = it;
            ++it;
            for (; it != s_thermalHistory.rend() && it->cpuTemp >= criticalWallLocal; ++it, ++prev) {
                accMs += (prev->timestampMs - it->timestampMs);
            }
            durationAboveCriticalSec = (int)(accMs / 1000ULL);
        }
    }

    int criticalExitWall = criticalWallLocal - thermal_runtime::kHysteresisDeltaC;
    if (criticalWallLocal > 0) {
        if (decisionTemp >= criticalWallLocal) {
            s_criticalState.streak++;
            if (!s_criticalState.active && s_criticalState.streak >= 2) {
                s_criticalState.active = true;
                s_criticalState.enteredAt = now;
                LOG_WARN("Thermal CRITICAL: temp=%dC >= %dC, migration pressure activated",
                         decisionTemp, criticalWallLocal);
            }
        } else if (decisionTemp <= criticalExitWall) {
            s_criticalState.streak = 0;
            if (s_criticalState.active) {
                bool minHoldMet = (now - s_criticalState.enteredAt) >= thermal_runtime::kMinHoldSec;
                if (minHoldMet) {
                    s_criticalState.active = false;
                    s_criticalState.exitedAt = now;
                    LOG_INFO("Thermal cooled below critical exit wall (%dC), migration pressure released",
                             criticalExitWall);
                }
            }
        } else {
            s_criticalState.streak = 0;
        }
    }

    int skinPressure = 0;
    if (foundSkinZone && skinTemp >= 0 && cfgSnap.thermalLimitSkin > 0) {
        if (skinTemp >= cfgSnap.thermalLimitSkin) {
            int excess = skinTemp - cfgSnap.thermalLimitSkin;
            skinPressure = std::min(100, 50 + excess * 5);
            if (durationAboveCriticalSec > 10) {
                skinPressure = std::min(100, skinPressure + 10);
            }
        }
    }

    int batteryPressure = 0;
    if (batteryTemp > 0 && cfgSnap.thermalLimitBattery > 0) {
        if (batteryTemp >= cfgSnap.thermalLimitBattery) {
            int excess = batteryTemp - cfgSnap.thermalLimitBattery;
            batteryPressure = std::min(100, 50 + excess * 5);
            if (durationAboveCriticalSec > 15) {
                batteryPressure = std::min(100, batteryPressure + 15);
            }
        }
    }

    int cpuPressure = 0;
    if (criticalWallLocal > 0 && decisionTemp >= (criticalWallLocal - 10)) {
        int proximity = criticalWallLocal - decisionTemp;
        if (proximity <= 0) {
            cpuPressure = 70;
        } else {
            cpuPressure = std::max(0, 70 - proximity * 7);
        }
        if (cpuSlopeCps > 1) {
            cpuPressure = std::min(100, cpuPressure + cpuSlopeCps * 5);
        }
        if (durationAboveCriticalSec > 20) {
            cpuPressure = std::min(100, cpuPressure + 10);
        }
    }

    if (!s_thermFfReady) {
        therm_ff_config_default(&s_thermFfCfg);
        therm_ff_state_init(&s_thermFfState);
        s_thermFfReady = true;
    }
    s_thermFfCfg.warning_temp_c   = (float)warningWall;
    s_thermFfCfg.emergency_temp_c = (float)emergencyWall;

    ThermHardWall ffWall = aatpe_therm_ff_map_wall(
        g_thermalEmergency.load(std::memory_order_relaxed),
        s_criticalState.active, s_warningState.active);

    ThermFfOut ffOut;
    therm_ff_update(&s_thermFfState, &s_thermFfCfg,
                    (double)nowMs, (float)decisionTemp, ffWall, &ffOut);

    FfGear ffWallGear = ffOut.wall_gear;
    FfGear ffGear     = ffOut.ff_gear;
    FfGear ffFinal    = aatpe_therm_ff_final_gear(ffWallGear, ffGear);

    if (ffWall == THERM_HW_EMERGENCY && ffFinal < FF_GEAR_SEVERE) {
        ffFinal = FF_GEAR_SEVERE;
    }

    bool ffPreThrottle = aatpe_therm_ff_pre_throttle(ffFinal, ffWallGear) &&
                         (ffOut.sensor_ok != 0);

    if (ffPreThrottle) {
        int ffPressure = 30 + (int)ffGear * 15;
        if (ffPressure > cpuPressure) cpuPressure = ffPressure;
    }

    writeFile((std::string(STATE_DIR) + "/thermal_skin_pressure").c_str(), skinPressure);
    writeFile((std::string(STATE_DIR) + "/thermal_battery_pressure").c_str(), batteryPressure);
    writeFile((std::string(STATE_DIR) + "/thermal_cpu_pressure").c_str(), cpuPressure);
    writeFile((std::string(STATE_DIR) + "/thermal_cpu_slope").c_str(), cpuSlopeCps);
    writeFile((std::string(STATE_DIR) + "/thermal_duration_above_critical_sec").c_str(),
              durationAboveCriticalSec);

    int combinedPressure = std::max({skinPressure, batteryPressure, cpuPressure});
    writeFile((std::string(STATE_DIR) + "/thermal_migration_pressure").c_str(), combinedPressure);

    writeFile((std::string(STATE_DIR) + "/thermal_ff_slope_cps").c_str(),
              ffOut.slope_cps);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_eta_warning_s").c_str(),
              ffOut.eta_warning_s);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_eta_emergency_s").c_str(),
              ffOut.eta_emergency_s);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_gear").c_str(),        (int)ffGear);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_wall_gear").c_str(),  (int)ffWallGear);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_final_gear").c_str(),  (int)ffFinal);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_sensor_ok").c_str(),   ffOut.sensor_ok);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_pre_throttle").c_str(), ffPreThrottle ? 1 : 0);

    extern std::atomic<bool> thermalSmoothEnabled;
    if (thermalSmoothEnabled.load(std::memory_order_relaxed)) {
        using namespace aatpe::thermh;
        static float  s_smoothPrev     = 1.0f;
        static int    s_smoothRelaxStrk = 0;

        ThermalHeadroomInput hIn{};
        hIn.tempC          = (float)decisionTemp;
        hIn.warningWallC   = (float)warningWall;
        hIn.criticalWallC  = (float)criticalWall;
        hIn.emergencyWallC = (float)emergencyWall;
        hIn.slopeCps       = ffOut.slope_cps;
        hIn.etaWarningS    = ffOut.eta_warning_s;
        hIn.sensorOk       = (ffOut.sensor_ok != 0);
        hIn.thermalEmergency = g_thermalEmergency.load(std::memory_order_relaxed);

        HeadroomOutput hOut = thermalHeadroom(hIn);
        smoothPlacementScale(hOut.placementScale, s_smoothPrev, s_smoothRelaxStrk);
        g_thermalPlacementScale.store(s_smoothPrev, std::memory_order_relaxed);

        writeFile((std::string(STATE_DIR) + "/thermal_headroom01").c_str(), hOut.headroom01);
        writeFile((std::string(STATE_DIR) + "/thermal_placement_scale").c_str(), s_smoothPrev);
    }
}

#include <condition_variable>
static std::thread s_thermalThread;
static std::atomic<bool> s_thermalRun{false};
static std::atomic<long long> s_lastScanBootSec{0};
static std::condition_variable s_thermalCv;
static std::mutex s_thermalMutex;
static std::atomic<bool> s_thermalDone{false};
static std::atomic<bool> s_thermalDetached{false};

static std::function<long long()> s_testBootClockFn;
static long long bootTimeSec() {
    if (s_testBootClockFn) return s_testBootClockFn();
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (long long)ts.tv_sec;
}
#ifdef AATPE_HOST_TEST
void thermalSetBootClockForTest(std::function<long long()> fn) { s_testBootClockFn = std::move(fn); }
void thermalSetScanStampForTest(long long sec) { s_lastScanBootSec.store(sec); }
#endif

struct ThermalHalInfo { int level; int skinC; int headroom; };
ThermalHalInfo parseThermalServiceDump(const std::string& dump);
static std::mutex s_halMutex;
static ThermalHalInfo s_halCache{-1,-1,-1};

static int mlFreezePauseC() {
    int wall = g_cfg.thermalEmergencyThreshold;
    if (wall <= 0) wall = 105;
    return wall - 15;
}
static int mlFreezeResumeC() { return mlFreezePauseC() - 5; }

namespace aatpe {
int aatpeThermalHalLevel() { std::lock_guard<std::mutex> lk(s_halMutex); return s_halCache.level; }
bool aatpeThermalDegraded() { return thermalDegradedAny(); }

const char* aatpeThermalFreezeReason() {
    return s_mlFreezeReason.load(std::memory_order_relaxed);
}
bool aatpeThermalMayFreezeTraining(bool thermalEnabled) {
    if (!thermalEnabled) {
        g_mlFreezeHysteresis.store(false);
        s_mlFreezeReason.store("disabled", std::memory_order_relaxed);
        return false;
    }
    if (aatpeThermalDegraded()) {
        s_mlFreezeReason.store("sensor_degraded", std::memory_order_relaxed);
        return false;
    }

    int cpuTemp = g_sysfsCpuMaxTempC.load(std::memory_order_relaxed);
    int halLevel = aatpeThermalHalLevel();

    if (g_mlFreezeHysteresis.load(std::memory_order_relaxed)) {
        if (cpuTemp > 0 && cpuTemp < mlFreezeResumeC()) {
            g_mlFreezeHysteresis.store(false);
            s_mlFreezeReason.store("resumed", std::memory_order_relaxed);
            return false;
        }
        s_mlFreezeReason.store(cpuTemp >= mlFreezePauseC() ? "cpu_hot" : "hal_severe",
                               std::memory_order_relaxed);
        return true;
    }

    bool sysfsHot = (cpuTemp > 0 && cpuTemp >= mlFreezePauseC());
    bool halConfirms = (halLevel >= 3 && cpuTemp > 0 && cpuTemp >= mlFreezePauseC());

    if (sysfsHot || halConfirms) {
        g_mlFreezeHysteresis.store(true);
        s_mlFreezeReason.store(halConfirms ? "hal_severe" : "cpu_hot",
                               std::memory_order_relaxed);
        return true;
    }
    if (halLevel >= 3) s_mlFreezeReason.store("hal_invalid", std::memory_order_relaxed);
    else s_mlFreezeReason.store("none", std::memory_order_relaxed);
    return false;
}
}
static long long s_halLastForkMs = 0;
static int s_scanCycle = 0;

static std::string aatpeExecCapture(const char* cmd) {
    return aatpeExecArgv({cmd, "thermalservice"}, "thermal-hal");
}

static void refreshThermalHal() {
    long long now = bootTimeSec() * 1000LL;
    if (now - s_halLastForkMs < 5000) return;
    std::string dump = aatpeExecCapture("dumpsys");
    if (dump.empty()) dump = aatpeExecCapture("cmd");
    s_halLastForkMs = now;
    ThermalHalInfo info{-1,-1,-1};
    if (!dump.empty()) info = parseThermalServiceDump(dump);
    {
        std::lock_guard<std::mutex> lk(s_halMutex);
        s_halCache = info;
    }
    static const char* lvlNames[] = {"NONE","LIGHT","MODERATE","SEVERE","CRITICAL","EMERGENCY","SHUTDOWN"};
    writeFileAtomic((std::string(STATE_DIR) + "/thermal_hal_status").c_str(),
        info.level < 0 ? "UNAVAILABLE" : lvlNames[info.level]);
    writeFile((std::string(STATE_DIR) + "/thermal_hal_skin").c_str(), info.skinC);
    writeFile((std::string(STATE_DIR) + "/thermal_hal_headroom").c_str(), info.headroom);
    writeFileAtomic((std::string(STATE_DIR) + "/thermal_hal_source").c_str(),
        info.level < 0 ? "sysfs" : "hal");
}

void startThermalSampling() {
    if (s_thermalRun.exchange(true)) return;
    if (s_thermalDetached.load()) return;
    s_lastScanBootSec.store(bootTimeSec());
    s_thermalDone.store(false);
    s_thermalThread = std::thread([](){
        while (s_thermalRun.load()) {
            try {
                readThermal();
                s_lastScanBootSec.store(bootTimeSec());
                if ((++s_scanCycle % 3) == 0) refreshThermalHal();
            } catch (...) {
                static int s_exceptCycleCount = 0;
                if ((++s_exceptCycleCount % 10) == 1) {
                    LOG_WARN("thermal: unexpected exception in scan loop (cycle #%d), continuing",
                             s_exceptCycleCount);
                }
            }
            std::unique_lock<std::mutex> lk(s_thermalMutex);
            s_thermalCv.wait_for(lk, std::chrono::milliseconds(1000),
                                 []{ return !s_thermalRun.load(); });
        }
        s_thermalDone.store(true);
        if (!s_thermalDetached.load()) s_thermalCv.notify_all();
    });
}

void thermalFreshnessTick() {
    long long last = s_lastScanBootSec.load(std::memory_order_acquire);
    if (last == 0) return;
    long long age = bootTimeSec() - last;
    if (age < 0) age = 0;
    if (age > 10) {
        g_thDegScanStuck.store(true, std::memory_order_release);
        writeFile((std::string(STATE_DIR) + "/thermal_degraded").c_str(), 1);
        writeFileAtomic((std::string(STATE_DIR) + "/thermal_freshness").c_str(),
                        std::string("stale=") + std::to_string(age));
    } else {
        g_thDegScanStuck.store(false, std::memory_order_release);
        bool degraded = thermalDegradedAny();
        writeFile((std::string(STATE_DIR) + "/thermal_degraded").c_str(), degraded ? 1 : 0);
        writeFileAtomic((std::string(STATE_DIR) + "/thermal_freshness").c_str(), "fresh");
    }
}

void stopThermalSampling() {
    if (!s_thermalRun.exchange(false)) return;
    s_thermalCv.notify_all();
    if (!s_thermalThread.joinable()) return;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!s_thermalDone.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (s_thermalDone.load()) {
        s_thermalThread.join();
    } else {
        s_thermalThread.detach();
        s_thermalDetached.store(true);
    }
}

static int halStatusToLevel(int s) {
    if (s < 0 || s > 6) return -1;
    return s;
}

ThermalHalInfo parseThermalServiceDump(const std::string& dump) {
    ThermalHalInfo r{-1, -1, -1};
    std::stringstream ss(dump);
    std::string line;
    while (std::getline(ss, line)) {
        size_t sp = line.find("mStatus=");
        if (sp == std::string::npos) continue;
        int st = atoi(line.c_str() + sp + 8);
        int lvl = halStatusToLevel(st);
        if (lvl < 0) continue;
        bool isBattery = line.find("mType=BATTERY") != std::string::npos;
        if (!isBattery && lvl > r.level) r.level = lvl;
        size_t vp = line.find("mValue=");
        if (vp != std::string::npos) {
            double v = atof(line.c_str() + vp + 7);
            if (!std::isfinite(v)) continue;
            if (line.find("mType=SKIN") != std::string::npos) {
                int s = (int)v;
                if (s < -40) s = -40;
                if (s > 150) s = 150;
                r.skinC = s;
            }
            if (line.find("mType=HEADROOM") != std::string::npos ||
                line.find("mType=BRAIN") != std::string::npos) {
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            int h = (int)(v * 100.0);
            if (h > r.headroom) r.headroom = h;
            }
        }
    }
    return r;
}

