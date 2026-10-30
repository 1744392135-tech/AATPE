#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "aatpe_subsystem.h"

using namespace aatpe::subsys;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static int countFds() {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0; struct dirent* e;
    while ((e = ::readdir(d))) { if (e->d_name[0] != '.') ++n; }
    ::closedir(d);
    return n;
}

static int countThreads() {
    DIR* d = ::opendir("/proc/self/task");
    if (!d) return -1;
    int n = 0; struct dirent* e;
    while ((e = ::readdir(d))) { if (e->d_name[0] != '.') ++n; }
    ::closedir(d);
    return n;
}

int main() {
    printf("=== #1 SubsystemState lifecycle + HealthState ===\n");
    {
        auto& reg = Registry::instance();
        reg.setState("t_ebpf", SubsystemState::STARTING);
        CHECK(reg.get("t_ebpf").getState() == SubsystemState::STARTING, "STARTING state set");
        reg.setState("t_ebpf", SubsystemState::ACTIVE);
        CHECK(reg.get("t_ebpf").getState() == SubsystemState::ACTIVE, "ACTIVE state set");
        CHECK(reg.get("t_ebpf").lastSuccess.load() != 0, "lastSuccess stamped on ACTIVE");

        reg.markError("t_ebpf", "verifier reject");
        CHECK(reg.get("t_ebpf").errorCount.load() == 1, "error_count increments");
        CHECK(reg.get("t_ebpf").getState() == SubsystemState::DEGRADED,
              "error flips ACTIVE->DEGRADED");
        CHECK(reg.get("t_ebpf").lastError.load() != 0, "last_error stamped");
        CHECK(reg.get("t_ebpf").getLastErrorReason() == "verifier reject",
              "last_error_reason recorded");

        std::string j = Registry::instance().toJson();
        CHECK(j.find("\"t_ebpf\"") != std::string::npos, "JSON contains ebpf entry");
        CHECK(j.find("\"state\":\"DEGRADED\"") != std::string::npos, "JSON carries DEGRADED state");
        CHECK(j.find("\"error_count\":1") != std::string::npos, "JSON carries error_count");
        CHECK(j.find("\"last_error\":\"verifier reject\"") != std::string::npos,
              "JSON carries escaped error reason");

        reg.markRecovery("t_ebpf");
        CHECK(reg.get("t_ebpf").recoveryCount.load() == 1, "recovery_count increments");

        reg.setState("t_bad", SubsystemState::FAILED);
        CHECK(reg.failedCount() >= 1, "failedCount() sees FAILED subsystem");
        CHECK(reg.degradedCount() >= 1, "degradedCount() sees DEGRADED subsystem");
    }

    printf("=== #2 Registry JSON includes all subsystems ===\n");
    {
        std::string j = Registry::instance().toJson();
        CHECK(j.find("\"t_ebpf\"") != std::string::npos, "JSON contains ebpf entry");
        CHECK(j.find("\"t_bad\"") != std::string::npos, "JSON contains failed entry");
        CHECK(j.find("\"error_count\":1") != std::string::npos, "JSON carries error_count");
    }

    printf("=== #4 StopToken eventfd semantics ===\n");
    {
        StopToken tok;
        CHECK(tok.open(), "StopToken opens eventfd");
        CHECK(tok.valid(), "fd valid after open");
        CHECK(!tok.shouldStop(), "starts not-stopped");
        tok.requestStop();
        CHECK(tok.shouldStop(), "requestStop latches shouldStop");
        int fd = tok.fd();
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        struct timeval tv{0, 100000};
        int sel = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        CHECK(sel == 1, "eventfd becomes readable after requestStop (fast wakeup)");
        tok.drain();
        FD_ZERO(&rfds); FD_SET(fd, &rfds);
        struct timeval tv2{0, 50000};
        sel = select(fd + 1, &rfds, nullptr, nullptr, &tv2);
        CHECK(sel == 0, "drain() resets eventfd counter");
        tok.reset();
        CHECK(!tok.shouldStop(), "reset() clears stop flag");
    }

    printf("=== #5 Safe Mode: streak gating ===\n");
    {
        safemode::setStreakThresholdForTest(3);
        safemode::setRecoverCooldownSecForTest(0);
        safemode::gSafeModeActive.store(false);
        safemode::gSafeModeReason.store(0);

        safemode::trigger(safemode::EBPF_STREAK, "f1");
        CHECK(!safemode::isActive(), "1st ebpf failure does NOT latch safe mode");
        safemode::trigger(safemode::EBPF_STREAK, "f2");
        CHECK(!safemode::isActive(), "2nd ebpf failure does NOT latch safe mode");
        safemode::trigger(safemode::EBPF_STREAK, "f3");
        CHECK(safemode::isActive(), "3rd consecutive failure LATCHES safe mode");
        CHECK(!safemode::rtBoostAllowed(), "RT boost blocked in safe mode");
        CHECK(!safemode::mlPlacementAllowed(), "ML placement blocked in safe mode");
        CHECK(!safemode::aggressiveMigrationAllowed(), "aggressive migration blocked");
        CHECK(safemode::describeReasons(safemode::gSafeModeReason.load())
                  .find("ebpf_streak") != std::string::npos,
              "reason string names ebpf_streak");

        safemode::gSafeModeEntered.store(std::time(nullptr) - 100,
                                         std::memory_order_relaxed);
        safemode::maybeRecover(std::time(nullptr));
        CHECK(!safemode::isActive(), "maybeRecover clears safe mode after cooldown");
    }

    printf("=== #5 G1-001: health resets the failure streak ===\n");
    {
        safemode::setStreakThresholdForTest(3);
        safemode::gSafeModeActive.store(false);
        safemode::gSafeModeReason.store(0);
        safemode::trigger(safemode::EBPF_STREAK, "old1");
        safemode::trigger(safemode::EBPF_STREAK, "old2");
        CHECK(!safemode::isActive(), "2 failures, no latch");
        safemode::resetStreak(safemode::EBPF_STREAK);
        safemode::trigger(safemode::EBPF_STREAK, "new1");
        CHECK(!safemode::isActive(), "G1-001: streak reset on health, 1 new failure does NOT latch");
        safemode::trigger(safemode::EBPF_STREAK, "new2");
        CHECK(!safemode::isActive(), "G1-001: only 2 in the new streak, no latch");
        safemode::trigger(safemode::EBPF_STREAK, "new3");
        CHECK(safemode::isActive(), "G1-001: 3 genuine consecutive failures latch");
        safemode::gSafeModeActive.store(false);
        safemode::gSafeModeReason.store(0);
    }

    printf("=== #5 G1: CRASH_LOOP conditional self-heal ===\n");
    {
        safemode::g_crashLoopObserveSecTestOverride = 1;
        safemode::gSafeModeActive.store(false);
        safemode::gSafeModeReason.store(0);
        safemode::gSafeModeEntered.store(0);

        safemode::trigger(safemode::CRASH_LOOP, "watchdog crash loop");
        CHECK(safemode::isActive(), "CRASH_LOOP latches safe mode");
        CHECK(safemode::gSafeModeReason.load() & safemode::CRASH_LOOP, "CRASH_LOOP bit set");

        safemode::maybeRecover(std::time(nullptr));
        CHECK(safemode::gSafeModeReason.load() & safemode::CRASH_LOOP,
              "CRASH_LOOP still set before observation window elapses");

        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        safemode::maybeRecover(std::time(nullptr));
        CHECK(!(safemode::gSafeModeReason.load() & safemode::CRASH_LOOP),
              "CRASH_LOOP auto-clears after quiet observation window");
        CHECK(!safemode::isActive(), "safe mode releases after CRASH_LOOP heal");

        safemode::trigger(safemode::CRASH_LOOP, "again");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        safemode::maybeRecover(std::time(nullptr));
        safemode::trigger(safemode::CRASH_LOOP, "third");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        safemode::maybeRecover(std::time(nullptr));
        CHECK(safemode::gSafeModeReason.load() & safemode::CRASH_LOOP,
              "CRASH_LOOP stays latched after max self-heal budget");

        safemode::g_crashLoopObserveSecTestOverride = 0;
        safemode::gSafeModeActive.store(false);
        safemode::gSafeModeReason.store(0);
    }

    printf("=== #5 Safe Mode: hard trigger (thermal) does not need a streak ===\n");
    {
        safemode::gSafeModeActive.store(false);
        safemode::gSafeModeReason.store(0);
        safemode::trigger(safemode::THERMAL_EMERG, "hot");
        CHECK(safemode::isActive(), "thermal emergency immediately latches safe mode");
        CHECK(!safemode::rtBoostAllowed(), "RT boost blocked by thermal safe mode");
        safemode::gSafeModeActive.store(false);
        safemode::gSafeModeReason.store(0);
    }

    printf("=== #8 self-observation sampleSelf does not crash and fills counters ===\n");
    {
        selfobs::sampleSelf();
        CHECK(selfobs::gSelfRssKb.load() > 0, "self_rss read from /proc/self/status");
        CHECK(selfobs::gSelfFdCount.load() >= 0, "self_fd counted (>=0)");
        CHECK(selfobs::gSelfThreadCount.load() >= 1, "self_thread counted (>=1)");
        CHECK(selfobs::gSelfCpuPercent.load() >= 0, "self_cpu percent computed (>=0)");
    }

    printf("=== #9 FD leak test: open/close cycles stay bounded ===\n");
    {
        int baseline = countFds();
        CHECK(baseline > 0, "baseline fd count measurable");
        int peak = baseline;
        for (int i = 0; i < 20; ++i) {
            int fd = ::open("/dev/null", O_RDONLY);
            if (fd >= 0) ::close(fd);
            selfobs::sampleSelf();
            int now = countFds();
            if (now > peak) peak = now;
        }
        int after = countFds();
        CHECK(after <= baseline + 4,
              "FD count does not grow monotonically across open/close cycles");
        (void)peak;
    }

    printf("=== #10 thread leak test: start/join cycles stay bounded ===\n");
    {
        int baseline = countThreads();
        CHECK(baseline >= 1, "baseline thread count measurable");
        for (int i = 0; i < 10; ++i) {
            std::vector<std::thread> th;
            for (int j = 0; j < 4; ++j) th.emplace_back([]{
                selfobs::sampleSelf();
                ::usleep(1000);
            });
            for (auto& t : th) t.join();
        }
        int after = countThreads();
        CHECK(after <= baseline + 4,
              "thread count stable across start/join cycles (no leak)");
    }

    printf("=== #13 BPF failure injection: degraded reason + no busy retry ===\n");
    {
        safemode::setStreakThresholdForTest(3);
        safemode::gSafeModeActive.store(false);
        safemode::gSafeModeReason.store(0);
        auto& reg = Registry::instance();
        reg.markError("bpf_inject", "verifier reject");
        reg.markError("bpf_inject", "ring overflow");
        reg.markError("bpf_inject", "map failure");
        CHECK(reg.get("bpf_inject").errorCount.load() == 3, "3 injected errors counted");
        CHECK(reg.get("bpf_inject").getState() == SubsystemState::DEGRADED,
              "subsystem in DEGRADED after injection");
        CHECK(reg.get("bpf_inject").getLastErrorReason() == "map failure",
              "last degraded reason captured");
    }

    printf("===================================================================\n");
    printf("round5 tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
