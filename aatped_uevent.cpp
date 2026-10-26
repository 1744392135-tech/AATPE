// AATPE - uEvent fallback polling (extracted from aatped_main.cpp)
#include "event_monitor.h"
#include "aatped.h"

using namespace aatpe;

// ==================== uevent fallback polling ====================
std::atomic<bool> g_ueventFallbackActive(false);
std::atomic<int> g_ueventEventCount(0);
static std::atomic<long long> g_ueventStartTimeMs{0};

void checkUeventFallback(void) {
    if (g_ueventFallbackActive.load()) return;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_ueventStartTimeMs.load() == 0) {
        g_ueventStartTimeMs.store(nowMs);
        return;
    }
    // 5 seconds after start, if 0 uevent events received, activate fallback
    // magic numbers wired to aatpe::fallback:: (central registry).
    if (nowMs - g_ueventStartTimeMs.load() > aatpe::fallback::kUeventActivateAfterMs && g_ueventEventCount.load() == 0) {
        g_ueventFallbackActive.store(true);
        LOG_INFO("fix95: uevent inactive after %lldms, activating sysfs polling fallback",
                 aatpe::fallback::kUeventActivateAfterMs);
    }
}

static int readSysfsInt(const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) return -1;
    int val = -1;
    // C-024: check fscanf return value.
    if (fscanf(fp, "%d", &val) != 1) val = -1;
    fclose(fp);
    return val;
}

void pollSysfsEvents(void) {
    if (!g_ueventFallbackActive.load()) return;
    // P2-10: these statics are only accessed from pollSysfsEvents() which runs
    // on the daemon main loop (single-threaded). No mutex needed.
    static int s_lastPerfMaxFreq = 0;
    static int s_lastCpuTemp = 0;
    static std::string s_lastOnlineCpus;
    static long long s_lastPollMs = 0;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // poll interval wired to aatpe::fallback::kUeventPollIntervalMs.
    if (nowMs - s_lastPollMs < aatpe::fallback::kUeventPollIntervalMs) return;
    s_lastPollMs = nowMs;

    // CPU频率轮询 — 遍历大核+中核首核心取max频率, 不再硬编码cpu0
    // 问题: 硬编码cpu0(小核), 中核/大核频率变化无法触发事件。
    // 修复: 遍历g_bigPool全部+g_midPool[0]核心(性能核), 取max(curFreq)变化检测。
    int curPerfMaxFreq = 0;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        std::vector<int> perfCores;
        for (int c : g_bigPool) perfCores.push_back(c);
        if (!g_midPool.empty()) perfCores.push_back(g_midPool[0]);
        if (perfCores.empty()) {
            perfCores.push_back(0);
            if (!g_allCpus.empty()) perfCores.push_back(g_allCpus.back());
        }
        for (int cpu : perfCores) {
            std::string freqPath = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq";
            int f = readSysfsInt(freqPath.c_str());
            if (f > 0 && f > curPerfMaxFreq) curPerfMaxFreq = f;
        }
    }
    // FREQ_CHANGE magnitude gate is centralized in EventMonitor::triggerEvent().
    // Here we just detect frequency change; triggerEvent() handles the 15%/200MHz threshold.
    if (curPerfMaxFreq > 0 && curPerfMaxFreq != s_lastPerfMaxFreq) {
        aatpe::event::EventMonitor::instance().triggerEvent(
            aatpe::event::EventType::FREQ_CHANGE,
            "perf_max_freq:" + std::to_string(curPerfMaxFreq));
    }
    if (curPerfMaxFreq > 0) s_lastPerfMaxFreq = curPerfMaxFreq;

    // 温度轮询 — 使用aatpe_getThermalCpuZonePath()智能检测的CPU区域, 不再硬编码thermal_zone0
    int curTemp = -1;
    {
        std::string cpuZonePath = aatpe_getThermalCpuZonePath();
        if (!cpuZonePath.empty()) {
            curTemp = readSysfsInt(cpuZonePath.c_str());
        }
        if (curTemp <= 0) {
            // 兜底: 扫描全部thermal zone取最高可读温度 (G6-021: keep RAW here;
            // unit normalization is done once below at line ~110 to avoid double
            // conversion). Lower bound rejected to avoid garbage negative readings.
            DIR* tzDir = opendir("/sys/class/thermal");
            if (tzDir) {
                struct dirent* e;
                while ((e = readdir(tzDir)) != nullptr) {
                    if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
                    std::string tp = std::string("/sys/class/thermal/") + e->d_name + "/temp";
                    int t = readSysfsInt(tp.c_str());
                    if (t > -50000 && t > curTemp && t <= 200000) curTemp = t;
                }
                closedir(tzDir);
            }
        }
    }
    // Three-tier unit normalization (matches thermal.cpp readThermal()):
    //   |raw| > 1500  -> millicelsius (m°C), divide by 1000
    //   |raw| >  150  -> deci-Celsius (0.1°C), divide by 10
    //   otherwise      -> Celsius (°C), use as-is
    // Preserve sign on conversion.
    if (curTemp > 0) {
        int tAbs = curTemp;
        if (tAbs > 1500)      curTemp /= 1000;  // millicelsius
        else if (tAbs > 150)  curTemp /= 10;    // 0.1°C
        // else already Celsius
    }
    // THERMAL_SPIKE only on warming (delta>0), threshold 5 (was abs>=2).
    // Cooling down must never trigger emergency migration.
    // LED-107 (R7-022): s_lastCpuTemp starts at 0, so the first valid reading was
    // always gated out by `s_lastCpuTemp > 0` — a high temperature already present
    // when the sysfs fallback activated (mid thermal event, netlink lost) needed a
    // further 5°C rise on the *second* poll before it fired. Special-case the first
    // valid baseline: if the CPU is already at/above the warm threshold, fire once
    // immediately (we are entering the event mid-stream), then continue with the
    // normal warming-delta detection.
    {
        if (curTemp > 0 && s_lastCpuTemp <= 0) {
            // First valid reading: establish baseline. If already warm, fire now.
            static constexpr int kFirstFrameWarmC = 60;
            if (curTemp >= kFirstFrameWarmC) {
                aatpe::event::EventMonitor::instance().triggerEvent(
                    aatpe::event::EventType::THERMAL_SPIKE,
                    "temp:" + std::to_string(curTemp));
            }
        } else if (curTemp > 0 && s_lastCpuTemp > 0) {
            int tempDelta = curTemp - s_lastCpuTemp;
            if (tempDelta >= 5) {
                aatpe::event::EventMonitor::instance().triggerEvent(
                    aatpe::event::EventType::THERMAL_SPIKE,
                    "temp:" + std::to_string(curTemp));
            }
        }
    }
    if (curTemp > 0) s_lastCpuTemp = curTemp;

    // Hotplug poll
    FILE* fp = fopen("/sys/devices/system/cpu/online", "r");
    if (fp) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            std::string curOnline(buf);
            if (!s_lastOnlineCpus.empty() && curOnline != s_lastOnlineCpus) {
                aatpe::event::EventMonitor::instance().triggerEvent(
                    aatpe::event::EventType::HOTPLUG, "online:" + curOnline);
            }
            s_lastOnlineCpus = curOnline;
        }
        fclose(fp);
    }
}
