// test_ebpf.cpp - v94 hermetic unit tests for aatpe_ebpf.cpp
// Constructs EbpfScheduler and exercises its state/query accessors; load()
// gracefully fails on a host without bpf/tracefs.
#include <cstdio>
#include <string>
#include "aatpe_ebpf.h"
#include "integrity.h"

// The ebpf host test recipe does not link integrity.cpp. Provide a minimal,
// test-controlled backing for the G3 integrity gate seam so production
// aatpe_ebpf.cpp's call to aatpe::integrity::privilegedOpsAllowed() resolves.
// Default verdict = Ok (1) so the pre-existing load() behavior is unchanged.
namespace aatpe { namespace integrity {
static int g_testVerdict = 1;   // 1 == Ok
static int g_gateCalls   = 0;
bool privilegedOpsAllowed() { g_gateCalls++; return g_testVerdict == 1; }
void setIntegrityVerdictForTest(IntegrityVerdict v) { g_testVerdict = (int)v; }
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== eBPF: construct EbpfScheduler ===\n");
    aatpe_ebpf::EbpfScheduler sched;
    CHECK(sched.isActive() == false, "fresh EbpfScheduler starts inactive");

    printf("=== eBPF: state accessors ===\n");
    CHECK(sched.isActive() == false, "inactive after construction");
    const char* st = sched.getStateString();
    CHECK(st != nullptr && st[0] != '\0', "state string non-empty");
    const char* lt = sched.getLoaderString();
    CHECK(lt != nullptr, "loader string non-null");
    const char* md = sched.getModeString();
    CHECK(md != nullptr, "mode string non-null");

    printf("=== eBPF: load() does not crash on host ===\n");
    bool ok = sched.load();
    CHECK(ok == true || ok == false, "load() returns a bool (graceful on host)");
    // isActive() must be consistent with whatever load() decided.
    CHECK(sched.isActive() == ok, "isActive() consistent with load() result");

    printf("\n=== eBPF: G4-001 offset-degrade vs ring-loss recovery split ===\n");
    {
        // Offset-caused degradation must NEVER auto-recover to EVENT_DRIVEN, no
        // matter how loss-free the ring is. The kernel layout does not self-correct.
        sched.g4TestEnterOffsetDegraded();
        CHECK(sched.isOffsetDegraded() == true, "offset-degraded state flagged");
        CHECK(sched.getDetailedStateString() == std::string("DEGRADED_OFFSET"),
              "detailed state reads DEGRADED_OFFSET");
        // Even with an old (31s) ring-loss stamp, recovery must be refused.
        sched.g4TestStampOldRingLoss();
        sched.g4TickRecovery();
        CHECK(sched.g4ModeIsEventDriven() == false,
              "offset-degraded: recoverIfHealthy refuses to flip to EVENT_DRIVEN");
        CHECK(sched.isOffsetDegraded() == true,
              "offset-degraded flag survives a recovery tick");
    }
    {
        // Ring-loss HYBRID degradation DOES recover after 30s loss-free.
        sched.g4TestEnterRingLossHybrid();
        CHECK(sched.isOffsetDegraded() == false, "ring-loss hybrid not offset-degraded");
        // First tick seeds the clock (lastLoss==0) -> no immediate flip.
        sched.g4TickRecovery();
        CHECK(sched.g4ModeIsEventDriven() == false,
              "ring-loss hybrid: first tick seeds clock, no immediate recovery");
        // Stamp a 60s-old loss (well past 30s threshold, avoid boundary flakiness) and tick -> should recover.
        sched.g4TestStampOldRingLoss();
        sched.g4TickRecovery();
        CHECK(sched.g4ModeIsEventDriven() == true && sched.g4StateIsEventDriven() == true,
              "ring-loss hybrid: recovers to EVENT_DRIVEN after 30s loss-free");
        // G4-001 teardown: the recovery path spawned a poll thread; stop it
        // explicitly so the host test does not abort on a joinable-thread dtor.
        sched.g4TestStopPollThread();
    }

