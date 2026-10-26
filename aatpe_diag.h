// AATPE ROUND-009: Holistic system-pressure diagnostics collector.
// Lightweight read-only sampler: PSI, GPU devfreq, thermal throttle state,
// and per-thread runnable-wait from schedstat. Thread-safe snapshot API.
#ifndef AATPE_DIAG_H
#define AATPE_DIAG_H

#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace aatpe {
namespace diag {

// Thread-safe snapshot of holistic system pressure signals.
// All fields are atomic read under g_diagMutex in the collector thread.
struct SystemPressure {
    // PSI (Pressure Stall Information). available=0 if /proc/pressure missing.
    bool  psiAvailable = false;
    float psiCpuSomePct = 0.0f;   // avg10 of "some" stall on CPU
    float psiCpuSome60Pct = 0.0f; // avg60
    float psiCpuSome300Pct = 0.0f;// avg300
    float psiIoSomePct  = 0.0f;   // avg10 of "some" stall on IO
    float psiIoSome60Pct  = 0.0f; // avg60
    float psiIoSome300Pct  = 0.0f;// avg300
    float psiMemSomePct  = 0.0f;   // avg10 of "some" stall on memory
    float psiMemSome60Pct  = 0.0f; // avg60
    float psiMemSome300Pct  = 0.0f;// avg300
    uint64_t psiCpuTotalUsec = 0;  // cumulative stall microseconds

    // GPU (devfreq). gpuUtil=-1 when util node unavailable (not 0).
    int   gpuUtilPct  = -1;       // 0-100, or -1 = unavailable
    int   gpuFreqKhz   = 0;        // current GPU freq
    int   gpuMaxKhz    = 0;        // max GPU freq
    std::string gpuGovernor;
    std::string gpuDevName;        // detected GPU devfreq node name
    // ROUND-014 hotfix: frequency *control* capability (not merely telemetry).
    // v4.1.1 final-round refinement: ctrl is now wired to an actual WRITABLE
    // control node (access W_OK), NOT merely a readable frequency. A readable
    // devfreq/ged frequency only earns gpuFreqAvail (monitoring).
    //   true  = at least one control node exists AND passes access(W_OK): a devfreq
    //           governor/max_freq/min_freq knob, or a MTK GED write node
    //           (custom_boost_gpu_freq / custom_upbound_gpu_freq / gpu_boost_level),
    //           or a writable file under the platform gpufreq device.
    //   false = we can see frequency/utilisation but cannot write it.
    bool  gpuCtrl = false;
    // v4.1.1 final-round: frequency *monitoring* available (a current-frequency
    // node was read). Independent of gpuCtrl. The panel renders "freq (只读监测)"
    // when freqAvail=1 and ctrl=0.
    bool  gpuFreqAvail = false;
    // True when the GED current-freq node was classified as an OPP/step index
    // rather than a clock (unit ambiguous -> we refuse to fake MHz). gpuFreqKhz
    // is then 0 and the panel shows "OPP level <gpuFreqRaw>".
    bool  gpuFreqIsLevel = false;
    // Raw integer read from the GED frequency node (before unit classification),
    // kept for honest display when gpuFreqIsLevel is true.
    int   gpuFreqRaw = 0;
    // One-line, ';'-separated node probe (name=R/W/N[:value]) written verbatim
    // to the gpu_probe state file. Built read-only during collectGpu.
    std::string gpuProbe;

    // Thermal throttle: scaling_max vs cpuinfo_max per cluster.
    bool  cpuThrottled = false;    // any cluster capped >5% below hw max
    int   cpuCapKhz    = 0;        // current effective max across clusters (lowest)
    int   cpuHwMaxKhz  = 0;        // hardware max (cpuinfo_max)
    bool  gpuThrottled = false;    // GPU devfreq capped below max

    // rq congestion: top-5 threads by runnable-wait time (ms).
    // Sorted descending by wait. Only key threads are polled by the caller.
    struct RqWaitEntry { int tid; int waitMs; };
    std::vector<RqWaitEntry> rqWaitTop;

    // Monotonic timestamp of last successful sample.
    int64_t sampleUptimeSec = 0;
};

// Collect one sample (called from the daemon main loop at ~1-2s cadence).
// Best-effort: missing nodes are logged once at DEBUG, then silently skipped.
void collect();

// Get a copy of the latest snapshot (thread-safe).
SystemPressure getSystemPressureSnapshot();

// Per-thread schedstat accessor: returns runnable-wait time in nanoseconds
// (field 2 of /proc/<tid>/schedstat). Returns -1 on error.
int64_t getThreadRqWaitNs(int tid);

// Per-thread schedstat: total run time in nanoseconds (field 3). -1 on error.
int64_t getThreadRunNs(int tid);

// WD-04: wire the rq_wait_top feature. The caller (main loop, which knows which
// threads are "key") polls getThreadRqWaitNs for its key tids and publishes the
// top-5 here. collect() PRESERVES this across samples (it is caller-populated, not
// sensor-derived), and writeStateFiles() persists it to rq_wait_top. Without this
// the state file was always empty (dead wiring). Pass an empty vector to clear.
void setRqWaitTop(const std::vector<SystemPressure::RqWaitEntry>& top);

// Atomic write all state files to STATE_DIR (single-line, semicolon-separated).
// Called after collect() from the main loop.
void writeStateFiles(const char* stateDir);

// Test seam: override the root path for /proc and /sys (host unit tests).
// Pass "" to reset to production defaults.
#ifdef AATPE_HOST_TEST
void setTestRoot(const std::string& root);
#endif

} // namespace diag
} // namespace aatpe

#endif // AATPE_DIAG_H
