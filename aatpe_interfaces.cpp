// aatpe_interfaces.cpp - 接口实现 (P3接口接入)
#include "aatpe_interfaces.h"
#include "aatped.h"
#include "aatpe_ml.h"
#include "aatped_topology.h"
#include <cstring>
#include <mutex>

IScorer* g_scorer = nullptr;
IPlacementDecider* g_placementDecider = nullptr;
ICpuMonitor* g_cpuMonitor = nullptr;
IEventSource* g_eventSource = nullptr;
IMlModel* g_mlModelInterface = nullptr;

static DefaultScorer s_defaultScorer;
static DefaultPlacementDecider s_defaultDecider;
static DefaultCpuMonitor s_defaultCpuMonitor;
static PollingEventSource s_pollingEvent;
static OnlineLearningModel s_mlModel;

int DefaultScorer::computeScore(int cpuId, const ThreadInfo& ti) {
    return ::computeScore(cpuId, ti);
}

Decision DefaultPlacementDecider::decidePlacement(const ThreadInfo& ti, const Config& cfg, bool emergency) {
    return ::decidePlacement(ti, cfg, emergency);
}

int DefaultCpuMonitor::getCpuCount() {
    // MON-01 fix: g_cpuCount is written by detectCpuTopology() under
    // g_topologyRwMutex; reading it without the lock is a data race.
    // Mirror event_monitor.cpp:103's locking discipline.
    std::lock_guard<std::mutex> lk(g_topologyRwMutex);
    return g_cpuCount;
}
float DefaultCpuMonitor::getCpuUtilization(int cpuId) {
    return readCpuUtilizationSchedstat(cpuId);
}
int DefaultCpuMonitor::getCpuFrequency(int cpuId) {
    (void)cpuId;
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpuId);
    FILE* f = fopen(path, "r");
    if (f) {
        int freq = 0;
        if (fscanf(f, "%d", &freq) == 1) { fclose(f); return freq; }
        fclose(f);
    }
    return 2000000;
}

// A5: sysfs thermal_zone temp is normally millidegrees (50000 = 50C), but some
// zones report centidegrees (5025 = 50.25C) or even whole degrees (50). The old
// `temp/1000` assumed millidegrees and returned 0 for centi/whole-degree sensors.
// Mirror the hwmon magnitude test in thermal.cpp: >1500 already milli, >150 centi,
// else whole-degrees. Normalize to millidegrees then divide by 1000.
static int normalizeTempToC(int raw) {
    int a = raw < 0 ? -raw : raw;
    int milli = (a > 1500) ? raw : (a > 150 ? raw * 100 : raw * 1000);
    return milli / 1000;
}

// F04[P3]: classify a thermal_zone type string as a plausible SoC/CPU temperature
// source. Factored out (a) so it can be unit-tested on host without /sys and (b) so
// the fallback below does not blindly read zone0, which on many devices is the
// battery/skin/charger zone. Battery/skin/charger/gpu/pch names that would
// otherwise substring-match ("ap"/"soc") are explicitly rejected.
static bool thermalZoneLooksLikeCpu(const char* type) {
    if (!type || !*type) return false;
    if (strcasestr(type, "battery") || strcasestr(type, "skin") ||
        strcasestr(type, "pcb")     || strcasestr(type, "board") ||
        strcasestr(type, "charger")  || strcasestr(type, "gpu") ||
        strcasestr(type, "pch")      || strcasestr(type, "usb")) {
        return false;
    }
    return (strcasestr(type, "cpu")   != nullptr ||
            strcasestr(type, "soc")   != nullptr ||
            strcasestr(type, "apc")   != nullptr ||
            strcasestr(type, "tsens") != nullptr);
}

int DefaultCpuMonitor::getTemperature() {
    // P1-fix (issue 32): previously hardcoded thermal_zone0, which on many
    // devices is battery/skin/GPU, not CPU. Scan thermal zones looking for one
    // whose type string is a CPU/SoC zone.
    for (int z = 0; z < 16; z++) {
        char tpath[128];
        char npath[128];
        snprintf(tpath, sizeof(tpath), "/sys/class/thermal/thermal_zone%d/temp", z);
        snprintf(npath, sizeof(npath), "/sys/class/thermal/thermal_zone%d/type", z);
        FILE* nf = fopen(npath, "r");
        if (!nf) continue;
        char type[64] = {0};
        if (fgets(type, sizeof(type), nf) == nullptr) { fclose(nf); continue; }
        fclose(nf);
        // Trim newline
        size_t L = strlen(type);
        while (L > 0 && (type[L-1]=='\n'||type[L-1]=='\r')) type[--L] = '\0';
        if (!thermalZoneLooksLikeCpu(type)) continue;
        FILE* f = fopen(tpath, "r");
        if (!f) continue;
        int temp = 0;
        if (fscanf(f, "%d", &temp) == 1) { fclose(f); return normalizeTempToC(temp); }
        fclose(f);
    }
    // F04[P3]: do NOT blindly fall back to zone0. On many devices zone0 is the
    // battery/skin/charger zone; reading it as "CPU temp" injected noise into the
    // ML ThreadFeatures.thermal feature. Only adopt zone0 if its OWN type passes
    // the CPU/SoC classifier; otherwise return the unknown sentinel (40) so the ML
    // feature treats the reading as missing rather than as battery temperature.
    {
        char npath[128];
        snprintf(npath, sizeof(npath), "/sys/class/thermal/thermal_zone0/type");
        FILE* nf = fopen(npath, "r");
        if (nf) {
            char type[64] = {0};
            if (fgets(type, sizeof(type), nf) != nullptr) {
                size_t L = strlen(type);
                while (L > 0 && (type[L-1]=='\n'||type[L-1]=='\r')) type[--L] = '\0';
                if (thermalZoneLooksLikeCpu(type)) {
                    FILE* f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
                    if (f) {
                        int temp = 0;
                        if (fscanf(f, "%d", &temp) == 1) { fclose(f); return normalizeTempToC(temp); }
                        fclose(f);
                    }
                }
            }
            fclose(nf);
        }
    }
    return 40;
}

