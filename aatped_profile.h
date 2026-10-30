#ifndef AATPED_PROFILE_H
#define AATPED_PROFILE_H

#include <string>
#include <mutex>
#include <atomic>

void logMsg(int level, const char* fmt, ...);

void loadProfile(const std::string& profile);

namespace aatpe_profile {

enum class ProfileSource : int {
    DEFAULT   = 0,
    GLOBAL    = 1,
    AUTO      = 2,
    THERMAL   = 3,
    APP       = 4,
    MANUAL    = 5,
    EMERGENCY = 6,
    COUNT     = 7
};

inline std::mutex g_pmMutex;
inline std::string g_pmDesired[(int)ProfileSource::COUNT];
inline std::string g_pmApplied;
inline ProfileSource g_pmAppliedSource = ProfileSource::DEFAULT;
inline std::atomic<uint64_t> g_pmGeneration{0};

inline void bumpGenerationLocked() {
    g_pmGeneration.fetch_add(1, std::memory_order_release);
}

inline const char* sourceName(ProfileSource s) {
    switch (s) {
        case ProfileSource::DEFAULT:   return "DEFAULT";
        case ProfileSource::GLOBAL:    return "GLOBAL";
        case ProfileSource::AUTO:      return "AUTO";
        case ProfileSource::THERMAL:   return "THERMAL";
        case ProfileSource::APP:       return "APP";
        case ProfileSource::MANUAL:    return "MANUAL";
        case ProfileSource::EMERGENCY: return "EMERGENCY";
        default: return "?";
    }
}

inline bool isValidProfileName(const std::string& name) {
    return name == "balanced" || name == "gaming" ||
           name == "battery"  || name == "thermal";
}

inline ProfileSource pickWinnerLocked(std::string& outName) {
    for (int i = (int)ProfileSource::COUNT - 1; i >= 0; --i) {
        if (!g_pmDesired[i].empty()) {
            outName = g_pmDesired[i];
            return static_cast<ProfileSource>(i);
        }
    }
    outName = "balanced";
    return ProfileSource::DEFAULT;
}

inline void arbitrateAndApplyLocked(std::unique_lock<std::mutex>& lk) {
    for (;;) {
        std::string winner;
        ProfileSource winnerSrc = pickWinnerLocked(winner);
        if (winner == g_pmApplied) {
            if (winnerSrc != g_pmAppliedSource) {
                g_pmAppliedSource = winnerSrc;
            }
            return;
        }
        const uint64_t gen = g_pmGeneration.load(std::memory_order_acquire);
        lk.unlock();
        try {
            loadProfile(winner);
        } catch (...) {
            lk.lock();
            throw;
        }
        lk.lock();
        if (g_pmGeneration.load(std::memory_order_acquire) != gen) {
            continue;
        }
        g_pmApplied = winner;
        g_pmAppliedSource = winnerSrc;
        return;
    }
}

inline void profileRequest(const std::string& profile, ProfileSource src) {
    if (profile.empty()) return;
    if (!isValidProfileName(profile)) return;
    std::unique_lock<std::mutex> lk(g_pmMutex);
    if (g_pmDesired[(int)src] == profile) return;
    g_pmDesired[(int)src] = profile;
    bumpGenerationLocked();
    arbitrateAndApplyLocked(lk);
}

inline void profileRelease(ProfileSource src) {
    std::unique_lock<std::mutex> lk(g_pmMutex);
    if (g_pmDesired[(int)src].empty()) return;
    g_pmDesired[(int)src].clear();
    bumpGenerationLocked();
    arbitrateAndApplyLocked(lk);
}

inline void profileInitGlobal(const std::string& base, const std::string& actuallyApplied) {
    std::lock_guard<std::mutex> lk(g_pmMutex);
    g_pmDesired[(int)ProfileSource::GLOBAL] = base.empty() ? "balanced" : base;
    g_pmApplied = isValidProfileName(actuallyApplied) ? actuallyApplied : "balanced";
    g_pmAppliedSource = ProfileSource::GLOBAL;
}

inline void profileReapplyWinner() {
    std::unique_lock<std::mutex> lk(g_pmMutex);
    for (;;) {
        std::string winner;
        ProfileSource winnerSrc = pickWinnerLocked(winner);
        const uint64_t gen = g_pmGeneration.load(std::memory_order_acquire);
        lk.unlock();
        try {
            loadProfile(winner);
        } catch (const std::exception& e) {
            logMsg(1, "profileReapplyWinner: loadProfile('%s') failed (%s); keeping previous tunables",
                     winner.c_str(), e.what());
            lk.lock();
            return;
        } catch (...) {
            logMsg(1, "profileReapplyWinner: loadProfile('%s') threw unknown; keeping previous tunables",
                     winner.c_str());
            lk.lock();
            return;
        }
        lk.lock();
        if (g_pmGeneration.load(std::memory_order_acquire) != gen) continue;
        g_pmApplied = winner;
        g_pmAppliedSource = winnerSrc;
        return;
    }
}

inline std::string effectiveProfile() {
    std::lock_guard<std::mutex> lk(g_pmMutex);
    return g_pmApplied;
}
inline ProfileSource effectiveSource() {
    std::lock_guard<std::mutex> lk(g_pmMutex);
    return g_pmAppliedSource;
}

inline bool hasManualVote() {
    std::lock_guard<std::mutex> lk(g_pmMutex);
    return !g_pmDesired[(int)ProfileSource::MANUAL].empty();
}

inline uint64_t generation() {
    return g_pmGeneration.load(std::memory_order_acquire);
}

}

#endif
