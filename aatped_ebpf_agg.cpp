#include "aatpe_ebpf.h"
#include "aatped_ebpf_agg.h"
#include "aatped.h"
#include "aatpe_recovery.h"
#include <fstream>
#include <unordered_set>

using namespace aatpe;

std::string g_cachedForegroundApp;
long long g_foregroundCacheTimeMs = 0;
static const long long FG_CACHE_TTL_MS = 3000;
std::mutex g_foregroundMutex;

std::string getCachedForegroundApp(void) {
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bool gotLock = false;
    {
        std::unique_lock<std::mutex> fgLock(g_foregroundMutex, std::try_to_lock);
        gotLock = fgLock.owns_lock();
        if (gotLock) {
            if (!g_cachedForegroundApp.empty() &&
                (nowMs - g_foregroundCacheTimeMs) < FG_CACHE_TTL_MS) {
                return g_cachedForegroundApp;
            }
        } else {
            if (!g_cachedForegroundApp.empty()) {
                return g_cachedForegroundApp;
            }
            return "unknown";
        }
    }
    std::string fg = aatpeDetectForegroundApp();
    if (!fg.empty() && fg != "unknown") {
        std::unique_lock<std::mutex> fgLock(g_foregroundMutex, std::try_to_lock);
        if (fgLock.owns_lock()) {
            g_cachedForegroundApp = fg;
            g_foregroundCacheTimeMs = nowMs;
        }
    }
    return fg;
}

static std::unordered_set<int> s_backgroundTids;

static std::unordered_map<int, ThreadInfo>::iterator findBgVictimLocked() {
    for (auto it = s_backgroundTids.begin(); it != s_backgroundTids.end(); ) {
        auto gt = g_threads.find(*it);
        if (gt == g_threads.end() || gt->second.role != ROLE_S5_BACKGROUND || gt->second.protect) {
            it = s_backgroundTids.erase(it);
            continue;
        }
        return gt;
    }
    for (auto mt = g_threads.begin(); mt != g_threads.end(); ++mt) {
        if (!mt->second.protect && mt->second.role == ROLE_S5_BACKGROUND) {
            s_backgroundTids.insert(mt->first);
            return mt;
        }
    }
    return g_threads.end();
}

void AatpeEventConsumer::onFork(const aatpe_ebpf::ForkEvent& ev) {
    g_ebpfEventsFork.fetch_add(1, std::memory_order_relaxed);
    g_ebpfEventsTotal.fetch_add(1, std::memory_order_relaxed);
    pendingForkCount_.fetch_add(1, std::memory_order_relaxed);
    const int childTid = (int)ev.childTid;
    if (childTid > 0) {
        const int maxTracked = g_maxTrackedThreadsAtomic.load(std::memory_order_relaxed);

        int evictTid = -1;
        {
            std::lock_guard<std::mutex> lock(g_threadsMutex);
            auto it = g_threads.find(childTid);
            if (it == g_threads.end()) {
                if ((int)g_threads.size() >= maxTracked) {
                    auto victim = findBgVictimLocked();
                    if (victim == g_threads.end()) {
                        LOG_DEBUG("eBPF FORK: table at cap=%d, no evictable thread; skipping child_tid=%u",
                                  maxTracked, ev.childTid);
                        LOG_DEBUG("eBPF FORK: parent=%u child_pid=%u child_tid=%u cpu=%u",
                                  ev.parentPid, ev.childPid, ev.childTid, ev.hdr.cpu);
                        return;
                    }
                    evictTid = victim->first;
                    s_backgroundTids.erase(evictTid);
                    g_threads.erase(victim);
                }
                ThreadInfo ti;
                ti.tid = childTid;
                ti.pid = (int)ev.childPid;
                ti.starttime = 0;
                ti.tname.clear();
                ti.role = ROLE_S5_BACKGROUND;
                ti.currentCpu = (int)ev.hdr.cpu;
                ti.needsHydration = true;
                ti.starttime = 0;
                g_threads[childTid] = ti;
                s_backgroundTids.insert(childTid);
            }
        }
        if (evictTid > 0) {
            cleanupThreadScoringState(evictTid);
            aatpe_recovery::recoveryErase(evictTid);
        }
    }
    LOG_DEBUG("eBPF FORK: parent=%u child_pid=%u child_tid=%u cpu=%u",
              ev.parentPid, ev.childPid, ev.childTid, ev.hdr.cpu);
}

