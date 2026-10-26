// ebpf_agg_deps.cpp - host link deps for aatped_ebpf_agg.cpp. Provides the
// background externals the event consumer references (defined across
// aatped_state.cpp / aatped_utils.cpp on device), without redefining the
// foreground-cache globals that ebpf_agg.cpp itself owns.
#include "aatped.h"

// LOG_DEBUG sampling counter (normally defined in aatped_logging.cpp).
std::atomic<unsigned int> g_debugSampleCounter{0};

// eBPF counters (state.h)
std::atomic<uint64_t> g_ebpfEventsTotal{0};std::atomic<uint64_t> g_ebpfEventsFork{0};
std::atomic<uint64_t> g_ebpfEventsExit{0};
std::atomic<uint64_t> g_ebpfEventsSwitch{0};

// thread bookkeeping
std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;

// foreground app (state.h)
std::string g_foregroundApp;

// fix-v96 #2: onFork() now snapshots the config to enforce maxTrackedThreads.
// Host test env has no real config store; return a default Config (cap=512),
// which the handful of test forks stay well under.
void snapshotConfig(Config& out) { out = Config(); }

// utils.h hooks
void markThreadScoreDirty(int /*tid*/, long long /*nowMs*/) {}
std::string aatpeDetectForegroundApp(bool /*forceRefresh*/) { return "unknown"; }
ThreadRole classifyThread(int /*pid*/, int /*tid*/, const std::string&,
                          const std::string&, int& outPriority, std::string& outSubtype,
                          int* outConfidence, unsigned long long /*cachedStarttime*/) {
    outPriority = aatpe::thread_classify::kDefaultPriority;
    outSubtype.clear();
    if (outConfidence) *outConfidence = 0;
    return ROLE_S5_BACKGROUND;
}
std::string readFile(const char* /*path*/) { return std::string(); }

// P1-4: scoring cleanup hook (no-op in test env)
void cleanupThreadScoringState(int /*tid*/) {}

// P1-8: recovery erase hook (no-op in test env)
#include "aatpe_recovery.h"
namespace aatpe_recovery { void recoveryErase(int /*tid*/) {} }
