// AATPE - eBPF event aggregation sub-header
#ifndef AATPED_EBPF_AGG_H
#define AATPED_EBPF_AGG_H

#include <string>
#include <mutex>
#include <atomic>
#include <cstdint>
#include "aatpe_ebpf.h"

// Foreground app cache (shared with eBPF consumer onSwitch)
extern std::string g_cachedForegroundApp;
extern long long g_foregroundCacheTimeMs;
extern std::mutex g_foregroundMutex;
std::string getCachedForegroundApp(void);

// P0-3 : lock-free hot cache of Config::maxTrackedThreads.
// onFork/onExec fire at hundreds-thousands of times/sec; the old per-event
// snapshotConfig() took g_cfgMutex + deep-copied the whole Config (which also
// drags in ML / profile / string state). This atomic is updated ONLY by the
// config hot-reload thread (aatped_config.cpp loadConfig()) and read lock-free
// from the eBPF hot path. Default 512 matches Config::maxTrackedThreads.
// `inline` (C++17) gives one definition merged by the linker across TUs, so
// both the production build and every host test binary that links
// aatped_ebpf_agg.o / aatped_config.o resolve the same symbol.
inline std::atomic<int> g_maxTrackedThreadsAtomic{512};

// eBPF Event Consumer — integrates events with thread list & foreground app
// Method bodies in aatped_ebpf_agg.cpp (needs aatped.h for ThreadInfo etc.)
class AatpeEventConsumer : public aatpe_ebpf::IEventConsumer {
public:
    void onFork(const aatpe_ebpf::ForkEvent& ev) override;
    void onExit(const aatpe_ebpf::ExitEvent& ev) override;
    void onSwitch(const aatpe_ebpf::SwitchEvent& ev) override;
    void onExec(const aatpe_ebpf::ExecEvent& ev) override;
    void onRunqLatency(const aatpe_ebpf::RunqEvent& ev) override;  // P1-1
    bool hasPendingEvents() const;
    void clearPending();
    uint64_t getPendingForks() const;

private:
    std::atomic<uint64_t> pendingForkCount_{0};
    std::atomic<uint64_t> pendingExitCount_{0};
    std::atomic<uint64_t> pendingExecCount_{0};
    // E-006: lastNextPid_/switchCounter_ are plain uint32_t/int. Verified single-
    // threaded: onSwitch() is reached ONLY through dispatchQueuedEvents() on the
    // daemon main loop (the poll thread pushes into the SPSC queue and never calls
    // onSwitch directly), so no cross-thread access exists. They were atomics
    // (LED-024) as a comment-only-enforced invariant; after confirming the
    // dispatch is serial we drop the per-event atomic overhead. If the delivery
    // wiring ever calls onSwitch() from more than one thread, restore atomics.
    uint32_t lastNextPid_{0};
    int switchCounter_{0};
    static long long getTimeMs();
    static std::string getPackageByPid(int pid);
};

extern AatpeEventConsumer g_eventConsumer;

#endif // AATPED_EBPF_AGG_H