void AatpeEventConsumer::onExit(const aatpe_ebpf::ExitEvent& ev) {
    g_ebpfEventsExit.fetch_add(1, std::memory_order_relaxed);
    g_ebpfEventsTotal.fetch_add(1, std::memory_order_relaxed);
    int tid = (int)ev.tid;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        auto it = g_threads.find(tid);
        if (it != g_threads.end()) {
            g_threads.erase(it);
        }
        s_backgroundTids.erase(tid);
    }
    cleanupThreadScoringState(tid);
    aatpe_recovery::recoveryErase(tid);
    pendingExitCount_.fetch_add(1, std::memory_order_relaxed);
    LOG_DEBUG("eBPF EXIT: pid=%u tid=%u cpu=%u", ev.pid, ev.tid, ev.hdr.cpu);
}

void AatpeEventConsumer::onSwitch(const aatpe_ebpf::SwitchEvent& ev) {
    g_ebpfEventsSwitch.fetch_add(1, std::memory_order_relaxed);
    g_ebpfEventsTotal.fetch_add(1, std::memory_order_relaxed);

    static uint32_t s_switchProcessCounter{0};
    uint32_t procCount = s_switchProcessCounter++;
    if ((procCount % 10) == 0) {
        const int nextTid = (int)ev.nextPid;
        if (nextTid > 0) {
            std::lock_guard<std::mutex> lock(g_threadsMutex);
            auto it = g_threads.find(nextTid);
            if (it != g_threads.end()) {
                it->second.currentCpu = (int)ev.hdr.cpu;
            }
            long long nowSwitchMs = getTimeMs();
            markThreadScoreDirty(nextTid, nowSwitchMs);
        }
    }

    uint32_t nextPid = ev.nextPid;
    if (nextPid == 0 || nextPid == lastNextPid_) return;
    lastNextPid_ = nextPid;
    int ctr = switchCounter_++ + 1;
    if (ctr < 50) return;
    switchCounter_ = 0;
    std::string pkg = getPackageByPid((int)nextPid);
    if (!pkg.empty() && pkg != "unknown") {
        LOG_DEBUG("eBPF SWITCH: next-to-run pid=%u (%s) [activity only; not foreground]",
                  nextPid, pkg.c_str());
    }
}

void AatpeEventConsumer::onExec(const aatpe_ebpf::ExecEvent& ev) {
    g_ebpfEventsTotal.fetch_add(1, std::memory_order_relaxed);
    pendingExecCount_.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint32_t> s_execProcessCounter{0};
    uint32_t execCount = s_execProcessCounter.fetch_add(1, std::memory_order_relaxed);
    if ((execCount % 3) != 0) return;
    const int tid = (int)ev.tid;
    if (tid > 0) {
        std::string comm(ev.comm, strnlen(ev.comm, sizeof(ev.comm)));
        std::string pname = getPackageByPid((int)ev.pid);
        const int maxTracked = g_maxTrackedThreadsAtomic.load(std::memory_order_relaxed);

        int prio = aatpe::thread_classify::kDefaultPriority;
        std::string subtype;
        ThreadRole role = classifyThread((int)ev.pid, tid, comm, pname, prio, subtype);

        int evictTid = -1;
        {
            std::lock_guard<std::mutex> lock(g_threadsMutex);
            auto it = g_threads.find(tid);
            if (it == g_threads.end()) {
                if ((int)g_threads.size() >= maxTracked) {
                    auto victim = findBgVictimLocked();
                    if (victim == g_threads.end()) {
                        LOG_DEBUG("eBPF EXEC: table at cap=%d, no evictable thread; skipping tid=%u",
                                  maxTracked, ev.tid);
                        return;
                    }
                    evictTid = victim->first;
                    s_backgroundTids.erase(evictTid);
                    g_threads.erase(victim);
                }
                ThreadInfo ti;
                ti.tid = tid;
                ti.pid = (int)ev.pid;
                ti.starttime = 0;
                ti.tname = comm;
                ti.currentCpu = (int)ev.hdr.cpu;
                ti.role = role;
                ti.priority = prio;
                ti.subtype = subtype;
                ti.needsHydration = true;
                ti.protect = (ti.role == ROLE_S0_SYSTEM);
                g_threads[tid] = ti;
                if (ti.role == ROLE_S5_BACKGROUND) s_backgroundTids.insert(tid);
            } else {
                it->second.tname = comm;
                it->second.role = role;
                it->second.priority = prio;
                it->second.subtype = subtype;
                it->second.protect = (it->second.role == ROLE_S0_SYSTEM);
                if (it->second.role == ROLE_S5_BACKGROUND) s_backgroundTids.insert(tid);
                else s_backgroundTids.erase(tid);
            }
        }
        if (evictTid > 0) {
            cleanupThreadScoringState(evictTid);
            aatpe_recovery::recoveryErase(evictTid);
        }
    }
    LOG_DEBUG("eBPF EXEC: pid=%u tid=%u comm=%s cpu=%u",
              ev.pid, ev.tid, ev.comm, ev.hdr.cpu);
}

