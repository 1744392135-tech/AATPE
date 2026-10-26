// AATPE - State/cache TTL cleanup (#77)
#include "aatped.h"
#include "aatpe_state_ttl.h"
#include <dirent.h>
#include <fcntl.h>      // F06: openat/unlinkat/O_NOFOLLOW
#include <sys/stat.h>
#include <cmath>
#include <climits>

bool stateTtlIsExpired(time_t mtime, time_t nowSec, int ttlHours) {
    if (ttlHours <= 0) return false;
    time_t ttlSec = (time_t)ttlHours * 3600;
    return (nowSec - mtime) > ttlSec;
}

// ---------------------------------------------------------------------------
// Accumulators for #35 three-value placement output
// ---------------------------------------------------------------------------
static std::atomic<long long> g_ttlSweeps{0};
static std::atomic<long long> g_ttlHistoryRemoved{0};
static std::atomic<long long> g_ttlSettleRemoved{0};
static std::atomic<long long> g_ttlScoreRemoved{0};
static std::atomic<long long> g_ttlFilesRemoved{0};

// #35 accumulators — atomic<double> is not lock-free on all platforms; use
// atomic<long long> with fixed-point (x100) scaling for thread-safe accumulation.
static std::atomic<long long> g_placementGainCount{0};
static std::atomic<long long> g_placementConfCount{0};
static std::atomic<long long> g_placementGainSumX100{0};  // gain% * 100
static std::atomic<long long> g_placementConfSum{0};     // confidence 0-100

void placementAvgRecordGain(float gainPercent, int confidence) {
    // P1-8: Inf/NaN gainPercent cast to long long is UB (wraps to LLONG_MIN).
    // Reject non-finite and clamp to a sane range.
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
    // avgGainX100 = avg(gainPercent * 100). To expose the percentage (0..100)
    // we must divide by 100 as well. Previously this final /100 was missing, so
    // the state file wrote avg(gain%*100) (e.g. 12907 for a 129% gain) instead of
    // the human-readable percentage. Clamp to [0,100] as documented.
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

// ---------------------------------------------------------------------------
// Main TTL sweep
// ---------------------------------------------------------------------------
void stateTtlSweep(time_t nowSec, int ttlHours) {
    g_ttlSweeps.fetch_add(1, std::memory_order_relaxed);
    // D-002: UNIFIED ttlHours<=0 semantics. stateTtlIsExpired() already documents
    // "ttlHours<=0 = never expire" (returns false). Previously the history/score
    // blocks below silently fell back to a hardcoded 86400s (24h) window for
    // ttlHours<=0, while the state-file block used stateTtlIsExpired() and thus
    // never deleted thread_*.json / decisions_* — so those files accumulated
    // forever. The contract is now explicit and uniform:
    //
    //   ttlHours >  0 : age-based sweeps use ttlHours*3600s everywhere.
    //   ttlHours <= 0 : age-based sweeps are DISABLED (history / score cache /
    //                   state files are all retained). This matches the
    //                   stateTtlIsExpired() "no expiration" contract.
    //
    // Block 2 below (settle entries for threads no longer tracked) is LIVENESS
    // garbage-collection, not age-based TTL: it always runs regardless of
    // ttlHours, because a dead thread's settle entry has nothing to do with the
    // configured retention window.
    const bool ttlEnabled = (ttlHours > 0);
    time_t ttlSec = ttlEnabled ? (time_t)ttlHours * 3600 : 0;

    // 1. Clean placement history entries older than TTL (skipped when TTL disabled)
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

    // 2. Clean settled-placement entries for threads no longer tracked
    {
        // P1-9: global lock order is g_threadsMutex -> g_settleMutex.
        // Previously this block took g_settleMutex first, then g_threadsMutex,
        // which can deadlock with another path that takes them in the opposite
        // order. Reverse the acquisition order.
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

    // 3. Clean score cache entries for inactive threads (skipped when TTL disabled)
    // LED-097 (R7-012, P1): g_scoreCache[].lastSwitchMs is recorded in
    // steady_clock milliseconds (see markThreadScoreDirty / the main loop's
    // nowDecisionMs). Previously this compared wall-clock nowSec*1000 (epoch
    // seconds, ~1.7e9) against that steady ms (~millions since boot), yielding
    // a ~1.7e12 "inactiveMs" that always exceeded the TTL — so EVERY entry was
    // erased on every sweep, defeating the 500ms hot-path cache entirely.
    // Measure inactivity entirely on the monotonic steady_clock basis (also
    // immune to wall-clock/NTP jumps). ttlSec here is only a duration window.
    if (ttlEnabled) {
        long long nowSteadyMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(g_scoreCacheMutex);
        auto it = g_scoreCache.begin();
        while (it != g_scoreCache.end()) {
            // Remove if no sched_switch in last TTL window
            // P2: cast to long long before multiply to avoid 32-bit overflow on
            // platforms where time_t is 32-bit.
            long long inactiveMs = nowSteadyMs - it->second.lastSwitchMs;
            if (it->second.lastSwitchMs != 0 && inactiveMs > (long long)ttlSec * 1000) {
                it = g_scoreCache.erase(it);
                g_ttlScoreRemoved.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++it;
            }
        }
    }

    // 4. Clean stale state files (thread_*.json, decision files older than TTL)
    // F06[P3]: eliminate the stat()→unlink() TOCTOU. STATE_DIR is root-only (0700),
    // so an unprivileged attacker cannot race it, but we now pin the inode by
    // openat-ing each candidate with O_NOFOLLOW (refuses a symlink swap), fstat the
    // OPENED fd for the mtime, and unlinkat by name relative to the open dirfd —
    // there is no longer a check-then-act window between a separate stat() and
    // unlink() on a re-resolved path.
    DIR* dir = opendir(STATE_DIR);
    if (dir) {
        int dirFd = ::dirfd(dir);
        struct dirent* de;
        while ((de = readdir(dir)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            std::string name = de->d_name;
            // Only clean thread-specific or per-decision files, not core state.
            // STATE-11: ml_pending/ml_history are written by the ML module on
            // every decision cycle, so a file older than ttlHours is genuinely
            // abandoned (e.g. daemon restart left stale state). No explicit
            // handshake with ML is needed — mtime freshness is the handshake.
            bool staleCandidate =
                (name.rfind("thread_", 0) == 0) ||
                (name.rfind("decisions_", 0) == 0) ||
                (name == "ml_pending" || name == "ml_history");
            if (!staleCandidate) continue;
            int fd = ::openat(dirFd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) continue;  // vanished, or a symlink (refused) -> skip
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

    // Write TTL diagnostics
    atomicWriteFile((std::string(STATE_DIR) + "/state_ttl_sweeps").c_str(),
                    (long long)g_ttlSweeps.load());
    atomicWriteFile((std::string(STATE_DIR) + "/state_ttl_removed").c_str(),
                    (long long)(g_ttlHistoryRemoved.load() + g_ttlSettleRemoved.load() +
                                g_ttlScoreRemoved.load() + g_ttlFilesRemoved.load()));
}
