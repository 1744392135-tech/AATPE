#ifndef AATPED_GPU_H
#define AATPED_GPU_H

#include <string>
#include <mutex>

#include <atomic>
extern std::atomic<int> g_gpuBusyPercent;
extern std::atomic<bool> g_gpuBottleneck;
void readGpuUtilization(void);
bool isGpuBottleneck(float cpuUtilization);
int getGpuBottleneckDecisionInterval(void);

int aatpeParseGedFreqMhz(const std::string& content);

enum GpuPlatform : int {
    GPU_PLATFORM_UNKNOWN = 0,
    GPU_PLATFORM_KGSL   = 1,
    GPU_PLATFORM_MALI    = 2,
};

struct GpuDualState {
    GpuPlatform platform = GPU_PLATFORM_UNKNOWN;
    bool        detected = false;
    bool        available = false;

    std::string model;
    std::string platformTag;
    std::string nodeRoot;

    int curFreqMhz = -1;
    int maxFreqMhz = -1;
    int minFreqMhz = -1;
    int utilPct    = -1;
    int tempC       = -1;
    int voltageUv   = -1;

    std::string nodeCurFreq;
    std::string nodeMaxFreq;
    std::string nodeMinFreq;
    std::string nodeGovernor;
    std::string nodeUtil;
    std::string nodeTemp;
    std::string nodeVoltage;
    std::string nodeModel;

    long long   savedDefaultMaxHz = -1;

    bool        nodeFreqIsKhz = false;

    long long   lastSampleMs = 0;
};

extern GpuDualState g_gpuDual;
extern std::mutex   g_gpuDualMutex;

extern const long long kGpuSampleMinMs;

void gpuDetectPlatform(void);

void gpuSampleAll(void);

void gpuWriteStateFiles(void);

void gpuAppendMetrics(void);

bool gpuSetMaxFreqMhz(int mhz);
bool gpuRestoreDefaultFreq(void);

int         gpuGetPlatform(void);
std::string gpuGetPlatformTag(void);
std::string gpuGetModel(void);

void aatpeGpuRefreshUtilForBoost(void);

#endif
bool aatpeGpuNodeAvailable(int vendor);
void aatpeGpuThrottleTick();