bool AatpeEventConsumer::hasPendingEvents() const {
    return pendingForkCount_.load() > 0 ||
           pendingExitCount_.load() > 0 ||
           pendingExecCount_.load() > 0;
}

void AatpeEventConsumer::clearPending() {
    pendingForkCount_.store(0);
    pendingExitCount_.store(0);
    pendingExecCount_.store(0);
}

uint64_t AatpeEventConsumer::getPendingForks() const {
    return pendingForkCount_.load();
}

long long AatpeEventConsumer::getTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string AatpeEventConsumer::getPackageByPid(int pid) {
    if (pid <= 0) return "";
    static std::unordered_map<int, std::pair<std::string, long long>> s_pkgCache;
    static std::mutex s_pkgCacheMutex;
    long long nowMs = getTimeMs();
    {
        std::lock_guard<std::mutex> lock(s_pkgCacheMutex);
        auto it = s_pkgCache.find(pid);
        if (it != s_pkgCache.end() && (nowMs - it->second.second) < 2000) {
            return it->second.first;
        }
    }
    std::string cmdlinePath = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream f(cmdlinePath);
    std::string pname;
    if (f) {
        std::getline(f, pname, '\0');
    }
    if (pname.empty()) {
        std::string commPath = "/proc/" + std::to_string(pid) + "/comm";
        pname = readFile(commPath.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(s_pkgCacheMutex);
        s_pkgCache[pid] = {pname, nowMs};
        if (s_pkgCache.size() > 512) {
            auto oldest = s_pkgCache.begin();
            for (auto it = s_pkgCache.begin(); it != s_pkgCache.end(); ++it) {
                if (it->second.second < oldest->second.second) oldest = it;
            }
            s_pkgCache.erase(oldest);
        }
    }
    return pname;
}

void AatpeEventConsumer::onRunqLatency(const aatpe_ebpf::RunqEvent& ev) {
    static std::atomic<uint64_t> s_runqWorstNs{0};
    static std::atomic<uint32_t> s_runqLogCounter{0};
    static std::atomic<uint64_t> s_runqWindowSeq{0};
    uint64_t d = ev.delayNs;
    uint64_t prev = s_runqWorstNs.load(std::memory_order_relaxed);
    while (d > prev &&
           !s_runqWorstNs.compare_exchange_weak(prev, d,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    if (s_runqWindowSeq.fetch_add(1, std::memory_order_relaxed) >= 65536) {
        s_runqWindowSeq.store(0, std::memory_order_relaxed);
        s_runqWorstNs.store(d, std::memory_order_relaxed);
    }
    uint32_t n = s_runqLogCounter.fetch_add(1, std::memory_order_relaxed);
    if (d > 5000000ULL && (n & 63U) == 0) {
        LOG_WARN("[P1-1] runq stall tid=%u cpu=%u delay=%.3fms",
                 ev.pid, ev.targetCpu, d / 1.0e6);
    }
}

AatpeEventConsumer g_eventConsumer;
