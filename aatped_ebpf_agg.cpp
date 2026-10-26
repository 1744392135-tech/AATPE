// AATPE - eBPF event aggregation / foreground cache (extracted from aatped_main.cpp)
#include "aatpe_ebpf.h"
#include "aatped_ebpf_agg.h"
#include "aatped.h"
#include "aatpe_recovery.h" // P1-8: recoveryErase on thread exit
#include <fstream>
#include <unordered_set>

using namespace aatpe;

// ==================== fix95 Task2: foreground app cache ====================
std::string g_cachedForegroundApp;
long long g_foregroundCacheTimeMs = 0;
static const long long FG_CACHE_TTL_MS = 3000;
std::mutex g_foregroundMutex;

std::string getCachedForegroundApp(void) {
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // Fast path: cache hit under lock.
    // D1 deadlock fix (v4.3.1h5): NEVER block the decision loop on g_foregroundMutex.
    // Use try_lock; if the lock is held (e.g. a nested foreground-detection path on
    // the same thread already owns it), return the last cached value immediately
    // instead of self-deadlocking. A stale fg for one cycle is harmless.
    bool gotLock = false;
    {
        std::unique_lock<std::mutex> fgLock(g_foregroundMutex, std::try_to_lock);
        gotLock = fgLock.owns_lock();
        if (gotLock) {
            // We hold the lock: serve a fresh cache hit straight away.
            if (!g_cachedForegroundApp.empty() &&
                (nowMs - g_foregroundCacheTimeMs) < FG_CACHE_TTL_MS) {
                return g_cachedForegroundApp;
            }
            // Lock is ours but the cache is empty or stale -> fall through and
            // refresh via the slow path below. fgLock goes out of scope here,
            // releasing the lock (required: aatpeDetectForegroundApp() re-acquires
            // g_foregroundMutex itself; re-locking a non-recursive std::mutex on
            // this thread would be UB / deadlock).
        } else {
            // N4 (v4.4.0): we could NOT take the lock, so another thread owns it
            // (typically an in-flight foreground detection that blocks on
            // g_foregroundMutex and may fork+exec `dumpsys` for up to ~3s). The
            // old code fell through to aatpeDetectForegroundApp() in this branch,
            // which re-blocked on the very same mutex and waited out dumpsys --
            // defeating try_lock entirely and stalling the decision tick. Never
            // run the slow path when we could not get the lock: hand back the last
            // cached value (even if slightly stale) so the loop is never blocked.
            // If we have no cache at all, report "unknown".
            if (!g_cachedForegroundApp.empty()) {
                return g_cachedForegroundApp;
            }
            return "unknown";
        }
    }
    // Slow path: aatpeDetectForegroundApp() internally locks g_foregroundMutex
    // (via aatpeGetForegroundPackage / aatpeSetForegroundLocked). We MUST release
    // the lock before calling it, otherwise the same thread re-locks a
    // non-recursive std::mutex -> UB / deadlock (deadlock root cause).
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

// ============================================================================
// v73: eBPF Event Consumer method bodies
// ============================================================================

// P2: eviction acceleration. onFork/onExec used to linearly scan the
// whole g_threads map (up to maxTrackedThreads=512) for the first non-protected
// ROLE_S5_BACKGROUND victim EVERY time the table hit capacity. This set tracks
// background tids inserted/updated by THIS TU (onFork placeholders, onExec
// classifications) so the common-case victim lookup is O(1). It may be
// incomplete (thread_scan.cpp also inserts background threads it does not track
// here), so when the set yields no live victim we fall back to the linear scan.
// Guarded by g_threadsMutex (the same lock callers already hold).
static std::unordered_set<int> s_backgroundTids;

// Erase any dead/non-background entries from s_backgroundTids and return an
// iterator to a live, evictable (non-protected, ROLE_S5_BACKGROUND) victim, or
// g_threads.end() if none. Caller must hold g_threadsMutex.
static std::unordered_map<int, ThreadInfo>::iterator findBgVictimLocked() {
    // Fast path: pick a live tid from the tracked set.
    for (auto it = s_backgroundTids.begin(); it != s_backgroundTids.end(); ) {
        auto gt = g_threads.find(*it);
        if (gt == g_threads.end() || gt->second.role != ROLE_S5_BACKGROUND || gt->second.protect) {
            it = s_backgroundTids.erase(it);  // stale entry
            continue;
        }
        return gt;
    }
    // Fallback: linear scan (covers threads inserted by thread_scan.cpp).
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
        // fix-v96 #2: honor maxTrackedThreads on the fork path exactly like the
        // /proc incremental scan does. Without this, fork() could insert unbounded
        // placeholders and bypass the cap that scanThreads() enforces.
        // P0-3: read the lock-free atomic cache instead of snapshotConfig()
        // (g_cfgMutex + deep Config copy per event was the dominant eBPF hot-path
        // lock contention). Config hot-reload updates this atomic in loadConfig().
        const int maxTracked = g_maxTrackedThreadsAtomic.load(std::memory_order_relaxed);

        // audit2: collect the evicted tid under g_threadsMutex but defer the
        // scoring/recovery teardown until AFTER the lock is released.
        // recoveryErase() ends in a disk ::unlink(recordPath); running it inside
        // the lock pinned the main loop's g_threadsMutex wait behind disk I/O on
        // every capacity-driven eviction. onExit() already releases the lock
        // before these two calls; mirror that here.
        int evictTid = -1;
        {
            std::lock_guard<std::mutex> lock(g_threadsMutex);
            auto it = g_threads.find(childTid);
            if (it == g_threads.end()) {
                // fix-v96 #2: capacity gate + eviction strategy. If the table is at
                // the cap, evict the first non-protected S5_BACKGROUND placeholder/thread
                // to make room. Protected (S0_SYSTEM) threads are never evicted. If no
                // evictable thread exists, skip the placeholder (the next /proc scan
                // will pick the thread up under the normal cap accounting).
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
                // fix-v96 #1 / A4: this CLONE_THREAD fork placeholder is NOT fully
                // populated (starttime==0, empty comm). Flag it so the next scanThreads()
                // forces a full populateNewThread() (real comm/role/schedstat/starttime)
                // instead of treating starttime==0 as a normal already-tracked thread.
                // INVARIANT (verified): thread_scan.cpp refreshTrackedThread() returns
                // REUSE whenever ti.needsHydration is set, and that REUSE path always
                // re-runs populateNewThread() before clearing the flag — so the
                // placeholder's starttime is guaranteed corrected on the very next scan,
                // never scored with the placeholder TID/starttime==0. scoring.cpp:903
                // also special-cases needsHydration threads. No code may read this
                // placeholder's starttime as authoritative between fork and next scan.
                ti.needsHydration = true;
                ti.starttime = 0;  // explicit: 0 is the sentinel that means "unpopulated"
                g_threads[childTid] = ti;
                s_backgroundTids.insert(childTid);
            }
        }
        // LED-160 (V-002): mirror the onExit teardown so evicted victims don't
        // leave scoring/recovery aux behind. Runs lock-free vs g_threadsMutex.
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
    // P1-4: clean up per-tid scoring maps (s_lastWrite, g_threadMigrationCount)
    cleanupThreadScoringState(tid);
    // P1-8: drop the recovery record so dead tid entries don't leak.
    aatpe_recovery::recoveryErase(tid);
    pendingExitCount_.fetch_add(1, std::memory_order_relaxed);
    LOG_DEBUG("eBPF EXIT: pid=%u tid=%u cpu=%u", ev.pid, ev.tid, ev.hdr.cpu);
}

void AatpeEventConsumer::onSwitch(const aatpe_ebpf::SwitchEvent& ev) {
    g_ebpfEventsSwitch.fetch_add(1, std::memory_order_relaxed);
    g_ebpfEventsTotal.fetch_add(1, std::memory_order_relaxed);

    // v95-CPU-FIX: sched_switch fires 1000-5000 times/sec even at idle. Each
    // event used to take g_threadsMutex + g_scoreCacheMutex (2 locks + 2 map
    // lookups), pinning a big core at 95%+. Throttle to 1-in-10 for the
    // currentCpu update + score-dirty marking. The main loop re-scores all
    // threads every 500ms-1s anyway, so 100-500Hz activity tracking is plenty.
    // E-006: single-threaded (onSwitch via dispatchQueuedEvents on the main loop),
    // so the throttle counter is a plain function-static rather than an atomic.
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

    // E-006: lastNextPid_ / switchCounter_ are plain fields. Confirmed single-
    // threaded: onSwitch() runs only on the daemon main loop via
    // dispatchQueuedEvents() (the poll thread only pushes to normalQueue_), so
    // there is no concurrent access. The delivery wiring is the SPSC queue +
    // bounded main-loop drain in EbpfScheduler::dispatchQueuedEvents().
    uint32_t nextPid = ev.nextPid;
    if (nextPid == 0 || nextPid == lastNextPid_) return;
    lastNextPid_ = nextPid;
    int ctr = switchCounter_++ + 1;
    if (ctr < 50) return;
    switchCounter_ = 0;
    // fix-v96 #5: sched_switch->nextPid is the kernel's next-to-run task, NOT the
    // Android foreground application. It can legitimately be system_server,
    // surfaceflinger, a Binder thread or any background service. Deriving
    // g_foregroundApp from it produced a bogus "foreground" that flickered with
    // kernel scheduling. sched_switch is now ONLY a thread-activity / scheduling-
    // pressure signal (markThreadScoreDirty above). The true foreground app is
    // updated exclusively by the APK/UsageStats/dumpsys foreground detector in
    // frame.cpp. We keep a throttled informational log but never mutate
    // g_foregroundApp here.
    std::string pkg = getPackageByPid((int)nextPid);
    if (!pkg.empty() && pkg != "unknown") {
        LOG_DEBUG("eBPF SWITCH: next-to-run pid=%u (%s) [activity only; not foreground]",
                  nextPid, pkg.c_str());
    }
}

void AatpeEventConsumer::onExec(const aatpe_ebpf::ExecEvent& ev) {
    g_ebpfEventsTotal.fetch_add(1, std::memory_order_relaxed);
    pendingExecCount_.fetch_add(1, std::memory_order_relaxed);
    // v95-CPU-FIX2: exec events can burst during game startup (hundreds/sec).
    // Each onExec opens /proc/<pid>/cmdline + schedstat via classifyThread.
    // Throttle to 1-in-3: thread classification is eventually consistent via
    // the periodic scanThreads() anyway, so dropping 2/3 of exec events is
    // safe and cuts file I/O by 66%.
    static std::atomic<uint32_t> s_execProcessCounter{0};
    uint32_t execCount = s_execProcessCounter.fetch_add(1, std::memory_order_relaxed);
    if ((execCount % 3) != 0) return;
    const int tid = (int)ev.tid;
    if (tid > 0) {
        std::string comm(ev.comm, strnlen(ev.comm, sizeof(ev.comm)));
        std::string pname = getPackageByPid((int)ev.pid);
        // 问题1 fix: onExec() must honor maxTrackedThreads exactly like onFork().
        // Without this, exec() on a brand-new TID could insert unbounded entries
        // and bypass the cap that scanThreads() enforces on the /proc path.
        // P0-3: lock-free atomic read (see onFork) — no g_cfgMutex, no deep copy.
        const int maxTracked = g_maxTrackedThreadsAtomic.load(std::memory_order_relaxed);

        // P1-3 fix: classifyThread() opens /proc/<pid>/cmdline + schedstat and runs
        // the rule matcher. The old code did this while holding g_threadsMutex, so
        // every exec event pinned the main loop's g_threadsMutex wait behind disk
        // I/O. Move classification OUTSIDE the lock: it is eventually-consistent
        // anyway (the periodic scanThreads() re-runs it), and we re-check the map
        // state under the lock below before committing.
        int prio = aatpe::thread_classify::kDefaultPriority;
        std::string subtype;
        ThreadRole role = classifyThread((int)ev.pid, tid, comm, pname, prio, subtype);

        // audit2: defer scoring/recovery teardown (incl. recoveryErase's disk
        // ::unlink) out of g_threadsMutex; see the onFork eviction comment.
        int evictTid = -1;
        {
            std::lock_guard<std::mutex> lock(g_threadsMutex);
            auto it = g_threads.find(tid);
            if (it == g_threads.end()) {
                // 问题1 fix: capacity gate + eviction strategy (mirrors onFork).
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
                // 问题2 fix: placeholder created from exec() is NOT fully populated.
                // Flag it so the next scanThreads() forces a full populateNewThread()
                // (real comm/role/schedstat/starttime) instead of treating
                // starttime==0 as a normal already-tracked thread. Mirrors onFork().
                ti.needsHydration = true;
                // 问题3 fix: protect state must be derived from role exactly like
                // thread_scan.cpp:populateNewThread() does. Without this, a brand-new
                // system thread arriving via exec() would have role=ROLE_S0_SYSTEM but
                // protect=false, which decidePlacement() treats as migratable.
                ti.protect = (ti.role == ROLE_S0_SYSTEM);
                g_threads[tid] = ti;
                if (ti.role == ROLE_S5_BACKGROUND) s_backgroundTids.insert(tid);
            } else {
                it->second.tname = comm;
                it->second.role = role;
                it->second.priority = prio;
                it->second.subtype = subtype;
                // 问题3 fix: when an existing thread's role changes on exec(), its
                // protect flag must be re-derived from the new role so we never
                // carry a stale protect=false for a system thread.
                it->second.protect = (it->second.role == ROLE_S0_SYSTEM);
                // P2: keep the background set in sync on role transitions.
                if (it->second.role == ROLE_S5_BACKGROUND) s_backgroundTids.insert(tid);
                else s_backgroundTids.erase(tid);
            }
        }
        if (evictTid > 0) {
            // LED-160 (V-002): mirror onExit teardown (see onFork eviction).
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
    // v95-CPU-FIX: cache pid->package mapping for 2s to avoid repeated /proc
    // file opens on high-frequency exec/switch events.
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
        // cap cache size. P2: the eviction is O(n) linear scan for the
        // oldest timestamp, but n is bounded to 512 entries and the cache is
        // hit on every exec/switch (already holding s_pkgCacheMutex), so the
        // 512-element scan amortizes to ~1us and only runs once per overflow.
        // A true LRU would need a linked list + hash map; not worth the
        // complexity here given the tiny cap.
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

// P1-1: runqueue (wakeup->run) latency sample. Stat-only: we keep a process-
// wide worst-case in an atomic and throttled-log extreme stalls (>5ms). The
// EbpfScheduler already tracks runqMaxDelayNs_; this consumer hook exists so
// the decision loop / WebUI can later react to repeated long stalls.
void AatpeEventConsumer::onRunqLatency(const aatpe_ebpf::RunqEvent& ev) {
    static std::atomic<uint64_t> s_runqWorstNs{0};
    static std::atomic<uint32_t> s_runqLogCounter{0};
    // G4-012: the historical worst value never reset, so it stale-inflated
    // "recent" latency. Re-seed the worst every ~64k samples so it reflects the
    // current window rather than a one-time historical spike.
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
    // Throttle: log at most 1 extreme-stall line per ~64 samples.
    uint32_t n = s_runqLogCounter.fetch_add(1, std::memory_order_relaxed);
    if (d > 5000000ULL && (n & 63U) == 0) {  // > 5ms stall
        LOG_WARN("[P1-1] runq stall tid=%u cpu=%u delay=%.3fms",
                 ev.pid, ev.targetCpu, d / 1.0e6);
    }
}

AatpeEventConsumer g_eventConsumer;
