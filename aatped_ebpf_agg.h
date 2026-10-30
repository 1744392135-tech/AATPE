#ifndef AATPED_EBPF_AGG_H
#define AATPED_EBPF_AGG_H

#include <string>
#include <mutex>
#include <atomic>
#include <cstdint>
#include "aatpe_ebpf.h"

extern std::string g_cachedForegroundApp;
extern long long g_foregroundCacheTimeMs;
extern std::mutex g_foregroundMutex;
std::string getCachedForegroundApp(void);

inline std::atomic<int> g_maxTrackedThreadsAtomic{512};

class AatpeEventConsumer : public aatpe_ebpf::IEventConsumer {
public:
    void onFork(const aatpe_ebpf::ForkEvent& ev) override;
    void onExit(const aatpe_ebpf::ExitEvent& ev) override;
    void onSwitch(const aatpe_ebpf::SwitchEvent& ev) override;
    void onExec(const aatpe_ebpf::ExecEvent& ev) override;
    void onRunqLatency(const aatpe_ebpf::RunqEvent& ev) override;
    bool hasPendingEvents() const;
    void clearPending();
    uint64_t getPendingForks() const;

private:
    std::atomic<uint64_t> pendingForkCount_{0};
    std::atomic<uint64_t> pendingExitCount_{0};
    std::atomic<uint64_t> pendingExecCount_{0};
    uint32_t lastNextPid_{0};
    int switchCounter_{0};
    static long long getTimeMs();
    static std::string getPackageByPid(int pid);
};

extern AatpeEventConsumer g_eventConsumer;

#endif
