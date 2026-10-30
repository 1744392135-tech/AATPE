#include "event_monitor.h"
#include "aatped.h"

using namespace aatpe;

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
    if (fscanf(fp, "%d", &val) != 1) val = -1;
    fclose(fp);
    return val;
}

void pollSysfsEvents(void) {
    if (!g_ueventFallbackActive.load()) return;
    static int s_lastPerfMaxFreq = 0;
    static int s_lastCpuTemp = 0;
    static std::string s_lastOnlineCpus;
    static long long s_lastPollMs = 0;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs - s_lastPollMs < aatpe::fallback::kUeventPollIntervalMs) return;
    s_lastPollMs = nowMs;

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
    if (curPerfMaxFreq > 0 && curPerfMaxFreq != s_lastPerfMaxFreq) {
        aatpe::event::EventMonitor::instance().triggerEvent(
            aatpe::event::EventType::FREQ_CHANGE,
            "perf_max_freq:" + std::to_string(curPerfMaxFreq));
    }
    if (curPerfMaxFreq > 0) s_lastPerfMaxFreq = curPerfMaxFreq;

    int curTemp = -1;
    {
        std::string cpuZonePath = aatpe_getThermalCpuZonePath();
        if (!cpuZonePath.empty()) {
            curTemp = readSysfsInt(cpuZonePath.c_str());
        }
        if (curTemp <= 0) {
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
    if (curTemp > 0) {
        int tAbs = curTemp;
        if (tAbs > 1500)      curTemp /= 1000;
        else if (tAbs > 150)  curTemp /= 10;
    }
    {
        if (curTemp > 0 && s_lastCpuTemp <= 0) {
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
