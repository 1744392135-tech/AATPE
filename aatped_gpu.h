// AATPE - GPU monitoring sub-header (Qualcomm KGSL + ARM Mali dual platform)
#ifndef AATPED_GPU_H
#define AATPED_GPU_H

#include <string>
#include <mutex>

// GPU bottleneck awareness (legacy, kept for placement scoring)
// C-010: changed to atomic for thread-safe cross-thread access.
#include <atomic>
extern std::atomic<int> g_gpuBusyPercent;
extern std::atomic<bool> g_gpuBottleneck;
void readGpuUtilization(void);
bool isGpuBottleneck(float cpuUtilization);
int getGpuBottleneckDecisionInterval(void);

// v4.2.7: parse GED freq from text content (handles "curfreq=836000" or raw "836000").
// Returns MHz value (KHz→MHz auto-conversion for values >100000).
int aatpeParseGedFreqMhz(const std::string& content);

// ============================================================================
// Dual-platform GPU subsystem (KGSL / Mali / devfreq)
//
// Platform is auto-detected once and cached. Probe order is deterministic:
//   1. Qualcomm Adreno KGSL   : /sys/class/kgsl/kgsl-3d0/*
//   2. ARM Mali               : /sys/class/misc/mali0[/device]/*, /sys/kernel/gpu/*
//   3. Generic devfreq node   : /sys/class/devfreq/<gpu-ish>/*  (bonus layer)
//   4. /proc/cpuinfo Hardware : coarse SoC family guess (no sysfs nodes)
//   5. unknown
//
// Every periodic sample reads the cached vendor sysfs nodes, normalizes units
// (Hz->MHz, millideg->C, 0..1000 load->0..100%), publishes normalized gpu_*
// state files under STATE_DIR for WebUI/APK consumers and appends a throttled
// CSV record under METRICS_DIR for history. All shared state is guarded by
// g_gpuDualMutex; raw sysfs reads are self-throttled to >= kGpuSampleMinMs.
// ============================================================================

enum GpuPlatform : int {
    GPU_PLATFORM_UNKNOWN = 0,
    GPU_PLATFORM_KGSL   = 1,   // Qualcomm Adreno, kgsl-3d0
    GPU_PLATFORM_MALI    = 2,   // ARM Mali, mali0 / devfreq mali
};

struct GpuDualState {
    GpuPlatform platform = GPU_PLATFORM_UNKNOWN;
    bool        detected = false;     // true once a platform node was found
    bool        available = false;     // true when at least one usable node works

    std::string model;                 // e.g. "Adreno (TM) 650" / "Mali-G78"
    std::string platformTag;            // "kgsl" / "mali" / "unknown"
    std::string nodeRoot;               // resolved sysfs root actually used

    int curFreqMhz = -1;               // MHz on success, -1 if node missing
    int maxFreqMhz = -1;
    int minFreqMhz = -1;
    int utilPct    = -1;               // 0..100, -1 unknown
    int tempC       = -1;               // degrees C, -1 unknown
    int voltageUv   = -1;               // microvolt, -1 unknown

    // Discovered node paths (filled at detection time, used at sample time)
    std::string nodeCurFreq;
    std::string nodeMaxFreq;
    std::string nodeMinFreq;
    std::string nodeGovernor;          // discovered governor node (e.g. /proc/gpufreq/gpufreq_governor)
    std::string nodeUtil;
    std::string nodeTemp;
    std::string nodeVoltage;
    std::string nodeModel;

    // Saved default max freq (Hz) so setMaxFreq can be restored.
    long long   savedDefaultMaxHz = -1;

    // LED-048 (R4-004): some devfreq/Mali nodes report frequency in kHz, not Hz.
    // The read-side rescaleKhz heuristic detects this; we cache the verdict here
    // so the write side (gpuSetMaxFreqMhz) scales MHz->kHz instead of MHz->Hz.
    bool        nodeFreqIsKhz = false;

    // Self-throttle: last wall-clock ms a real sysfs read happened.
    long long   lastSampleMs = 0;
};

extern GpuDualState g_gpuDual;
extern std::mutex   g_gpuDualMutex;

// Min interval between two real sysfs samples (ms). Callers may invoke the
// sample path more often (e.g. every decision cycle); reads are skipped when
// fresher than this.
extern const long long kGpuSampleMinMs;

// One-shot platform detection (idempotent, cached). Safe to call repeatedly.
void gpuDetectPlatform(void);

// Full sample: freq / util / temp / voltage / model. Locks g_gpuDualMutex.
// Self-throttled to kGpuSampleMinMs; returns early when too soon.
void gpuSampleAll(void);

// Write normalized gpu_* state files under STATE_DIR (WebUI/APK consumers).
void gpuWriteStateFiles(void);

// Append one throttled history record under METRICS_DIR/gpu_metrics.csv.
// The file is rolled (truncated to its header line) once it exceeds ~256 KiB.
void gpuAppendMetrics(void);

// Frequency cap control. mhz is clamped to [minFreqMhz, maxFreqMhz].
// Returns true on success; logs errno-classified failures.
bool gpuSetMaxFreqMhz(int mhz);
// Restore the max freq captured at detection time.
bool gpuRestoreDefaultFreq(void);

// Convenience accessors (lock + copy) for JSON / metrics producers.
int         gpuGetPlatform(void);
std::string gpuGetPlatformTag(void);
std::string gpuGetModel(void);

// F-07: lightweight util refresh for the 50ms boost tick. Calls gpuSampleAll()
// (self-throttled to 1s) and mirrors utilPct to g_gpuUtilization. Cheap to call
// every decision cycle; no-op inside the 1s sample window.
void aatpeGpuRefreshUtilForBoost(void);

#endif // AATPED_GPU_H
bool aatpeGpuNodeAvailable(int vendor);
void aatpeGpuThrottleTick();
