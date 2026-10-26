// AATPE - App launch preloading (extracted from aatped_main.cpp)
#include "aatped.h"
#include <mutex>
#include <list>

using namespace aatpe;

// Predictive preloading implementation
// P2-9: g_appLaunchHistory is read by getPreloadedPreferredCpu() (WebUI thread)
// and written by recordAppLaunch() (main loop). Guard with a mutex.
std::mutex g_appLaunchHistoryMutex;
std::unordered_map<std::string, AppLaunchRecord> g_appLaunchHistory;
// ROUND-014 P3 (S14-05): true LRU recency list. Front = most recently used; evict from
// the back. The old erase(begin()) on an unordered_map removed an arbitrary bucket entry,
// not the least-recently-used package.
static std::list<std::string> g_appLaunchRecency;

static void lruPromote(const std::string& pkg) {
    g_appLaunchRecency.remove(pkg);
    g_appLaunchRecency.push_front(pkg);
}
static void lruEvictIfFull() {
    while ((int)g_appLaunchHistory.size() >= 256 && !g_appLaunchRecency.empty()) {
        std::string old = g_appLaunchRecency.back();
        g_appLaunchRecency.pop_back();
        g_appLaunchHistory.erase(old);
    }
}

// audit found no production callers (only the legacy AB test
// stub references it). Kept — may have external/module-instrumentation callers.
void recordAppLaunch(const char* packageName, int mainTid, int renderTid) {
    std::lock_guard<std::mutex> lk(g_appLaunchHistoryMutex);
    // P9: Dynamic preferredCpu based on topology and historical performance
    AppLaunchRecord rec;
    rec.packageName = packageName;
    rec.mainThreadTid = mainTid;
    rec.renderThreadTid = renderTid;
    rec.lastLaunchTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Choose least-loaded mid core as default (not fixed CPU4)
    // LED-091 (R7-006): g_midPool/g_bigPool are mutated under g_topologyRwMutex;
    // snapshot them under that lock instead of copying iterators unlocked.
    std::vector<int> candidateCpus;
    {
        std::lock_guard<std::mutex> topoLk(g_topologyRwMutex);
        candidateCpus.insert(candidateCpus.end(), g_midPool.begin(), g_midPool.end());
        candidateCpus.insert(candidateCpus.end(), g_bigPool.begin(), g_bigPool.end());
    }
    // Fallback to CPU 0 (always present) rather than hardcoded CPU 4, which
    // does not exist on 4-core devices.
    int bestCpu = candidateCpus.empty() ? 0 : candidateCpus[0];
    float bestLoad = 100.0f;
    for (int c : candidateCpus) {
        float load = readCpuUtilizationSchedstat(c);
        if (load >= 0 && load < bestLoad) { bestLoad = load; bestCpu = c; }
    }

    // Check historical performance for this app
    auto it = g_appLaunchHistory.find(packageName);
    if (it != g_appLaunchHistory.end()) {
        // Use historically best CPU if we have data
        if (it->second.preferredCpu >= 0) {
            bestCpu = it->second.preferredCpu;
        }
    }
    rec.preferredCpu = bestCpu;

    // ROUND-014 P3 (S14-05): true LRU — evict least-recently-used, then promote pkg.
    lruEvictIfFull();
    g_appLaunchHistory[packageName] = rec;
    lruPromote(packageName);
    // P3-38: check writeFile return value.
    if (!writeFile((std::string(STATE_DIR) + "/preload_count").c_str(),
                    (int)g_appLaunchHistory.size())) {
        LOG_WARN("applaunch: failed to write preload_count state");
    }
}

// no production callers (only the legacy AB test stub).
int getPreloadedPreferredCpu(const char* packageName) {
    if (!packageName) return -1;
    std::lock_guard<std::mutex> lk(g_appLaunchHistoryMutex);
    auto it = g_appLaunchHistory.find(packageName);
    if (it != g_appLaunchHistory.end()) return it->second.preferredCpu;
    return -1;
}

void checkAndPreloadNewApps(void) {
    // S-005: DEAD/UNSAFE CODE REMOVED. The previous body performed:
    //   (1) package-name SUBSTRING matching via strstr(cmdline, pair.first) over
    //       /proc/<pid>/cmdline — any process whose cmdline merely CONTAINED the
    //       recorded package string matched (false positives on unrelated apps);
    //   (2) whole-process affinity pinning via sched_setaffinity(pid, ...) to a
    //       single CPU — pinning an entire unrelated process to one core causes
    //       severe jank and is wrong (only specific render/main threads should be
    //       placed, never the whole process).
    // A production audit found no caller relying on this side effect (the only
    // references are the legacy AB-test stub and the aux-scan tick in
    // aatped_main.cpp). The dangerous body is therefore deleted; the function is
    // retained as a documented no-op so the daemon call site and the v94 hermetic
    // test keep linking. recordAppLaunch()/getPreloadedPreferredCpu() remain
    // safe history bookkeeping consumed by state.json's preload_count.
    return;
}
