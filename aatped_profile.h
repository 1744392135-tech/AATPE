// AATPE - Unified Profile Source State Machine
// ============================================================================
// Problem being solved:
//   Previously g_currentProfile was written directly by FIVE independent code
//   paths (per-app override, auto-scene select, thermal warning switch, manual
//   set_profile, thermal-emergency recovery). They ran in a fixed call order and
//   clobbered each other:
//     * leaving a per-app app left its profile "sticky" (auto was disabled);
//     * per-app gaming was overwritten by auto-select later in the same cycle;
//     * thermal recovery restored a *saved historical string* instead of
//       re-evaluating the live foreground app / manual lock / auto layer.
//
// Design: every source now VOTES for the profile it wants via profileRequest()
// and abstains via profileRelease(). A single priority arbiter picks the winning
// profile and the low-level loadProfile() is called ONLY when the winner actually
// changes. The scheduler / WebUI still read g_currentProfile (unchanged).
//
// Priority (highest wins, encoded by enum magnitude):
//   1. EMERGENCY   thermal emergency latch (full protection)
//   2. MANUAL       user explicit set_profile lock
//   3. APP          per-app [app:pkg] profile override
//   4. THERMAL      thermal warning auto-switch
//   5. AUTO         auto scene detection (game/battery/balanced)
//   6. GLOBAL       global base profile (boot / CURRENT_PROFILE)
//   (DEFAULT = "balanced" fallback when nobody votes)
//
// GLOBAL -> APP_OVERRIDE -> APP_EXIT -> RESTORE_GLOBAL is now explicit:
//   entering App A with profile=gaming  => profileRequest(gaming, APP)
//   leaving App A (no match)            => profileRelease(APP)
//                                         arbiter re-picks MANUAL/AUTO/GLOBAL
//
// Header-only (C++17 inline) so it links cleanly into every isolated unit-test
// translation unit without per-test stub additions; it only calls the existing
// loadProfile() primitive (resolved per-binary).
// ============================================================================
#ifndef AATPED_PROFILE_H
#define AATPED_PROFILE_H

#include <string>
#include <mutex>
#include <atomic>

// Forward declaration so the header-only profile manager can log failures.
void logMsg(int level, const char* fmt, ...);

// Low-level profile tunable loader, defined in aatped_config.cpp. It applies
// profiles/<name>.conf and sets g_currentProfile / g_currentProfileEnum.
// The manager calls this ONLY when the winning profile changes.
void loadProfile(const std::string& profile);

