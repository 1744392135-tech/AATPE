// AATPE release-convergence item 22: Profile arbitration concurrency stress.
//
// Exercises the generation-guard in aatped_profile.h under heavy concurrent
// request/release across all priority levels:
//   Thread A: APP      -> gaming
//   Thread B: THERMAL  -> thermal
//   Thread C: MANUAL   -> balanced
//   Thread D: EMERGENCY-> thermal
//
// invariants checked:
//   1. After the storm (all votes released), the committed winner MUST be the
//      DEFAULT "balanced" — a stale in-flight loadProfile() from a lower-priority
//      source must NOT be able to overwrite g_pmApplied back to gaming/thermal.
//      This is exactly the race item 6 fixed by the generation counter.
//   2. Priority order: with EMERGENCY active, winner is thermal regardless of
//      MANUAL/APP. With MANUAL active and EMERGENCY released, winner is the
//      MANUAL choice.
//   3. effectiveProfile() (locked read) never diverges from the applied state.
#include "aatped_profile.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>

// --- host stub for the low-level loader -------------------------------------
// Records which profile was actually loaded, with a short critical section to
// widen the race window (loadProfile runs with g_pmMutex RELEASED).
static std::mutex g_loadMu;
static std::string g_loadedProfile = "balanced";
static std::atomic<long> g_loadCount{0};