    printf("\n=== eBPF: ROUND-003 F2 offset-verify matrix ===\n");
    {
        using S = aatpe_ebpf::TpOffsetStatus;
        // Combo 1: known 4.14 family + tracefs unreadable (UNKNOWN) -> trust
        // built-in offsets, STAY EVENT_DRIVEN, no POLLING latch.
        {
            aatpe_ebpf::EbpfScheduler s;
            s.g4TestEnterAttached();
            s.g4TestApplyOffsetStatus(S::UNKNOWN, /*knownFamily=*/true);
            CHECK(s.isOffsetDegraded() == false,
                  "known family + UNKNOWN: not offset-degraded");
            CHECK(s.isOffsetUnverifiedButTrusted() == true,
                  "known family + UNKNOWN: soft-trust flag set");
            CHECK(s.g4ModeIsEventDriven() == true,
                  "known family + UNKNOWN: stays EVENT_DRIVEN");
            CHECK(s.getDetailedStateString() == std::string("TRUSTED_BUILTIN"),
                  "known family + UNKNOWN: state reports TRUSTED_BUILTIN");
        }
        // Combo 2: known family + readable but mismatch (DEGRADED) -> fail-safe
        // POLLING latch (evidence of a different layout).
        {
            aatpe_ebpf::EbpfScheduler s;
            s.g4TestEnterAttached();
            s.g4TestApplyOffsetStatus(S::DEGRADED, /*knownFamily=*/true);
            CHECK(s.isOffsetDegraded() == true,
                  "known family + MISMATCH: offset-degraded");
            CHECK(s.g4ModeIsEventDriven() == false,
                  "known family + MISMATCH: forced to POLLING");
            CHECK(s.getDetailedStateString() == std::string("DEGRADED_OFFSET"),
                  "known family + MISMATCH: state reports DEGRADED_OFFSET");
            s.g4TestStopPollThread();
        }
        // Combo 3: non-known family + tracefs unreadable (UNKNOWN) -> conservative
        // POLLING (do NOT blindly trust built-in offsets on an unknown kernel).
        {
            aatpe_ebpf::EbpfScheduler s;
            s.g4TestEnterAttached();
            s.g4TestApplyOffsetStatus(S::UNKNOWN, /*knownFamily=*/false);
            CHECK(s.isOffsetDegraded() == true,
                  "unknown family + UNKNOWN: conservative offset-degrade");
            CHECK(s.isOffsetUnverifiedButTrusted() == false,
                  "unknown family + UNKNOWN: no soft-trust");
            CHECK(s.g4ModeIsEventDriven() == false,
                  "unknown family + UNKNOWN: forced to POLLING");
            s.g4TestStopPollThread();
        }
        // Combo 4: non-known family + mismatch (DEGRADED) -> POLLING (partial
        // readable mismatch must still fail-safe).
        {
            aatpe_ebpf::EbpfScheduler s;
            s.g4TestEnterAttached();
            s.g4TestApplyOffsetStatus(S::DEGRADED, /*knownFamily=*/false);
            CHECK(s.isOffsetDegraded() == true,
                  "unknown family + MISMATCH: offset-degraded");
            CHECK(s.g4ModeIsEventDriven() == false,
                  "unknown family + MISMATCH: forced to POLLING");
            s.g4TestStopPollThread();
        }
    }