namespace aatpe_profile {

// Higher value = higher priority in arbitration.
enum class ProfileSource : int {
    DEFAULT   = 0,  // fallback ("balanced"), not directly voted
    GLOBAL    = 1,  // global base profile
    AUTO      = 2,  // auto scene detection
    THERMAL   = 3,  // thermal warning auto-switch
    APP       = 4,  // per-app override
    MANUAL    = 5,  // user manual lock
    EMERGENCY = 6,  // thermal emergency
    COUNT     = 7
};

// Manager state. inline variables => exactly one instance across the program
// (C++17), regardless of how many translation units include this header.
inline std::mutex g_pmMutex;
inline std::string g_pmDesired[(int)ProfileSource::COUNT];  // empty = abstain
inline std::string g_pmApplied;                              // last applied profile
inline ProfileSource g_pmAppliedSource = ProfileSource::DEFAULT;
// arbitration generation counter.
// Every vote mutation (request/release) bumps this UNDER g_pmMutex. While an
// arbitration is in flight (loadProfile() runs with g_pmMutex RELEASED), another
// thread may change the votes. We snapshot the generation before releasing the
// lock and re-check it after re-acquiring; if it advanced, our loadProfile() ran
// against a now-stale vote set, so we MUST NOT overwrite g_pmApplied with the old
// winner. We discard and loop to re-arbitrate against the fresh votes. This is
// the anti-race that prevents "thread A finishes loadProfile(gaming) after thread
// B moved the winner to balanced, then A clobbers g_pmApplied back to gaming".
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

// profile-name whitelist. Every vote — regardless of
// source (GLOBAL/AUTO/APP/MANUAL/THERMAL/EMERGENCY) — must pass this check.
// Previously per-app config or a garbage control line could vote for
// "abc"/"unknown", causing loadProfile("garbage") to leave g_currentProfile in
// an inconsistent state. The whitelist is the four shipped profiles; anything
// else is rejected. (profiles/ also ships exactly these four .conf files.)
inline bool isValidProfileName(const std::string& name) {
    return name == "balanced" || name == "gaming" ||
           name == "battery"  || name == "thermal";
}

// Pick the highest-priority non-empty vote. Caller holds g_pmMutex.
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

// Apply the winner if it differs from what is currently applied. loadProfile()
// takes g_cfgMutex; we must NOT hold g_pmMutex across that call (lock order is
// always g_pmMutex -> g_cfgMutex; callers never hold g_cfgMutex here). The
// unique_lock is passed in and released/re-acquired around loadProfile().
//
// item 6 (generation guard): because the lock is released across loadProfile(),
// a concurrent vote change can land mid-load. We snapshot g_pmGeneration before
// releasing the lock; after re-acquiring, if it advanced we loop and re-pick the
// winner instead of stamping g_pmApplied with the stale result. The loop exits
// either when the winner already matches g_pmApplied (someone else applied it) or
// when we run a full loadProfile() with no intervening vote mutation.
//
// A2-013 (accepted observation): the unique_lock IS released across loadProfile()
// (it must be — loadProfile() takes g_cfgMutex and the lock order is
// g_pmMutex -> g_cfgMutex, never the reverse). The exposed window is bounded by
// the generation guard above: any vote mutation while unlocked bumps the generation
// and forces a re-pick, so g_pmApplied can never be stamped with a stale winner.
// The try/catch guarantees the lock is re-acquired even if loadProfile() throws.
// Accepted as-is; no lock-elision rewrite needed.
inline void arbitrateAndApplyLocked(std::unique_lock<std::mutex>& lk) {
    for (;;) {
        std::string winner;
        ProfileSource winnerSrc = pickWinnerLocked(winner);
        if (winner == g_pmApplied) {
            // No profile-NAME transition: the applied tunables are already correct,
            // so loadProfile() would be redundant. A *source-only* transition can
            // still happen though — e.g. APP already applied "gaming", then a
            // higher-priority MANUAL votes for the SAME "gaming" (or MANUAL later
            // releases and the winner drops back down to APP). The applied name is
            // unchanged but the winning SOURCE moved; we must still record it,
            // otherwise hasManualVote() and effectiveSource() disagree
            // (g_pmAppliedSource stuck on the old source). We hold g_pmMutex and
            // winnerSrc is this loop's fresh pickWinnerLocked() result, so the
            // assignment is safe and needs no generation guard (no loadProfile()).
            if (winnerSrc != g_pmAppliedSource) {
                g_pmAppliedSource = winnerSrc;
            }
            return;
        }
        const uint64_t gen = g_pmGeneration.load(std::memory_order_acquire);
        lk.unlock();
        // P2-16: if loadProfile() throws, the mutex would stay unlocked and
        // all subsequent profile operations would deadlock on lk.lock().
        // Use try/catch to ensure the lock is always re-acquired.
        try {
            loadProfile(winner);
        } catch (...) {
            lk.lock();
            throw;  // rethrow after re-locking; caller handles
        }
        lk.lock();
        if (g_pmGeneration.load(std::memory_order_acquire) != gen) {
            // Votes changed while loadProfile() was running. Our result is stale;
            // do NOT touch g_pmApplied. Re-pick the winner from the fresh votes.
            continue;
        }
        g_pmApplied = winner;
        g_pmAppliedSource = winnerSrc;
        return;
    }
}

// A source wants `profile` to be active. Empty profile == abstain (use
// profileRelease for clarity). Re-arbitrates immediately.
//
// //   P0-8: invalid profile names are rejected (no-op, logged nowhere hot) so a
//         garbage per-app/control vote can never trigger loadProfile("garbage").
//   P1-9/P1-10: if the source already votes for this exact profile, this is a
//         NO-OP: no generation bump, no re-arbitration. The AUTO main loop calls
//         this every cycle with the same result; without the dedup it would bump
//         generation and re-arbitrate forever.
inline void profileRequest(const std::string& profile, ProfileSource src) {
    if (profile.empty()) return;
    if (!isValidProfileName(profile)) return;  // P0-8: reject unknown profile
    std::unique_lock<std::mutex> lk(g_pmMutex);
    if (g_pmDesired[(int)src] == profile) return;  // P1-9: no-op dedup
    g_pmDesired[(int)src] = profile;
    bumpGenerationLocked();  // mark the vote set changed; invalidates in-flight arbitration
    arbitrateAndApplyLocked(lk);
}

// A source no longer has an opinion. Re-arbitrates so a lower-priority source
// (or the global default) takes over. This is the APP_EXIT / thermal-cool-down
// / emergency-recovery path.
//
// P1-9: releasing a source that was already abstaining is a no-op (no generation
// bump, no re-arbitration).
inline void profileRelease(ProfileSource src) {
    std::unique_lock<std::mutex> lk(g_pmMutex);
    if (g_pmDesired[(int)src].empty()) return;  // P1-9: no-op dedup
    g_pmDesired[(int)src].clear();
    bumpGenerationLocked();  // vote set changed; invalidate in-flight arbitration
    arbitrateAndApplyLocked(lk);
}

// Seed the GLOBAL base profile at boot. Does NOT call loadProfile() (the boot
// loadProfile(g_currentProfile) already applied the tunables); it only records
// the vote and syncs the "applied" bookkeeping so the first real arbitration
// does not spuriously reload.
inline void profileInitGlobal(const std::string& base, const std::string& actuallyApplied) {
    std::lock_guard<std::mutex> lk(g_pmMutex);
    g_pmDesired[(int)ProfileSource::GLOBAL] = base.empty() ? "balanced" : base;
    // P2-17: validate actuallyApplied against the whitelist so a corrupted
    // g_currentProfile on disk cannot leave g_pmApplied in an invalid state.
    g_pmApplied = isValidProfileName(actuallyApplied) ? actuallyApplied : "balanced";
    g_pmAppliedSource = ProfileSource::GLOBAL;
}

// Force re-apply the current winner (SIGUSR1 / after a profile .conf edit on
// disk). Unlike arbitrateAndApplyLocked it does NOT skip when winner ==
// g_pmApplied: the on-disk tunables may have changed and must be re-read. The
// generation guard still applies, so a stale result cannot clobber a newer
// winner. Production paths use this instead of calling loadProfile() directly.
// ROUND-004: wrap loadProfile() in try/catch — a corrupted .conf line (bad
// stoi / out-of-range) must NOT propagate into the scheduler main loop and
// crash the daemon; we keep the previously-applied tunables on parse error.
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
            return;  // do NOT stamp g_pmApplied with a half-applied state
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

// Read-only introspection (WebUI / status / tests).
inline std::string effectiveProfile() {
    std::lock_guard<std::mutex> lk(g_pmMutex);
    return g_pmApplied;
}
inline ProfileSource effectiveSource() {
    std::lock_guard<std::mutex> lk(g_pmMutex);
    return g_pmAppliedSource;
}

// the Profile Manager is the single source of truth for
// whether a manual lock is active. g_profileLock (the legacy atomic<bool>) is a
// derived mirror updated by set_profile / set_auto_profile; new code should read
// this instead of consulting g_profileLock directly, so a "MANUAL vote released"
// and "g_profileLock=false" can never disagree.
inline bool hasManualVote() {
    std::lock_guard<std::mutex> lk(g_pmMutex);
    return !g_pmDesired[(int)ProfileSource::MANUAL].empty();
}

// introspection accessor for tests (generation counter). The
// dedup rules (P1-9/P1-10) mean a no-op request/release must NOT bump this.
inline uint64_t generation() {
    return g_pmGeneration.load(std::memory_order_acquire);
}

}  // namespace aatpe_profile

#endif  // AATPED_PROFILE_H