#ifdef AATPE_HOST_TEST
// Test seam: expose the zone classifier for host unit tests (no /sys needed).
bool aatpeThermalZoneLooksLikeCpuForTest(const char* type) { return thermalZoneLooksLikeCpu(type); }
#endif

// P2-7: Explicit placeholder. Production daemon uses inotify-based
// EventMonitor. This poll()-based fallback always returns false (no event).
bool PollingEventSource::poll(int timeoutMs) {
    (void)timeoutMs;
    return false;
}

static aatpe_ml::ThreadFeatures featuresFromVector(const std::vector<float>& f) {
    aatpe_ml::ThreadFeatures tf{};
    if (f.size() >= 1)  tf.avg_run_us        = f[0];
    if (f.size() >= 2)  tf.nr_switches       = f[1];
    if (f.size() >= 3)  tf.cpu_usage         = f[2];
    if (f.size() >= 4)  tf.priority          = f[3];
    if (f.size() >= 5)  tf.nice              = f[4];
    if (f.size() >= 6)  tf.is_game           = f[5];
    if (f.size() >= 7)  tf.thermal           = f[6];
    if (f.size() >= 8)  tf.capacity          = f[7];
    if (f.size() >= 9)  tf.current_cpu_match = f[8];
    if (f.size() >= 10) tf.migration_count   = f[9];
    if (f.size() >= 11) tf.cross_run_cap     = f[10];
    if (f.size() >= 12) tf.cross_run_usage   = f[11];
    if (f.size() >= 13) tf.cross_cap_thermal = f[12];
    if (f.size() >= 14) tf.cross_run_thermal = f[13];
    return tf;
}

// P1-5: poolIdx passed through to g_mlModel (was hardcoded to 0).
float OnlineLearningModel::predict(int poolIdx, const std::vector<float>& features) {
    aatpe_ml::ThreadFeatures tf = featuresFromVector(features);
    std::lock_guard<std::mutex> lock(g_mlMutex);
    return g_mlModel.predict(poolIdx, tf);
}

void OnlineLearningModel::update(int poolIdx, const std::vector<float>& features, float target) {
    aatpe_ml::ThreadFeatures tf = featuresFromVector(features);
    std::lock_guard<std::mutex> lock(g_mlMutex);
    g_mlModel.update(poolIdx, tf, target);
}

// P1-4: Per-pool weight access via getPoolModel(poolIdx).
std::vector<float> OnlineLearningModel::getWeights(int poolIdx) const {
    std::lock_guard<std::mutex> lock(g_mlMutex);
    aatpe_ml::PoolModel pm = g_mlModel.getPoolModel(poolIdx);
    std::vector<float> w;
    w.reserve(aatpe_ml::ML_WEIGHT_COUNT);
    for (int i = 0; i < aatpe_ml::ML_WEIGHT_COUNT; i++) w.push_back(pm.weights[i]);
    return w;
}

void OnlineLearningModel::setWeights(int poolIdx, const std::vector<float>& w) {
    std::lock_guard<std::mutex> lock(g_mlMutex);
    // call per-pool setWeights instead of full-model setWeights
    g_mlModel.setWeights(poolIdx, w);
}

int OnlineLearningModel::getPoolCount() const {
    std::lock_guard<std::mutex> lock(g_mlMutex);
    return g_mlModel.getPoolCount();
}

Decision MockPlacementDecider::decidePlacement(const ThreadInfo& ti, const Config& cfg, bool emergency) {
    (void)ti; (void)cfg; (void)emergency;
    Decision d;
    d.targetCpu = 0;
    d.reason = "mock";
    return d;
}

void initInterfaces() {
    g_scorer = &s_defaultScorer;
    g_placementDecider = &s_defaultDecider;
    g_cpuMonitor = &s_defaultCpuMonitor;
    g_eventSource = &s_pollingEvent;
    g_mlModelInterface = &s_mlModel;
}

void cleanupInterfaces() {
    g_scorer = nullptr;
    g_placementDecider = nullptr;
    g_cpuMonitor = nullptr;
    g_eventSource = nullptr;
    g_mlModelInterface = nullptr;
}
