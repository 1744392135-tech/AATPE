// AATPE round-5 (final convergence): unified subsystem lifecycle.
//
// Every long-lived subsystem (eBPF, WebUI/HTTP, ML, Frame, Thermal, Watchdog,
// CommandQueue) previously tracked only a bare `bool enabled`. There was no
// way for the WebUI /state/all endpoint or the on-disk state directory to
// answer "is eBPF up? when did it last succeed? how many times has it failed?
// has it recovered?". This module provides:
//
//   1. SubsystemState enum  { INIT, STARTING, ACTIVE, DEGRADED, STOPPING,
//                             STOPPED, FAILED }  — a single lifecycle vocabulary.
//   2. SubsystemHealth      { state, last_success, last_error, error_count,
//                             recovery_count, last_error_reason } — the HealthState
//                             contract every subsystem reports.
//   3. Registry            — process-wide singleton; serializes all subsystems to
//                             JSON for WebUI /state/all and writes
//                             <STATE_DIR>/subsystem_health.
//   4. StopToken           — eventfd-based unified stop/wakeup primitive. Replaces
//                             the ad-hoc atomic-bool + sleep/poll pattern so worker
//                             threads (eBPF poll, hot-reload, watchdog) can be woken
//                             and joined immediately on shutdown.
//   5. SafeMode            — latches when eBPF/scheduler fail repeatedly, on crash
//                             loops, memory pressure or thermal emergency. While
//                             active it suppresses RT boost, ML and aggressive
//                             migration (low-risk affinity/monitoring only).
//   6. SelfObservation     — the daemon samples its own CPU/RSS/FD/thread count and
//                             auto-throttles scan/WebUI/ML/placement frequency when
//                             it exceeds budget.
//
// Design notes:
//   - Locking: per-entry mutex only guards the string reason; the counters and
//     state are lock-free atomics. The registry's map itself is populated once at
//     startup (under mMutex) and only read afterwards.
//   - This TU deliberately has NO dependency on the rest of the daemon so the
//     hermetic host unit tests can link it standalone.
#ifndef AATPE_SUBSYSTEM_H
#define AATPE_SUBSYSTEM_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aatpe {
namespace subsys {

// ---------------------------------------------------------------------------
// 1. Unified lifecycle state machine
// ---------------------------------------------------------------------------
enum class SubsystemState : int {
    INIT = 0,       // constructed, not yet started
    STARTING = 1,   // start() in progress
    ACTIVE = 2,     // fully operational
    DEGRADED = 3,   // running in reduced/fallback mode (e.g. eBPF -> polling)
    STOPPING = 4,   // shutdown requested, workers being joined
    STOPPED = 5,    // cleanly stopped
    FAILED = 6      // failed unrecoverably (start rejected, hard error)
};

const char* subsystemStateName(SubsystemState s);

// ---------------------------------------------------------------------------
// 2. HealthState contract
// ---------------------------------------------------------------------------
struct SubsystemHealth {
    std::atomic<SubsystemState> state{SubsystemState::INIT};
    std::atomic<time_t> lastSuccess{0};   // epoch of last successful operation
    std::atomic<time_t> lastError{0};     // epoch of last error
    std::atomic<uint64_t> errorCount{0};
    std::atomic<uint64_t> recoveryCount{0};

    // Human-readable reason for the last error / current degraded mode.
    std::mutex reasonMutex;
    std::string lastErrorReason;

    void setState(SubsystemState s);
    void markSuccess();
    void markError(const std::string& reason);
    void markRecovery();
    SubsystemState getState() const { return state.load(std::memory_order_relaxed); }
    std::string getLastErrorReason();

    // JSON fragment, e.g.  "ebpf":{"state":"ACTIVE","error_count":0,...}
    std::string toJson(const std::string& name);
};

// ---------------------------------------------------------------------------
// 3. Process-wide registry
// ---------------------------------------------------------------------------
class Registry {
public:
    static Registry& instance();

    // Idempotent: registers the slot on first use. Thread-safe after startup.
    SubsystemHealth& get(const std::string& name);

    void setState(const std::string& name, SubsystemState s);
    void markSuccess(const std::string& name);
    void markError(const std::string& name, const std::string& reason);
    void markRecovery(const std::string& name);

    // Aggregate helpers.
    std::vector<std::string> listNames();
    // {"ebpf":{...},"webui":{...},...}
    std::string toJson();
    // Writes <dir>/subsystem_health (JSON array) best-effort.
    void writeStateFile(const char* dir);

    // Number of subsystems currently in FAILED state (for safe-mode gating).
    int failedCount();
    // Number currently in DEGRADED.
    int degradedCount();

private:
    Registry();
    std::mutex mMutex;
    std::map<std::string, std::unique_ptr<SubsystemHealth>> mEntries;
};

// ---------------------------------------------------------------------------
// 4. Unified StopToken (eventfd-based fast wakeup)
// ---------------------------------------------------------------------------
class StopToken {
public:
    StopToken();
    ~StopToken();
    StopToken(const StopToken&) = delete;
    StopToken& operator=(const StopToken&) = delete;

