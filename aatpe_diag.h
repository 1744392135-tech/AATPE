#ifndef AATPE_DIAG_H
#define AATPE_DIAG_H

#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace aatpe {
namespace diag {

struct SystemPressure {
    bool  psiAvailable = false;
    float psiCpuSomePct = 0.0f;
    float psiCpuSome60Pct = 0.0f;
    float psiCpuSome300Pct = 0.0f;
    float psiIoSomePct  = 0.0f;
    float psiIoSome60Pct  = 0.0f;
    float psiIoSome300Pct  = 0.0f;
    float psiMemSomePct  = 0.0f;
    float psiMemSome60Pct  = 0.0f;
    float psiMemSome300Pct  = 0.0f;
    uint64_t psiCpuTotalUsec = 0;

    int   gpuUtilPct  = -1;
    int   gpuFreqKhz   = 0;
    int   gpuMaxKhz    = 0;
    std::string gpuGovernor;
    std::string gpuDevName;
    bool  gpuCtrl = false;
    bool  gpuFreqAvail = false;
    bool  gpuFreqIsLevel = false;
    int   gpuFreqRaw = 0;
    std::string gpuProbe;

    bool  cpuThrottled = false;
    int   cpuCapKhz    = 0;
    int   cpuHwMaxKhz  = 0;
    bool  gpuThrottled = false;

    struct RqWaitEntry { int tid; int waitMs; };
    std::vector<RqWaitEntry> rqWaitTop;

    int64_t sampleUptimeSec = 0;
};

void collect();

SystemPressure getSystemPressureSnapshot();

int64_t getThreadRqWaitNs(int tid);

int64_t getThreadRunNs(int tid);

void setRqWaitTop(const std::vector<SystemPressure::RqWaitEntry>& top);

void writeStateFiles(const char* stateDir);

#ifdef AATPE_HOST_TEST
void setTestRoot(const std::string& root);
#endif

}
}

#endif
