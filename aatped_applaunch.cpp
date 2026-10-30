#include "aatped.h"
#include <mutex>
#include <list>

using namespace aatpe;

std::mutex g_appLaunchHistoryMutex;
std::unordered_map<std::string, AppLaunchRecord> g_appLaunchHistory;
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

void recordAppLaunch(const char* packageName, int mainTid, int renderTid) {
    std::lock_guard<std::mutex> lk(g_appLaunchHistoryMutex);
    AppLaunchRecord rec;
    rec.packageName = packageName;
    rec.mainThreadTid = mainTid;
    rec.renderThreadTid = renderTid;
    rec.lastLaunchTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::vector<int> candidateCpus;
    {
        std::lock_guard<std::mutex> topoLk(g_topologyRwMutex);
        candidateCpus.insert(candidateCpus.end(), g_midPool.begin(), g_midPool.end());
        candidateCpus.insert(candidateCpus.end(), g_bigPool.begin(), g_bigPool.end());
    }
    int bestCpu = candidateCpus.empty() ? 0 : candidateCpus[0];
    float bestLoad = 100.0f;
    for (int c : candidateCpus) {
        float load = readCpuUtilizationSchedstat(c);
        if (load >= 0 && load < bestLoad) { bestLoad = load; bestCpu = c; }
    }

    auto it = g_appLaunchHistory.find(packageName);
    if (it != g_appLaunchHistory.end()) {
        if (it->second.preferredCpu >= 0) {
            bestCpu = it->second.preferredCpu;
        }
    }
    rec.preferredCpu = bestCpu;

    lruEvictIfFull();
    g_appLaunchHistory[packageName] = rec;
    lruPromote(packageName);
    if (!writeFile((std::string(STATE_DIR) + "/preload_count").c_str(),
                    (int)g_appLaunchHistory.size())) {
        LOG_WARN("applaunch: failed to write preload_count state");
    }
}

int getPreloadedPreferredCpu(const char* packageName) {
    if (!packageName) return -1;
    std::lock_guard<std::mutex> lk(g_appLaunchHistoryMutex);
    auto it = g_appLaunchHistory.find(packageName);
    if (it != g_appLaunchHistory.end()) return it->second.preferredCpu;
    return -1;
}

void checkAndPreloadNewApps(void) {
    return;
}
