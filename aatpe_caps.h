// AATPE ROUND-010: Universal device capabilities probe layer.
// Read-only startup probe: kernel, cgroup v1/v2, cpuset paths, uclamp,
// schedtune, PSI, GPU, loader type, page size. Thread-safe cached snapshot.
#ifndef AATPE_CAPS_H
#define AATPE_CAPS_H

#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace aatpe {
namespace caps {

struct DeviceCaps {
    // Kernel
    std::string kernelVersion;    // e.g. "4.14.186"
    int kernelMajor = 0;
    int kernelMinor = 0;

    // Cgroup
    enum class CgroupVersion { UNKNOWN, V1, V2 };
    CgroupVersion cgroupVer = CgroupVersion::UNKNOWN;
    std::string cpusetRoot;        // /dev/cpuset or /sys/fs/cgroup
    std::string cpuRoot;           // /dev/cpuctl or /sys/fs/cgroup
    bool cpusetTopAppExists = false;
    std::string cpusetTopAppCpus; // e.g. "4-6"

    // UCLAMP (split: task-level syscall probe vs cgroup node presence).
    bool uclampAvailable = false;
    bool uclampTaskAvailable = false;  // sched_setattr probe (real syscall)
    bool uclampCgroupV1  = false;      // cgroup node exists (v1)
    bool uclampCgroupV2  = false;      // cgroup node exists (v2)

    // ROUND-014 P3: cgroup uclamp scale detection (101 readback probe).
    // "percent" = kernel expects 0-100 percentage (v2 standard, most v1 backports)
    // "raw1024" = kernel expects 0-1024 raw util (2019 LPC prototype, rare)
    // "unknown" = probe failed; safe default is "percent"
    std::string cgroupUclampScale = "unknown";

    // ROUND-013: per-thread cgroup v2 (cgroup.threads) runtime capability.
    // Starts true; set false if kernel returns EOPNOTSUPP/EBUSY/EINVAL.
    bool perThreadCgroupV2 = true;

    // Schedtune
    bool schedtuneAvailable = false;
    std::string schedtuneRoot;     // /dev/stune

    // PSI
    bool psiAvailable = false;

    // GPU (reuse diag discovery result)
    bool gpuDetected = false;
    std::string gpuSource;

    // Loader environment
    enum class Loader { UNKNOWN, MAGISK, KSU, APATCH };
    Loader loader = Loader::UNKNOWN;

    // Page size
    int pageSize = 4096;
    bool pageSize16K = false;

    // Probe health
    bool probeComplete = false;
    int64_t probeUptimeSec = 0;
};

// Run the full probe (called once at startup). Safe to call again for retry.
void probe();

// Get the cached snapshot (thread-safe).
DeviceCaps getCaps();

// v4.4.4 (C4/GPU hardening): PURE GPU platform classifier. Given already-detected
// presence flags (the glue does the sysfs I/O), return the platform tag:
//   "kgsl" (Adreno) / "mali" / "ged" (MTK) / "none".
// Priority: kgsl > mali > ged > none. Header-only inline so host tests link it
// without pulling the whole aatpe_caps.o (which carries autocal/probe deps).
inline const char* classifyGpuPlatform(bool devfreqHasGpu, bool devfreqHasMali,
                               bool devfreqHasKgsl, bool kgslNodePresent,
                               bool gedHalPresent, bool gedDevPresent) {
    if (kgslNodePresent || devfreqHasKgsl) return "kgsl";  // Adreno
    if (devfreqHasMali || devfreqHasGpu)  return "mali";  // Mali (generic gpu devfreq)
    if (gedHalPresent || gedDevPresent)   return "ged";    // MTK GED
    return "none";
}

// Resolve the cgroup.procs / tasks path for a given group name (process-level).
std::string resolveCgroupProcsPath(const std::string& group);

// ROUND-013: resolve the THREAD-LEVEL cgroup path.
// v1: <root>/<group>/tasks  (v1 tasks = thread-level)
// v2: <root>/<group>/cgroup.threads  (NOT cgroup.procs = process-level)
std::string resolveCgroupThreadPath(const std::string& group);
std::string resolveCpuCgroupThreadPath(const std::string& group);

// Write tid to a cgroup group's thread file. Returns:
//   0 = success
//  -1 = generic IO error
//  -2 = kernel rejected thread-level granularity (EOPNOTSUPP/EBUSY/EINVAL)
// On -2, perThreadCgroupV2 is cached to false; no fallback to cgroup.procs.
int writeTidToCgroupGroup(int tid, const std::string& group);
int writeTidToCpuCgroupGroup(int tid, const std::string& group);

// Resolve the CPU controller process-level path (for future process-group moves).
std::string resolveCpuCgroupProcsPath(const std::string& group);

// Atomic-write capabilities state files (called after probe).
void writeStateFiles(const char* stateDir);

// Test seam: override root paths.
#ifdef AATPE_HOST_TEST
void setTestRoot(const std::string& root);
#endif

// ---- v4.2 A3: capacity self-calibration -----------------------------------
// autocalBigRatio() returns the BIG-cluster capacity ratio used to scale the
// uclamp.floor. DEFAULT (no state/autocal.json / corrupt / topology-fingerprint
// mismatch) == 0.87f, bit-identical to the v4.1.1 hardcoded literal. No runtime
// switch — absence of a valid file IS the off state. Cached after first load.
float autocalBigRatio();
// v6.0.0 round4c test seam: force a fixed BIG ratio (r<0 resets). Host-only.
void autocalSetTestRatio(float r);
// Called from the placement tick. No-op unless charging + screen-off + idle AND no
// valid calibration already on disk; never blocks / crashes on failure. On success
// writes state/autocal.json and the autocal_ratio (x1000) state key.
void  runAutocalIfDue();

#ifdef AATPE_HOST_TEST
// Test seams: force the calibration fingerprint (production uses
// aatpe_ml::computeDeviceTopologyHash()) and drop the in-memory cache.
void autocalSetFingerprintForTest(const std::string& fp);
void autocalResetForTest();
#endif

// ROUND-013b: test seam for task-uclamp probe result (host hermetic tests).
#ifdef AATPE_HOST_TEST
void setTaskUclampProbeResultForTest(bool available, int probeErrno);
#endif

// RC6 (B1): READ-ONLY cgroup uclamp scale inference (pure, no I/O). The boot probe
// never writes the shared cgroup node; this maps the node's current resting value to
// "percent"/"raw1024"/"unknown". Exposed for host unit-testing.
const char* inferCgroupUclampScaleReadOnly(const std::string& nodeValue);

} // namespace caps
} // namespace aatpe

#endif // AATPE_CAPS_H
