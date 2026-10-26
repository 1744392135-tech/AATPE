// test_subsystem.cpp — v4.4.9 G1 host regression for aatpe_subsystem.
//
// Covers:
//   - SubsystemHealth state machine (setState/markSuccess/markError/markRecovery)
//   - Registry: idempotent get(), per-slot routing, failedCount/degradedCount,
//     writeStateFile(dir) -> <dir>/subsystem_health JSON
//   - StopToken: open/requestStop/shouldStop/drain/reset eventfd semantics
//   - light concurrency: concurrent Registry::get on a fresh name -> one slot
#include "aatpe_subsystem.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace aatpe::subsys;

static int g_fail = 0;
#define CHECK(c, m) do { if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); g_fail++; } } while(0)

int main() {
    printf("=== G1: aatpe_subsystem health/registry/StopToken ===\n");

    // ---- SubsystemHealth state machine ----
    {
        SubsystemHealth h;
        h.setState(SubsystemState::ACTIVE);
        CHECK(h.getState() == SubsystemState::ACTIVE, "setState -> ACTIVE");

        h.markSuccess();
        CHECK(h.lastSuccess.load() != 0, "markSuccess updates lastSuccess");

        time_t before = h.lastError.load();
        h.markError("boom");
        CHECK(h.errorCount.load() == 1, "markError increments error_count");
        CHECK(h.lastError.load() >= before, "markError updates lastError");
        CHECK(h.getLastErrorReason() == "boom", "markError stores reason");
        h.markError("again");
        CHECK(h.errorCount.load() == 2, "second markError -> count 2");

        h.markRecovery();
        CHECK(h.recoveryCount.load() == 1, "markRecovery increments recovery_count");
    }

    // ---- Registry ----
    Registry& reg = Registry::instance();
    {
        SubsystemHealth& a = reg.get("g1_ebpf");
        SubsystemHealth& b = reg.get("g1_ebpf");
        CHECK(&a == &b, "Registry::get idempotent (same slot)");

        reg.setState("g1_ebpf", SubsystemState::ACTIVE);
        reg.markSuccess("g1_ebpf");
        reg.markError("g1_webui", "weird");
        reg.markError("g1_webui", "weird2");
        reg.markRecovery("g1_webui");
        reg.setState("g1_webui", SubsystemState::DEGRADED);
        reg.setState("g1_ml", SubsystemState::FAILED);
        reg.markError("g1_ml", "crash");

        CHECK(reg.get("g1_webui").errorCount.load() == 2, "Registry routes markError to webui slot");
        CHECK(reg.get("g1_webui").recoveryCount.load() == 1, "Registry routes markRecovery");
        CHECK(reg.get("g1_ebpf").getState() == SubsystemState::ACTIVE, "Registry setState lands on right slot");

        CHECK(reg.failedCount() >= 1, "failedCount sees FAILED slot");
        CHECK(reg.degradedCount() >= 1, "degradedCount sees DEGRADED slot");
    }

    // ---- writeStateFile ----
    {
        system("rm -rf /tmp/aatpe_g1_sub && mkdir -p /tmp/aatpe_g1_sub");
        reg.writeStateFile("/tmp/aatpe_g1_sub");
        std::ifstream f("/tmp/aatpe_g1_sub/subsystem_health");
        std::stringstream ss; ss << f.rdbuf();
        std::string j = ss.str();
        CHECK(!j.empty(), "writeStateFile produces non-empty subsystem_health");
        CHECK(j.find("g1_ebpf") != std::string::npos, "JSON contains ebpf slot");
        CHECK(j.find("g1_webui") != std::string::npos, "JSON contains webui slot");
        CHECK(j.find("g1_ml") != std::string::npos, "JSON contains ml slot");
        CHECK(j.find("error_count") != std::string::npos, "JSON carries error_count field");
    }

    // ---- StopToken ----
    {
        StopToken tok;
        CHECK(!tok.valid(), "StopToken starts invalid");
        CHECK(tok.open(), "StopToken open() succeeds");
        CHECK(tok.valid(), "StopToken valid after open");
        CHECK(!tok.shouldStop(), "shouldStop false before requestStop");
        tok.requestStop();
        CHECK(tok.shouldStop(), "shouldStop true after requestStop");
        tok.drain();
        CHECK(tok.shouldStop(), "shouldStop stays true after drain (latch)");
        tok.reset();
        CHECK(!tok.valid() || !tok.shouldStop(), "reset() re-arms token");
    }

    // ---- light concurrency: concurrent get on a fresh name ----
    {
        const char* fresh = "g1_fresh_concurrent";
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i) ts.emplace_back([&]() { Registry::instance().get(fresh); });
        for (auto& t : ts) t.join();
        // exactly one slot: listNames must contain it once (map dedups)
        auto names = Registry::instance().listNames();
        int cnt = 0;
        for (auto& n : names) if (n == fresh) cnt++;
        CHECK(cnt == 1, "concurrent get -> exactly one slot");
    }

    // ---- STATE-01 regression: /proc/self/stat field parsing ----
    // A realistic /proc/<pid>/stat tail after ") ":
    //   state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime ...
    // We choose tpgid=-1 (daemon not on a tty), flags=4194304 (0x400000),
    // minflt=12345, and utime=100 stime=200. The OLD buggy parser read
    // tpgid/flags as utime/stime (returning -1/4194304); the fix must return
    // 100/200.
    {
        using namespace aatpe::subsys::selfobs;
        // after ")" : state=R ppid=1 pgrp=1000 session=0 tty_nr=0 tpgid=-1
        //             flags=4194304 minflt=12345 cminflt=0 majflt=12 cmajflt=0
        //             utime=100 stime=200 cutime=0 cstime=0 ...
        std::string tail = "R 1 1000 0 0 -1 4194304 12345 0 12 0 100 200 0 0 20 0 1 0 123456789";
        long long ut = -1, st = -1;
        bool ok = parseSelfStatCpuForTest(tail, ut, st);
        CHECK(ok, "parseSelfStatCpuForTest parses a well-formed tail");
        CHECK(ut == 100, "utime extracted from field 12 (not tpgid=-1)");
        CHECK(st == 200, "stime extracted from field 13 (not flags=4194304)");

        // Malformed input must return false (no UB, no garbage).
        long long a = -999, b = -999;
        bool bad = parseSelfStatCpuForTest("R 1 1000", a, b);
        CHECK(!bad, "parseSelfStatCpuForTest rejects truncated tail");
        (void)a; (void)b;
    }

    // ---- STATE-01: sampleSelf() on the real process yields non-zero CPU% ----
    // Burn CPU between two samples so utime+stime actually advances. The old
    // buggy parser always read constant tpgid/flags and produced pct=0.
    {
        using namespace aatpe::subsys::selfobs;
        // First sample establishes the baseline.
        sampleSelf();
        // Burn ~50ms of busy work.
        volatile double sink = 0.0;
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.05) {
            for (int i = 0; i < 100000; ++i) sink += (double)i * 1.0001;
        }
        (void)sink;
        sampleSelf();
        int pct = gSelfCpuPercent.load();
        CHECK(pct > 0, "sampleSelf() computes non-zero self CPU% after busy work");
    }

    // ---- STATE-01: evaluateThrottle() actually engages when CPU is over budget ----
    {
        using namespace aatpe::subsys::selfobs;
        setCpuBudgetPercentForTest(10);   // budget 10%
        setSustainSecForTest(1);          // sustain 1s
        gSelfCpuPercent.store(99);        // way over budget
        bool first = evaluateThrottle();  // over but not sustained yet
        CHECK(!first, "evaluateThrottle: not throttled on first over-budget sample");
        // Wait past the sustain window.
        struct timespec ts = {1, 50 * 1000 * 1000};  // 1.05s
        ::nanosleep(&ts, nullptr);
        bool second = evaluateThrottle();
        CHECK(second, "evaluateThrottle: throttled after sustain window over budget");
        CHECK(gThrottled.load(), "gThrottled latched true");
    }

    // ---- STATE-03: safemode trigger/clearReason concurrency invariant ----
    // Fire many trigger(THERMAL_EMERG) and clearReason(THERMAL_EMERG) from
    // concurrent threads. The invariant: reason==0 <=> active==false. The old
    // TOCTOU could leave active=false while reason!=0 (or vice versa) in a tiny
    // window; the pair-mutex fix closes it. After all threads join, the final
    // state must satisfy the invariant.
    {
        using namespace aatpe::subsys::safemode;
        // Start from a clean slate.
        gSafeModeReason.store(0);
        gSafeModeActive.store(false);
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i) {
            ts.emplace_back([]() {
                for (int k = 0; k < 200; ++k) {
                    trigger(THERMAL_EMERG, "stress");
                    clearReason(THERMAL_EMERG);
                }
            });
        }
        for (auto& t : ts) t.join();
        int reason = gSafeModeReason.load();
        bool active = isActive();
        CHECK((reason == 0) == (!active),
              "STATE-03: after concurrent trigger/clear, reason==0 iff !active");
        // If reason is non-zero (some trigger won the last race), active must be true.
        if (reason != 0) {
            CHECK(active, "STATE-03: reason!=0 implies active latched");
        } else {
            CHECK(!active, "STATE-03: reason==0 implies latch dropped");
        }
    }

    if (g_fail == 0) { printf("=== test_subsystem: ALL PASS ===\n"); return 0; }
    printf("=== test_subsystem: %d FAILURES ===\n", g_fail);
    return 1;
}
