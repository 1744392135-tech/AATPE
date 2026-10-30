#include "aatped_profile.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

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

    uint64_t genBefore = generation();
    profileRequest("garbage", ProfileSource::AUTO);
    profileRequest("abc", ProfileSource::APP);
    profileRequest("unknown", ProfileSource::MANUAL);
    profileRequest("gaming ", ProfileSource::THERMAL);
    CHECK(effectiveProfile() == "balanced",
          "invalid profile names rejected (still DEFAULT balanced)");
    CHECK(generation() == genBefore,
          "rejected votes did not bump generation");

    profileRequest("gaming", ProfileSource::AUTO);
    uint64_t afterFirst = generation();
    CHECK(effectiveProfile() == "gaming", "AUTO=gaming accepted");
    profileRequest("gaming", ProfileSource::AUTO);
    CHECK(generation() == afterFirst,
          "identical repeated request does NOT bump generation (P1-10 dedup)");
    CHECK(effectiveProfile() == "gaming", "still gaming after repeat");

    uint64_t beforeReleaseNoop = generation();
    profileRelease(ProfileSource::MANUAL);
    CHECK(generation() == beforeReleaseNoop,
          "release of absent vote does NOT bump generation (P1-9 dedup)");

    profileRelease(ProfileSource::AUTO);
    CHECK(effectiveProfile() == "balanced", "after AUTO release -> DEFAULT balanced");

    const char* kValid[] = {"balanced", "gaming", "battery", "thermal"};
    std::atomic<bool> stop{false};
    auto worker = [&](int seed) {
        unsigned int rng = (unsigned int)seed;
        for (int i = 0; i < 60000 && !stop.load(std::memory_order_relaxed); ++i) {
            rng = rng * 1103515245u + 12345u;
            int src = (rng >> 16) % 6;
            int prof = (rng >> 8) & 3;
            ProfileSource s = static_cast<ProfileSource>(src);
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
