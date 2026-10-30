#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include "aatped.h"
#include "aatped_ebpf_agg.h"
#include "aatpe_ebpf.h"

using namespace aatpe_ebpf;

static long long nowMsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void seedCache(const std::string& pkg) {
    std::lock_guard<std::mutex> lk(g_foregroundMutex);
    g_cachedForegroundApp = pkg;
    g_foregroundCacheTimeMs = nowMsNow();
}
static void clearCache() {
    std::lock_guard<std::mutex> lk(g_foregroundMutex);
    g_cachedForegroundApp.clear();
    g_foregroundCacheTimeMs = 0;
}
static std::atomic<bool> s_holderReady{false};
static void holderHoldsLock(long long holdMs) {
    std::lock_guard<std::mutex> lk(g_foregroundMutex);
    s_holderReady.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
}
static std::string callTimed(long long& elapsedMsOut) {
    auto t0 = std::chrono::steady_clock::now();
    std::string v = getCachedForegroundApp();
    auto t1 = std::chrono::steady_clock::now();
    elapsedMsOut = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    return v;
}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static ForkEvent makeFork(uint32_t parent, uint32_t childPid, uint32_t childTid, uint32_t cpu) {
    ForkEvent e; e.hdr.cpu = cpu; e.parentPid = parent; e.childPid = childPid; e.childTid = childTid; return e;
}
static ExitEvent makeExit(uint32_t pid, uint32_t tid, uint32_t cpu) {
    ExitEvent e; e.hdr.cpu = cpu; e.pid = pid; e.tid = tid; return e;
}
static SwitchEvent makeSwitch(uint32_t next, uint32_t cpu) {
    SwitchEvent e; e.hdr.cpu = cpu; e.prevPid = 1; e.nextPid = next; return e;
}

int main() {
    printf("=== ebpf_agg: getCachedForegroundApp (host -> unknown, empty cache) ===\n");
    std::string fg = getCachedForegroundApp();
    CHECK(fg == "unknown" || fg.empty(), "host detectForegroundApp -> unknown/empty");

    printf("=== ebpf_agg: onFork registers a new thread ===\n");
    g_eventConsumer.clearPending();
    uint64_t forksBefore = g_ebpfEventsFork.load();
    g_eventConsumer.onFork(makeFork(1000, 2000, 2001, 3));
    CHECK(g_ebpfEventsFork.load() == forksBefore + 1, "fork counter incremented");
    CHECK(g_eventConsumer.getPendingForks() == 1, "pending forks == 1");
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        auto it = g_threads.find(2001);
        CHECK(it != g_threads.end() && it->second.currentCpu == 3,
              "fork inserted thread with cpu=3");
    }

    printf("=== ebpf_agg: onSwitch updates current cpu ===\n");
    g_eventConsumer.onSwitch(makeSwitch(2001, 5));
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        CHECK(g_threads[2001].currentCpu == 5, "switch updated thread cpu to 5");
    }

    printf("=== ebpf_agg: onExec sets comm ===\n");
    ExecEvent ex; ex.hdr.cpu = 2; ex.pid = 2000; ex.tid = 2002;
    snprintf(ex.comm, sizeof(ex.comm), "renderthread");
    g_eventConsumer.onExec(ex);
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        CHECK(g_threads.count(2002) == 1, "exec inserted new thread");
    }

    printf("=== ebpf_agg: onExit removes the thread ===\n");
    g_eventConsumer.onExit(makeExit(2000, 2001, 3));
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        CHECK(g_threads.count(2001) == 0, "exit erased the thread");
    }

    printf("=== ebpf_agg: hasPendingEvents / clearPending ===\n");
    CHECK(g_eventConsumer.hasPendingEvents() == true, "pending events present after fork/exit/exec");
    g_eventConsumer.clearPending();
    CHECK(g_eventConsumer.getPendingForks() == 0, "clearPending zeroes fork counter");
    CHECK(g_eventConsumer.hasPendingEvents() == false, "hasPendingEvents false after clear");

    printf("=== ebpf_agg: duplicate child fork does not duplicate entry ===\n");
    g_eventConsumer.onFork(makeFork(1000, 3000, 3001, 1));
    g_eventConsumer.onFork(makeFork(1000, 3000, 3001, 1));
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        CHECK(g_threads.count(3001) == 1, "duplicate fork does not create 2 entries");
    }

    printf("=== N4-1: fresh cache + lock free -> returns cached value ===\n");
    seedCache("com.example.game");
    {
        long long dt = 0;
        std::string fg = callTimed(dt);
        CHECK(fg == "com.example.game", "lock-free fresh cache returns the cached package");
        CHECK(dt < 200, "lock-free fast path is instantaneous (<200ms)");
    }

    printf("=== N4-2: lock held by another thread, cache non-empty -> returns cache (no block) ===\n");
    seedCache("com.example.game");
    {
        s_holderReady.store(false, std::memory_order_release);
        std::thread h(holderHoldsLock, 1000LL);
        while (!s_holderReady.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        long long dt = 0;
        std::string fg = callTimed(dt);
        CHECK(fg == "com.example.game", "lock held + cache non-empty -> returns cached pkg (NOT slow-path unknown)");
        CHECK(dt < 200, "lock held -> returns in <200ms (never waits out the 1000ms holder / dumpsys)");
        h.join();
    }

    printf("=== N4-3: lock held by another thread, cache empty -> returns \"unknown\" (no block) ===\n");
    clearCache();
    {
        s_holderReady.store(false, std::memory_order_release);
        std::thread h(holderHoldsLock, 1000LL);
        while (!s_holderReady.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        long long dt = 0;
        std::string fg = callTimed(dt);
        CHECK(fg == "unknown", "lock held + empty cache -> returns \"unknown\"");
        CHECK(dt < 200, "lock held + empty cache -> returns in <200ms (never blocks)");
        h.join();
    }
    clearCache();

    printf("\n========================================\n"
           "ebpf_agg results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
