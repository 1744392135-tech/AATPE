#ifndef AATPE_CAPS_H
#define AATPE_CAPS_H

#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace aatpe {
namespace caps {

struct DeviceCaps {
    std::string kernelVersion;
    int kernelMajor = 0;
    int kernelMinor = 0;

    enum class CgroupVersion { UNKNOWN, V1, V2 };
    CgroupVersion cgroupVer = CgroupVersion::UNKNOWN;
    std::string cpusetRoot;
    std::string cpuRoot;
    bool cpusetTopAppExists = false;
    std::string cpusetTopAppCpus;

    bool uclampAvailable = false;
    bool uclampTaskAvailable = false;
    bool uclampCgroupV1  = false;
    bool uclampCgroupV2  = false;

    std::string cgroupUclampScale = "unknown";

    bool perThreadCgroupV2 = true;

    bool schedtuneAvailable = false;
    std::string schedtuneRoot;

    bool psiAvailable = false;

    bool gpuDetected = false;
    std::string gpuSource;

    enum class Loader { UNKNOWN, MAGISK, KSU, APATCH };
    Loader loader = Loader::UNKNOWN;

    int pageSize = 4096;
    bool pageSize16K = false;

    bool probeComplete = false;
    int64_t probeUptimeSec = 0;
};

void probe();

DeviceCaps getCaps();

inline const char* classifyGpuPlatform(bool devfreqHasGpu, bool devfreqHasMali,
                               bool devfreqHasKgsl, bool kgslNodePresent,
                               bool gedHalPresent, bool gedDevPresent) {
    if (kgslNodePresent || devfreqHasKgsl) return "kgsl";
    if (devfreqHasMali || devfreqHasGpu)  return "mali";
    if (gedHalPresent || gedDevPresent)   return "ged";
    return "none";
}

std::string resolveCgroupProcsPath(const std::string& group);

std::string resolveCgroupThreadPath(const std::string& group);
std::string resolveCpuCgroupThreadPath(const std::string& group);

int writeTidToCgroupGroup(int tid, const std::string& group);
int writeTidToCpuCgroupGroup(int tid, const std::string& group);

std::string resolveCpuCgroupProcsPath(const std::string& group);

void writeStateFiles(const char* stateDir);

#ifdef AATPE_HOST_TEST
void setTestRoot(const std::string& root);
#endif

float autocalBigRatio();
void autocalSetTestRatio(float r);
void  runAutocalIfDue();

#ifdef AATPE_HOST_TEST
void autocalSetFingerprintForTest(const std::string& fp);
void autocalResetForTest();
#endif

#ifdef AATPE_HOST_TEST
void setTaskUclampProbeResultForTest(bool available, int probeErrno);
#endif

const char* inferCgroupUclampScaleReadOnly(const std::string& nodeValue);

}
}

#endif