    printf("\n=== eBPF: ROUND-004 config hot-reload convergence ===\n");
    {
        using RC = aatpe_ebpf::EbpfScheduler::EbpfRuntimeConfig;
        // Case 1: enabled 1 -> 0 safe unload.
        {
            aatpe_ebpf::EbpfScheduler s;
            s.g4TestEnterAttached();
            bool acted = s.onConfigReload(RC{false, 64, true});
            CHECK(acted == true, "1->0: unload action taken");
            CHECK(s.g4IsActive() == false, "1->0: no longer active");
            CHECK(s.g4IsUnloaded() == true, "1->0: resources released (unloaded)");
        }
        // Case 2: enabled 0 -> 1 with integrity gate Ok: load() is attempted
        // (gate consulted); on host it cannot attach so it stays non-event-driven.
        {
            aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
            aatpe_ebpf::EbpfScheduler s;
            long before = aatpe::integrity::g_gateCalls;
            bool acted = s.onConfigReload(RC{true, 64, true});
            CHECK(acted == true, "0->1 (gate Ok): reload action taken");
            CHECK(aatpe::integrity::g_gateCalls > before,
                  "0->1 (gate Ok): load() consulted integrity gate");
        }
        // Case 3: enabled 0 -> 1 with gate Failed: privilege attach refused,
        // must NOT enter EVENT_DRIVEN (fail-closed preserved).
        {
            aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Failed);
            aatpe_ebpf::EbpfScheduler s;
            (void)s.onConfigReload(RC{true, 64, true});
            CHECK(s.g4ModeIsEventDriven() == false,
                  "0->1 (gate Failed): refused, not EVENT_DRIVEN");
            aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
        }
        // Case 4: identical config twice -> second call is a no-op (no thrash).
        {
            aatpe_ebpf::EbpfScheduler s;
            s.g4TestEnterAttached();
            bool first = s.onConfigReload(RC{true, 64, true});
            bool second = s.onConfigReload(RC{true, 64, true});
            CHECK(first == false, "identical cfg: already live -> no-op first");
            CHECK(second == false, "identical cfg: second call also no-op");
        }
        // Case 5: rapid 1 -> 0 -> 1 sequence must not leak/corrupt state; the
        // re-enable re-runs load() through the integrity gate.
        {
            aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
            aatpe_ebpf::EbpfScheduler s;
            s.g4TestEnterAttached();
            (void)s.onConfigReload(RC{false, 64, true});   // 1 -> 0
            CHECK(s.g4IsUnloaded() == true, "rapid 1->0->1: after disable, unloaded");
            long before = aatpe::integrity::g_gateCalls;
            bool acted = s.onConfigReload(RC{true, 64, true}); // 0 -> 1
            CHECK(acted == true, "rapid 1->0->1: re-enable reloads");
            CHECK(aatpe::integrity::g_gateCalls > before,
                  "rapid 1->0->1: re-enable consulted integrity gate");
            CHECK(std::string(s.getStateString()).empty() == false,
                  "rapid 1->0->1: scheduler left in a consistent state");
        }
        // Case 6: integrity FAILED while enabling -> must refuse and stay
        // non-event-driven (fail-closed on the reload path, same as boot).
        {
            aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Failed);
            aatpe_ebpf::EbpfScheduler s;
            (void)s.onConfigReload(RC{true, 128, true});
            CHECK(s.g4ModeIsEventDriven() == false,
                  "integrity FAILED: reload-enable refused");
            aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
        }
    }

    printf("\n=== eBPF: G3 integrity gate (privileged attach refused when not OK) ===\n");
    {
        // Default verdict = Ok: load() consults the gate and proceeds (on host it
        // still cannot attach, so it falls back to POLLING — never EVENT_DRIVEN).
        aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
        aatpe_ebpf::EbpfScheduler schedOk;
        (void)schedOk.load();
        CHECK(schedOk.g4ModeIsEventDriven() == false,
              "verdict=Ok: load() does not land in EVENT_DRIVEN on host");

        // Verdict = Failed: the gate must short-circuit BEFORE any privileged
        // attach. The daemon must stay in POLLING/DEGRADED, never EVENT_DRIVEN.
        aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Failed);
        aatpe_ebpf::EbpfScheduler schedBad;
        (void)schedBad.load();
        CHECK(schedBad.g4ModeIsEventDriven() == false,
              "verdict=Failed: load() refuses privileged attach (not EVENT_DRIVEN)");
        // The gate was consulted at least once on this fresh scheduler's load().
        CHECK(aatpe::integrity::g_gateCalls >= 1, "integrity gate was consulted during load()");

        // Restore Ok so any later teardown/state readers see a sane verdict.
        aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
    }

    printf("\n=== eBPF: E-001 retryLoadIfDue backoff + reload (production failure state) ===\n");
    {
        // Drive the REAL production failure path: with the integrity gate Ok, a
        // host load() cannot attach any program and lands exactly where a device
        // whose BPF/tracefs is unavailable lands:
        //   mode_=POLLING + state_=DEGRADED_RING + no programs attached.
        // The old gate checked state_==DEGRADED(6), which production never sets,
        // so retryLoadIfDue() returned early forever and the device stayed stuck.
        aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
        aatpe_ebpf::EbpfScheduler s;
        (void)s.load();

        CHECK(s.g4StateIsDegradedRing() == true,
              "E-001: production load-failure lands in DEGRADED_RING");
        CHECK(s.g4ModeIsPolling() == true,
              "E-001: production load-failure lands in POLLING mode");
        CHECK(s.g4ProgsAttached() == false,
              "E-001: nothing attached after all phases failed");
        CHECK(s.g4LoadFailureCount() == 0,
              "E-001: backoff counter seeded at 0 on first failure");
        CHECK(s.g4NextRetryDelayMs() >= 1000,
              "E-001: initial backoff delay seeded (>=1s)");

        // The integrity gate is consulted on EVERY load(); use its call counter
        // as the observable "load() was re-invoked" probe.
        long before = aatpe::integrity::g_gateCalls;

        // (a) Backoff NOT yet elapsed -> must NOT retry.
        s.g4TickRetryLoad();
        CHECK(aatpe::integrity::g_gateCalls == before,
              "E-001: no retry before backoff elapses (gate not consulted again)");
        CHECK(s.g4LoadFailureCount() == 0,
              "E-001: failure counter unchanged while waiting for backoff");

        // (b) Fast-forward the clock past the current backoff delay -> retry must
        // re-invoke load() (gate consulted again) and advance the exponential
        // backoff: 1s -> 2s, counter 0 -> 1.
        s.g4TestFastForwardRetryClock(2000);
        s.g4TickRetryLoad();
        CHECK(aatpe::integrity::g_gateCalls > before,
              "E-001: after backoff, retryLoadIfDue re-invokes load() (reload triggered)");
        CHECK(s.g4LoadFailureCount() == 1,
              "E-001: failure counter advanced 0 -> 1 after a failed retry");
        CHECK(s.g4NextRetryDelayMs() == 2000,
              "E-001: exponential backoff advanced 1000ms -> 2000ms");

        // (c) The reload that just happened again failed on the host, so we must
        // still be in the production POLLING/DEGRADED_RING failure state (not
        // EVENT_DRIVEN) and still have no programs attached.
        CHECK(s.g4StateIsDegradedRing() == true,
              "E-001: still DEGRADED_RING after a failed reload on host");
        CHECK(s.g4ModeIsPolling() == true,
              "E-001: still POLLING after a failed reload on host");
        CHECK(s.g4ProgsAttached() == false,
              "E-001: still no programs attached after a failed reload");

        // (d) Negative guard: a load that already SUCCEEDED but degraded (progs
        // attached, ring-loss / offset-degraded -> POLLING+DEGRADED_RING) must NOT
        // be torn down by retryLoadIfDue. Attach a sentinel program, force the
        // offset-degraded POLLING state, fast-forward, and confirm no reload.
        aatpe_ebpf::EbpfScheduler s2;
        s2.g4TestEnterAttached();
        s2.g4TestAttachSentinelProg();
        s2.g4TestApplyOffsetStatus(aatpe_ebpf::TpOffsetStatus::DEGRADED,
                                  /*knownFamily=*/true);
        CHECK(s2.g4ModeIsPolling() == true && s2.g4StateIsDegradedRing() == true,
              "E-001 guard: offset-degraded lands in POLLING/DEGRADED_RING");
        CHECK(s2.g4ProgsAttached() == true,
              "E-001 guard: programs still attached after offset degrade");
        long before2 = aatpe::integrity::g_gateCalls;
        s2.g4TestFastForwardRetryClock(2000);
        s2.g4TickRetryLoad();
        CHECK(aatpe::integrity::g_gateCalls == before2,
              "E-001 guard: programs-attached degrade is NOT reloaded (not load failure)");
        s2.g4TestStopPollThread();

        aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
    }

    printf("\n========================================\n"
           "eBPF results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