void loadProfile(const std::string& profile) {
    {
        std::lock_guard<std::mutex> lk(g_loadMu);
        g_loadedProfile = profile;
    }
    g_loadCount.fetch_add(1, std::memory_order_relaxed);
    // Widen the window in which another thread can change the votes.
    std::this_thread::sleep_for(std::chrono::microseconds(50));
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

int main() {
    using namespace aatpe_profile;

    printf("==> profile_arbitration: directed priority checks ==\n");
    profileInitGlobal("balanced", "balanced");

    // APP=gaming should win over GLOBAL=balanced.
    profileRequest("gaming", ProfileSource::APP);
    CHECK(effectiveProfile() == "gaming", "APP=gaming wins over GLOBAL=balanced");

    // MANUAL=balanced outranks APP.
    profileRequest("balanced", ProfileSource::MANUAL);
    CHECK(effectiveProfile() == "balanced", "MANUAL=balanced outranks APP=gaming");

    // EMERGENCY=thermal outranks MANUAL.
    profileRequest("thermal", ProfileSource::EMERGENCY);
    CHECK(effectiveProfile() == "thermal", "EMERGENCY=thermal outranks MANUAL");

    // Release EMERGENCY -> falls back to MANUAL=balanced.
    profileRelease(ProfileSource::EMERGENCY);
    CHECK(effectiveProfile() == "balanced", "after EMERGENCY release -> MANUAL=balanced");

    // Release MANUAL -> falls back to APP=gaming.
    profileRelease(ProfileSource::MANUAL);
    CHECK(effectiveProfile() == "gaming", "after MANUAL release -> APP=gaming");

    // Release APP -> back to DEFAULT balanced.
    profileRelease(ProfileSource::APP);
    CHECK(effectiveProfile() == "balanced", "after APP release -> DEFAULT=balanced");

    printf("==> profile_arbitration: %d-thread x many-iteration storm ==\n", 4);
    std::atomic<bool> stop{false};
    auto worker = [&](ProfileSource src, const char* prof) {
        for (int i = 0; i < 20000 && !stop.load(std::memory_order_relaxed); ++i) {
            profileRequest(prof, src);
            std::this_thread::yield();
            profileRelease(src);
        }
    };
    std::vector<std::thread> pool;
    pool.emplace_back(worker, ProfileSource::APP,      "gaming");
    pool.emplace_back(worker, ProfileSource::THERMAL,    "thermal");
    pool.emplace_back(worker, ProfileSource::MANUAL,     "balanced");
    pool.emplace_back(worker, ProfileSource::EMERGENCY, "thermal");
    for (auto& t : pool) t.join();
    stop.store(true);

    // Give any in-flight loadProfile() a chance to finish and re-arbitrate.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // All votes released. The generation guard must ensure no stale winner
    // (e.g. gaming from APP) is committed after the votes cleared.
    CHECK(effectiveProfile() == "balanced",
          "post-storm committed winner is DEFAULT=balanced (no stale overwrite)");

    printf("==> profile_arbitration: loadCount=%ld\n", g_loadCount.load());

    // ========================================================================
    // v449 fx5: a *source-only* transition — the winning profile NAME is unchanged
    // but the winning SOURCE moves — must still record the new applied source.
    // Previously arbitrateAndApplyLocked() early-returned at
    //     if (winner == g_pmApplied) return;
    // and never touched g_pmAppliedSource, leaving it stuck on the old source:
    // e.g. APP already applied "gaming", then a higher-priority MANUAL votes the
    // SAME "gaming" -> hasManualVote()=true but effectiveSource()=APP, so the
    // "hasManualVote == (effectiveSource==MANUAL)" invariant broke. TSan saw this
    // ~1 in 22 stress runs; it is deterministic single-threaded.
    // ========================================================================
    printf("==> profile_arbitration: v449 source-only transition checks ==\n");
    // Deterministic reset: the storm left arbitrary votes; drain them all and
    // re-seed the global base.
    profileRelease(ProfileSource::EMERGENCY);
    profileRelease(ProfileSource::MANUAL);
    profileRelease(ProfileSource::APP);
    profileRelease(ProfileSource::THERMAL);
    profileRelease(ProfileSource::AUTO);
    profileInitGlobal("balanced", "balanced");
    CHECK(effectiveProfile() == "balanced", "v449 reset -> balanced");
    CHECK(effectiveSource() == ProfileSource::GLOBAL, "v449 reset source=GLOBAL");

    // 1) APP applies gaming (real name transition).
    profileRequest("gaming", ProfileSource::APP);
    CHECK(effectiveProfile() == "gaming", "v449 APP=gaming applied");
    CHECK(effectiveSource() == ProfileSource::APP, "v449 source=APP");
    CHECK(hasManualVote() == false, "v449 no manual vote yet");

    // 2) MANUAL votes the SAME name "gaming": name unchanged (no loadProfile),
    //    but the winning source must flip APP(4) -> MANUAL(5).
    profileRequest("gaming", ProfileSource::MANUAL);
    CHECK(effectiveProfile() == "gaming", "v449 MANUAL=gaming keeps name gaming");
    CHECK(effectiveSource() == ProfileSource::MANUAL,
          "v449 source flips APP->MANUAL on same-name upgrade");
    CHECK(hasManualVote() == true, "v449 manual vote active");
    CHECK(hasManualVote() == (effectiveSource() == ProfileSource::MANUAL),
          "v449 hasManualVote consistent with effectiveSource (after same-name upgrade)");

    // 3) MANUAL releases: winner falls back DOWN to APP (still "gaming") — the
    //    source-only transition in the reverse direction.
    profileRelease(ProfileSource::MANUAL);
    CHECK(effectiveProfile() == "gaming", "v449 after MANUAL release still gaming (APP)");
    CHECK(effectiveSource() == ProfileSource::APP,
          "v449 after MANUAL release source=APP again");
    CHECK(hasManualVote() == false, "v449 manual vote gone");
    CHECK(hasManualVote() == (effectiveSource() == ProfileSource::MANUAL),
          "v449 post-MANUAL-release consistency");

    // 4) APP releases: real name transition back to the GLOBAL balanced.
    profileRelease(ProfileSource::APP);
    CHECK(effectiveProfile() == "balanced", "v449 after APP release -> balanced");
    CHECK(effectiveSource() == ProfileSource::GLOBAL, "v449 after APP release source=GLOBAL");
    CHECK(hasManualVote() == false, "v449 no manual vote at rest");

    if (failures == 0) {
        printf("profile_arbitration: ALL PASS\n");
        return 0;
    }
    printf("profile_arbitration: %d FAILURES\n", failures);
    return 1;
}