    // Create the eventfd. Returns false on resource exhaustion (caller falls
    // back to polling). Safe to call once at thread start.
    bool open();
    // Idempotently request stop and write 1 to the eventfd (wakes poll()).
    void requestStop();
    bool shouldStop() const { return mStop.load(std::memory_order_acquire); }
    int fd() const { return mFd; }
    bool valid() const { return mFd >= 0; }
    // Drain the eventfd counter so the token can be re-polled after a wakeup.
    void drain();
    // Close and re-arm for a fresh lifecycle (used in restart/reload tests).
    void reset();

private:
    int mFd{-1};
    std::atomic<bool> mStop{false};
};

// ---------------------------------------------------------------------------
// 5. Safe Mode
// ---------------------------------------------------------------------------
// While safe mode is active the daemon keeps ONLY low-risk behaviour:
//   - static affinity pinning / monitoring (read-only)
// It SUSPENDS:
//   - RT boost (SCHED_FIFO priority elevation)
//   - ML-driven placement
//   - aggressive proactive / load-driven migration
//
// Triggers (latched until the cooldown elapses and subsystems recover):
//   - eBPF load failure streak
//   - scheduler/placement failure streak
//   - repeated daemon crash loop (seen via watchdog state file)
//   - memory pressure (self RSS over budget)
//   - thermal emergency
namespace safemode {

extern std::atomic<bool> gSafeModeActive;
extern std::atomic<int>  gSafeModeReason;        // bitmask of SafeReason
extern std::atomic<time_t> gSafeModeEntered;

enum SafeReason : int {
    NONE            = 0,
    EBPF_STREAK     = 1 << 0,
    SCHED_STREAK    = 1 << 1,
    CRASH_LOOP      = 1 << 2,
    MEM_PRESSURE    = 1 << 3,
    THERMAL_EMERG   = 1 << 4,
    SELF_OVERLOAD   = 1 << 5,
};

const char* reasonName(int bit);
// Human-readable combined reason string (e.g. "ebpf_streak+thermal_emergency").
std::string describeReasons(int bits);

// Record a trigger. If it crosses the streak threshold safe mode latches on.
void trigger(SafeReason r, const char* detail);
// G1-001: record that the subsystem behind a streak trigger recovered healthily.
// Resets the consecutive-failure counter for that trigger. Without this,
// "consecutive failures" silently became "cumulative failures" because a long
// healthy period did not reset the streak. Safe to call on every successful poll.
void resetStreak(SafeReason r);
// clear a hard-trigger bit when its condition recovers. If no bits
// remain, safe mode is dropped entirely.
void clearReason(SafeReason r);
// Call each loop; clears safe mode once the cooldown has elapsed AND the
// offending subsystem reports ACTIVE again.
void maybeRecover(std::time_t now);

// Convenience predicates used by placement/RT/ML paths.
bool isActive();
bool rtBoostAllowed();
bool mlPlacementAllowed();
bool aggressiveMigrationAllowed();

// On-change state file writers (best-effort, <dir> is STATE_DIR).
void writeStateFiles(const char* dir);

// Test seams: override the required consecutive-failure streak and the
// recovery cooldown. <=0 restores the production default.
#ifdef AATPE_HOST_TEST  // RC7(C6): test seams, no production callers
void setStreakThresholdForTest(int n);
void setRecoverCooldownSecForTest(int s);

// G1 ROUND-003: CRASH_LOOP observation-window override (>0 = seconds).
extern int g_crashLoopObserveSecTestOverride;
#endif

}  // namespace safemode

// ---------------------------------------------------------------------------
// 6. Self-observation (daemon watches itself)
// ---------------------------------------------------------------------------
namespace selfobs {

// Sampled metrics, updated roughly every 1s by the main loop.
extern std::atomic<int> gSelfCpuPercent;   // % of one core, 0..N
extern std::atomic<long> gSelfRssKb;
extern std::atomic<int> gSelfFdCount;
extern std::atomic<int> gSelfThreadCount;
extern std::atomic<bool> gThrottled;       // currently in self-throttle

// Read /proc/self/stat, /proc/self/status and count FDs/threads. Best-effort;
// on any parse failure the counter is left unchanged. Called ~1Hz.
void sampleSelf();

// Decide whether the daemon must self-throttle (CPU > budget sustained).
// Returns true when throttling should be engaged/kept. When engaged the caller
// lowers scan rate / WebUI update rate / ML / placement frequency.
bool evaluateThrottle();

// Write self_cpu / self_rss / self_fd / self_thread state files.
void writeStateFiles(const char* dir);

// Test seams.
#ifdef AATPE_HOST_TEST  // RC7(C6): test seams, no production callers
void setCpuBudgetPercentForTest(int pct);     // default 50
void setSustainSecForTest(int s);             // default 10
// STATE-01 regression: parse a "/proc/<pid>/stat" tail (everything after the
// closing ") ") and return utime/stime in clock ticks. False on malformed.
#include <string>
bool parseSelfStatCpuForTest(const std::string& afterParen,
                             long long& utimeOut, long long& stimeOut);
#endif

}  // namespace selfobs

}  // namespace subsys
}  // namespace aatpe

#endif  // AATPE_SUBSYSTEM_H
