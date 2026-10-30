#include "aatped.h"
#include "aatpe_state_ttl.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cmath>
#include <climits>

bool stateTtlIsExpired(time_t mtime, time_t nowSec, int ttlHours) {
    if (ttlHours <= 0) return false;
    time_t ttlSec = (time_t)ttlHours * 3600;
    return (nowSec - mtime) > ttlSec;
}

static std::atomic<long long> g_ttlSweeps{0};
static std::atomic<long long> g_ttlHistoryRemoved{0};
static std::atomic<long long> g_ttlSettleRemoved{0};
static std::atomic<long long> g_ttlScoreRemoved{0};
static std::atomic<long long> g_ttlFilesRemoved{0};

static std::atomic<long long> g_placementGainCount{0};
static std::atomic<long long> g_placementConfCount{0};
static std::atomic<long long> g_placementGainSumX100{0};
static std::atomic<long long> g_placementConfSum{0};

void placementAvgRecordGain(float gainPercent, int confidence) {
    if (!std::isfinite(gainPercent)) return;
    if (gainPercent > 1000.0f) gainPercent = 1000.0f;
    if (gainPercent < 0.0f) gainPercent = 0.0f;
    if (gainPercent > 0) {
        g_placementGainCount.fetch_add(1, std::memory_order_relaxed);
        g_placementGainSumX100.fetch_add((long long)(gainPercent * 100.0f), std::memory_order_relaxed);
    }
    if (confidence > 0) {
        g_placementConfCount.fetch_add(1, std::memory_order_relaxed);
        g_placementConfSum.fetch_add((long long)confidence, std::memory_order_relaxed);
    }
}

void writePlacementAvgStats() {
    long long gc = g_placementGainCount.load(std::memory_order_relaxed);
    long long cc = g_placementConfCount.load(std::memory_order_relaxed);
    long long avgGainX100 = gc > 0 ? g_placementGainSumX100.load() / gc : 0;
    long long avgGain = avgGainX100 / 100;
    if (avgGain < 0) avgGain = 0;
    if (avgGain > 100) avgGain = 100;
    long long avgConf = cc > 0 ? g_placementConfSum.load() / cc : 0;
    if (avgConf < 0) avgConf = 0;
    if (avgConf > 100) avgConf = 100;
    atomicWriteFile((std::string(STATE_DIR) + "/placement_avg_gain").c_str(), avgGain);
    atomicWriteFile((std::string(STATE_DIR) + "/placement_avg_confidence").c_str(), avgConf);
}

void stateTtlSweep(time_t nowSec, int ttlHours) {
    g_ttlSweeps.fetch_add(1, std::memory_order_relaxed);
    const bool ttlEnabled = (ttlHours > 0);
    time_t ttlSec = ttlEnabled ? (time_t)ttlHours * 3600 : 0;

    if (ttlEnabled) {
        std::lock_guard<std::mutex> lk(g_historyMutex);
        auto it = g_history.begin();
        while (it != g_history.end()) {
            if (it->second.lastUpdate > 0 &&
                (nowSec - it->second.lastUpdate) > ttlSec) {
                it = g_history.erase(it);
                g_ttlHistoryRemoved.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++it;
            }
        }
    }

    {
        std::lock_guard<std::mutex> tlk(g_threadsMutex);
        std::lock_guard<std::mutex> lk(g_settleMutex);
        auto it = g_threadSettle.begin();
        while (it != g_threadSettle.end()) {
            if (g_threads.find(it->first) == g_threads.end()) {
                it = g_threadSettle.erase(it);
                g_ttlSettleRemoved.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++it;
            }
        }
    }

    if (ttlEnabled) {
        long long nowSteadyMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(g_scoreCacheMutex);
        auto it = g_scoreCache.begin();
        while (it != g_scoreCache.end()) {
            long long inactiveMs = nowSteadyMs - it->second.lastSwitchMs;
            if (it->second.lastSwitchMs != 0 && inactiveMs > (long long)ttlSec * 1000) {
                it = g_scoreCache.erase(it);
                g_ttlScoreRemoved.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++it;
            }
        }
    }

    DIR* dir = opendir(STATE_DIR);
    if (dir) {
        int dirFd = ::dirfd(dir);
        struct dirent* de;
        while ((de = readdir(dir)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            std::string name = de->d_name;
            bool staleCandidate =
                (name.rfind("thread_", 0) == 0) ||
                (name.rfind("decisions_", 0) == 0) ||
                (name == "ml_pending" || name == "ml_history");
            if (!staleCandidate) continue;
            int fd = ::openat(dirFd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) continue;
            struct stat st;
            bool expired = false;
            if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
                expired = stateTtlIsExpired(st.st_mtime, nowSec, ttlHours);
            }
            ::close(fd);
            if (expired) {
                std::string path = std::string(STATE_DIR) + "/" + name;
                if (::unlinkat(dirFd, name.c_str(), 0) == 0) {
                    stateCacheNoteRemove(path.c_str());
                    g_ttlFilesRemoved.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        closedir(dir);
    }

    atomicWriteFile((std::string(STATE_DIR) + "/state_ttl_sweeps").c_str(),
                    (long long)g_ttlSweeps.load());
    atomicWriteFile((std::string(STATE_DIR) + "/state_ttl_removed").c_str(),
                    (long long)(g_ttlHistoryRemoved.load() + g_ttlSettleRemoved.load() +
                                g_ttlScoreRemoved.load() + g_ttlFilesRemoved.load()));
}
