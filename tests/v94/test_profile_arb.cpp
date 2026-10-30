#include "aatped_profile.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>

static std::mutex g_loadMu;
static std::string g_loadedProfile = "balanced";
static std::atomic<long> g_loadCount{0};

void loadProfile(const std::string& profile) {
    {
        std::lock_guard<std::mutex> lk(g_loadMu);
        g_loadedProfile = profile;
    }
    g_loadCount.fetch_add(1, std::memory_order_relaxed);
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

    profileRequest("gaming", ProfileSource::APP);
    CHECK(effectiveProfile() == "gaming", "APP=gaming wins over GLOBAL=balanced");

    profileRequest("balanced", ProfileSource::MANUAL);
    CHECK(effectiveProfile() == "balanced", "MANUAL=balanced outranks APP=gaming");

    profileRequest("thermal", ProfileSource::EMERGENCY);
    CHECK(effectiveProfile() == "thermal", "EMERGENCY=thermal outranks MANUAL");

    profileRelease(ProfileSource::EMERGENCY);
    CHECK(effectiveProfile() == "balanced", "after EMERGENCY release -> MANUAL=balanced");

    profileRelease(ProfileSource::MANUAL);
    CHECK(effectiveProfile() == "gaming", "after MANUAL release -> APP=gaming");

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

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CHECK(effectiveProfile() == "balanced",
          "post-storm committed winner is DEFAULT=balanced (no stale overwrite)");

    printf("==> profile_arbitration: loadCount=%ld\n", g_loadCount.load());

    printf("==> profile_arbitration: v449 source-only transition checks ==\n");
    profileRelease(ProfileSource::EMERGENCY);
    profileRelease(ProfileSource::MANUAL);
    profileRelease(ProfileSource::APP);
    profileRelease(ProfileSource::THERMAL);
    profileRelease(ProfileSource::AUTO);
    profileInitGlobal("balanced", "balanced");
    CHECK(effectiveProfile() == "balanced", "v449 reset -> balanced");
    CHECK(effectiveSource() == ProfileSource::GLOBAL, "v449 reset source=GLOBAL");

    profileRequest("gaming", ProfileSource::APP);
    CHECK(effectiveProfile() == "gaming", "v449 APP=gaming applied");
    CHECK(effectiveSource() == ProfileSource::APP, "v449 source=APP");
    CHECK(hasManualVote() == false, "v449 no manual vote yet");

    profileRequest("gaming", ProfileSource::MANUAL);
    CHECK(effectiveProfile() == "gaming", "v449 MANUAL=gaming keeps name gaming");
    CHECK(effectiveSource() == ProfileSource::MANUAL,
          "v449 source flips APP->MANUAL on same-name upgrade");
    CHECK(hasManualVote() == true, "v449 manual vote active");
    CHECK(hasManualVote() == (effectiveSource() == ProfileSource::MANUAL),
          "v449 hasManualVote consistent with effectiveSource (after same-name upgrade)");

    profileRelease(ProfileSource::MANUAL);
    CHECK(effectiveProfile() == "gaming", "v449 after MANUAL release still gaming (APP)");
    CHECK(effectiveSource() == ProfileSource::APP,
          "v449 after MANUAL release source=APP again");
    CHECK(hasManualVote() == false, "v449 manual vote gone");
    CHECK(hasManualVote() == (effectiveSource() == ProfileSource::MANUAL),
          "v449 post-MANUAL-release consistency");

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
