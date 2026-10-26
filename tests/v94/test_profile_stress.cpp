// AATPE round-3 — #27 Profile high-concurrency stress + dedup + whitelist.
//
// Verifies the round-3 profile-manager fixes:
//   P1-9/P1-10: requesting the SAME vote again, or releasing an absent vote, does
//               NOT bump the arbitration generation. The AUTO main loop calls
//               profileRequest() every cycle with the same result; without the
//               dedup it would bump generation and re-arbitrate forever.
//   P0-8:       invalid profile names ("garbage", "abc", "unknown") are rejected.
//   #27:        a random mixed-source storm (GLOBAL/AUTO/APP/THERMAL/MANUAL/
//               EMERGENCY) of hundreds of thousands of operations must leave the
//               winner (effectiveProfile) consistent with the actual loaded
//               profile, and must never select an invalid name.
#include "aatped_profile.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

// --- host stub for loadProfile (mirrors test_profile_arb.cpp) ----------------
static std::mutex g_loadMu;
static std::string g_loadedProfile = "balanced";
static std::atomic<long> g_loadCount{0};

void loadProfile(const std::string& profile) {
    {
        std::lock_guard<std::mutex> lk(g_loadMu);
        g_loadedProfile = profile;
    }
    g_loadCount.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::microseconds(20));
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

int main() {
    using namespace aatpe_profile;
    printf("==> profile_stress: dedup + whitelist + storm ==\n");
    profileInitGlobal("balanced", "balanced");

    // --- P0-8: invalid profile names are rejected ---------------------------
    uint64_t genBefore = generation();
    profileRequest("garbage", ProfileSource::AUTO);
    profileRequest("abc", ProfileSource::APP);
    profileRequest("unknown", ProfileSource::MANUAL);
    profileRequest("gaming ", ProfileSource::THERMAL);  // trailing space, not in whitelist
    CHECK(effectiveProfile() == "balanced",
          "invalid profile names rejected (still DEFAULT balanced)");
    CHECK(generation() == genBefore,
          "rejected votes did not bump generation");

    // --- P1-9/P1-10: no-op request does NOT bump generation -----------------
    profileRequest("gaming", ProfileSource::AUTO);       // real change
    uint64_t afterFirst = generation();
    CHECK(effectiveProfile() == "gaming", "AUTO=gaming accepted");
    profileRequest("gaming", ProfileSource::AUTO);       // identical repeat
    CHECK(generation() == afterFirst,
          "identical repeated request does NOT bump generation (P1-10 dedup)");
    CHECK(effectiveProfile() == "gaming", "still gaming after repeat");

    // Releasing an absent vote (MANUAL was never requested) does not bump.
    uint64_t beforeReleaseNoop = generation();
    profileRelease(ProfileSource::MANUAL);
    CHECK(generation() == beforeReleaseNoop,
          "release of absent vote does NOT bump generation (P1-9 dedup)");

    // Re-requesting the same vote after an explicit release IS a change.
    profileRelease(ProfileSource::AUTO);
    CHECK(effectiveProfile() == "balanced", "after AUTO release -> DEFAULT balanced");

    // --- #27: random mixed-source storm --------------------------------------
    // Randomly request/release across all six sources, hundreds of thousands of
    // ops. The winner must always be a valid whitelist profile and must match
    // what loadProfile last loaded.
    const char* kValid[] = {"balanced", "gaming", "battery", "thermal"};
    std::atomic<bool> stop{false};
    auto worker = [&](int seed) {
        unsigned int rng = (unsigned int)seed;
        for (int i = 0; i < 60000 && !stop.load(std::memory_order_relaxed); ++i) {
            rng = rng * 1103515245u + 12345u;
            int src = (rng >> 16) % 6;
            int prof = (rng >> 8) & 3;
            ProfileSource s = static_cast<ProfileSource>(src);
            // 50% request, 50% release.
            if (rng & 1) {
                profileRequest(kValid[prof], s);
            } else {
                profileRelease(s);
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < 6; ++t) pool.emplace_back(worker, t + 1);
    for (auto& t : pool) t.join();
    stop.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // After the storm, every source's vote state is arbitrary but the winner
    // must still be a valid profile.
    std::string win = effectiveProfile();
    bool winValid = (win == "balanced" || win == "gaming" ||
                     win == "battery"  || win == "thermal");
    CHECK(winValid, "storm winner is a whitelist profile");
    {
        std::lock_guard<std::mutex> lk(g_loadMu);
        CHECK(g_loadedProfile == win,
              "effectiveProfile matches the actually-loaded profile");
    }
    CHECK(hasManualVote() == (effectiveSource() == ProfileSource::MANUAL),
          "hasManualVote consistent with effectiveSource");

    printf("==> profile_stress: loadCount=%ld operations done\n", g_loadCount.load());

    if (failures == 0) {
        printf("profile_stress: ALL PASS\n");
        return 0;
    }
    printf("profile_stress: %d FAILURES\n", failures);
    return 1;
}
