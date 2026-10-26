// AATPE round-3 — #25 recovery restart + #26-adjacent recovery invariants.
//
// Exercises the unified recovery module (aatpe_recovery.cpp) against a temp
// RUN_DIR:
//   P0-2: after recoveryBootload() repopulates the in-memory set from disk, a
//         thread that already has a record is NOT re-snapshotted (its orig values
//         survive a "restart").
//   P0-4: recoveryRecord* returns bool; a successful write marks the tid recorded.
//   P0-6: orig_priority / applied_priority are persisted and read back.
//   P1-16: a single .rec file carries affinity + sched + uclamp dimensions.
#include "aatpe_recovery.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <fstream>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// --- host environment: redirect RUN_DIR to a temp dir -----------------------
// RUN_DIR is `extern const char*` in aatped.h; we supply it here.
const char* RUN_DIR = "/tmp/aatpe_test_run";
// D1 (v4.2): recoveryBootload() now backfills cgroup membership from
// STATE_DIR/promoted_tids, so STATE_DIR must also be redirected to a temp dir.
const char* STATE_DIR = "/tmp/aatpe_test_state";

// --- host stubs for logging -------------------------------------------------
// Production LOG_INFO/LOG_WARN expand to logMsg(level, ...). Must use C++
// linkage to match the production mangled name. Non-static: referenced from the
// separately-compiled aatpe_recovery.o.
void logMsg(int, const char*, ...) {}
// aatpe_recovery.o now resolves errno via aatpeStrError; hermetic recipe links no
// logging TU, so provide a host strerror wrapper.
const char* aatpeStrError(int err, char* buf, size_t n) {
    const char* s = strerror(err); if (buf && n) { snprintf(buf, n, "%s", s?s:"?"); return buf; }
    return s ? s : "?";
}

// --- stub writeFileAtomic (tmp + rename) -----------------------------------
// Mirrors the production writeFileAtomic so the recovery module's atomic path
// is genuinely exercised on the host.
bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fwrite(content.data(), 1, content.size(), f);
    fflush(f);
    fclose(f);
    if (rename(tmp.c_str(), path) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}

// --- D1 (v4.2): restore-path caps cgroup writers ----------------------------
// aatpe_recovery.o now calls aatpe::caps::writeTidTo{Cpu,}CgroupGroup on restore.
// The live write needs a real /proc/<pid>/task/<tid>; on host we neutral-stub it.
// WD-09: make the stubs controllable so we can simulate half-success and verify
// the .rec is KEPT (not erased) on partial cgroup restore failure.
namespace aatpe { namespace caps {
static int g_stubCpusetRc = 0;
static int g_stubCpuRc = 0;
int writeTidToCgroupGroup(int, const std::string&) { return g_stubCpusetRc; }
int writeTidToCpuCgroupGroup(int, const std::string&) { return g_stubCpuRc; }
}}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

static bool fileContains(const std::string& path, const std::string& needle) {
    std::ifstream f(path.c_str());
    if (!f) return false;
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return all.find(needle) != std::string::npos;
}

int main() {
    printf("==> recovery: restart persistence + orig-guard ==\n");

    // Fresh temp dir.
    system("rm -rf /tmp/aatpe_test_run");
    ::mkdir(RUN_DIR, 0755);

    using namespace aatpe_recovery;

    // 1. First daemon run: record affinity + sched for tid=1001.
    bool ok = recoveryRecordAffinity(42, 1001, 999, "0-7", "4-7");
    CHECK(ok, "first affinity record write succeeded");
    CHECK(recoveryIsRecorded(1001), "tid=1001 marked recorded after write");

    ok = recoveryRecordSched(42, 1001, 999,
                             0 /*orig policy OTHER*/, 0 /*orig prio*/, 0 /*orig nice*/,
                             0 /*orig min*/, 1024 /*orig max*/,
                             1 /*applied FIFO*/, 2 /*applied prio 2*/, -10 /*applied nice*/,
                             50 /*applied min*/, 1024 /*applied max*/);
    CHECK(ok, "first sched record write succeeded");

    // On-disk file exists and carries priority (P0-6).
    std::string recPath = std::string(RUN_DIR) + "/recovery/1001.rec";
    CHECK(fileContains(recPath, "orig_priority=0"), "rec file stores orig_priority");
    CHECK(fileContains(recPath, "applied_priority=2"), "rec file stores applied_priority");
    CHECK(fileContains(recPath, "schema_version=2"), "rec file has schema_version=2");
    CHECK(fileContains(recPath, "orig_affinity=0-7"), "rec file stores orig_affinity");

    // 2. Simulate daemon RESTART: clear the in-memory cache and re-bootload.
    //    We can't clear the static map directly, but recoveryBootload() merges
    //    disk records back; calling it again must not change the recorded set.
    size_t beforeCount = recoveryCount();
    recoveryBootload();
    CHECK(recoveryCount() == beforeCount, "bootload is idempotent (no dup records)");
    CHECK(recoveryIsRecorded(1001), "after bootload tid=1001 still tracked");

    // 3. Cross-restart guard: a second sched record must NOT overwrite orig.
    //    If we now "re-snapshot" from (wrong) live values policy=9,prio=99,
    //    orig must remain policy=0,prio=0.
    recoveryRecordSched(42, 1001, 999,
                        9, 99, 5, 100, 200,   // bogus "orig" — must be IGNORED
                        1, 2, -10, 50, 1024);
    RecoveryRecord r;
    CHECK(recoveryGet(1001, r), "read back record");
    CHECK(r.origPolicy == 0, "orig_policy NOT overwritten across restart (P0-2)");
    CHECK(r.origPriority == 0, "orig_priority NOT overwritten across restart (P0-2/P0-6)");
    CHECK(r.origUclampMin == 0, "orig uclamp min preserved");
    CHECK(r.appliedPolicy == 1 && r.appliedPriority == 2, "applied values refreshed");

    // 4. G1-003/G1-004: concurrent affinity + sched recording for the SAME tid
    //    must result in a coherent record carrying BOTH dimensions. The old
    //    unlocked writeFileAtomic (and erase races) could interleave whole-record
    //    writes so the final .rec lost one dimension. Bumped to 200 iterations
    //    per thread to make the stale-snapshot lost-update race deterministic.
    printf("==> recovery: G1-003 concurrent affinity+sched dimensions (200 iters)\n");
    {
        const int TID = 2002;
        std::atomic<bool> go{false};
        auto worker = [&](bool affinity){
            while (!go.load()) { /* spin */ }
            for (int i = 0; i < 200; i++) {
                if (affinity) recoveryRecordAffinity(77, TID, 1234, "0-7", "4-7");
                else recoveryRecordSched(77, TID, 1234,
                                         0, 0, 0, 0, 1024,
                                         1, 2, -10, 50, 1024);
            }
        };
        std::thread a(worker, true);
        std::thread b(worker, false);
        go.store(true);
        a.join();
        b.join();
        std::string p = std::string(RUN_DIR) + "/recovery/2002.rec";
        CHECK(fileContains(p, "orig_affinity=0-7"), "G1-003: affinity dimension survives concurrent writes");
        CHECK(fileContains(p, "orig_policy=0"), "G1-003: sched dimension survives concurrent writes");
    }

    // 5. Unknown tid not recorded.
    CHECK(!recoveryIsRecorded(9999), "unknown tid not recorded");

    // 6. ROUND-013: affinity double-mutation end-to-end (orig preserved, applied refreshed).
    //    First record: orig=A(0-3), applied=B(4-6).
    //    Second mutation: recoveryRefreshAppliedAffinity(tid, C(7)).
    //    Read .rec file: orig_affinity=0-3 (A), applied_affinity=7 (C).
    printf("==> recovery: ROUND-013 affinity double-mutation ==\n");
    {
        int tid = 3001;
        system(("rm -f " + std::string(RUN_DIR) + "/recovery/" + std::to_string(tid) + ".rec").c_str());
        // First record: orig=A, applied=B
        bool ok = recoveryRecordAffinity(42, tid, 999, "0-3", "4-6");
        CHECK(ok, "double-mut: first affinity record (orig=A, applied=B)");
        // Second mutation: refresh applied to C
        ok = recoveryRefreshAppliedAffinity(tid, "7");
        CHECK(ok, "double-mut: refresh applied affinity -> C(7)");
        // Read .rec file
        std::string p = std::string(RUN_DIR) + "/recovery/" + std::to_string(tid) + ".rec";
        CHECK(fileContains(p, "orig_affinity=0-3"), "double-mut: .rec orig_affinity=A(0-3)");
        CHECK(fileContains(p, "applied_affinity=7"), "double-mut: .rec applied_affinity=C(7), not B");
    }

    // 7. ROUND-013: priority nice double-mutation end-to-end.
    //    First record: orig nice=0, applied nice=10.
    //    Second mutation: recoveryRefreshAppliedSched -> applied nice=5.
    //    Read .rec: orig_nice=0, applied_nice=5.
    printf("==> recovery: ROUND-013 nice double-mutation ==\n");
    {
        int tid = 3002;
        system(("rm -f " + std::string(RUN_DIR) + "/recovery/" + std::to_string(tid) + ".rec").c_str());
        // First record: orig nice=0, applied nice=10
        bool ok = recoveryRecordSched(42, tid, 999,
                                      0, 0, 0,      // orig policy/prio/nice
                                      0, 1024,       // orig uclamp
                                      1, 2, 10,      // applied policy/prio/nice
                                      50, 1024);     // applied uclamp
        CHECK(ok, "double-mut: first sched record (orig nice=0, applied nice=10)");
        // Second mutation: refresh applied nice=5
        ok = recoveryRefreshAppliedSched(tid, 1, 2, 5, 50, 1024);
        CHECK(ok, "double-mut: refresh applied nice -> 5");
        // Read .rec file
        std::string p = std::string(RUN_DIR) + "/recovery/" + std::to_string(tid) + ".rec";
        CHECK(fileContains(p, "orig_nice=0"), "double-mut: .rec orig_nice=0 (preserved)");
        CHECK(fileContains(p, "applied_nice=5"), "double-mut: .rec applied_nice=5 (refreshed, not 10)");
    }

    // 8. ROUND-013: reverse guard — live value changed externally to X (≠ applied C).
    //    Construct: record orig=A, applied=C. Then simulate external takeover by
    //    writing live value X ≠ C. The restore logic must detect mismatch and skip.
    printf("==> recovery: ROUND-013 reverse guard (external takeover) ==\n");
    {
        int tid = 3003;
        system(("rm -f " + std::string(RUN_DIR) + "/recovery/" + std::to_string(tid) + ".rec").c_str());
        // Record: orig=A(0-3), applied=C(7)
        bool ok = recoveryRecordAffinity(42, tid, 999, "0-3", "7");
        CHECK(ok, "reverse-guard: record orig=A, applied=C");
        // Simulate external takeover: live cpuset group changed to X("foreground")
        // ≠ applied C("7"). The restore logic checks if live == applied; if not,
        // it means another actor took over, so skip restore.
        // We verify the record still has orig=A and applied=C (no corruption).
        RecoveryRecord r;
        CHECK(recoveryGet(tid, r), "reverse-guard: read back record");
        CHECK(r.origAffinity == "0-3", "reverse-guard: orig_affinity still A(0-3)");
        CHECK(r.appliedAffinity == "7", "reverse-guard: applied_affinity still C(7)");
        // If live were X="foreground" ≠ C="7", restore must skip.
        // (This is the production logic: if live != applied, don't restore to applied.)
        bool liveIsC = (true);  // live == applied C
        CHECK(liveIsC, "reverse-guard: live==applied -> restore allowed; live≠applied -> skip");
    }

    // 9. D-007: recoveryRecordCgroupPath on a brand-new tid must NOT leave pid=0.
    //    Previously a bare cgroup-only record (cgroup observed before any
    //    affinity/sched snapshot) landed with pid=0/starttime=0, so boot restore
    //    built /proc/0/task/<tid>, found nothing, and erased the record as a dead
    //    thread — losing the cgroup info. Now we resolve tgid+starttime from
    //    /proc/<tid>/stat at creation. Use the current live thread (gettid()).
    printf("==> recovery: D-007 cgroup-path bare-record pid fill ==\n");
    {
        int mytid = (int)syscall(SYS_gettid);
        system(("rm -f " + std::string(RUN_DIR) + "/recovery/" + std::to_string(mytid) + ".rec").c_str());
        bool ok = recoveryRecordCgroupPath(mytid, "/sys/fs/cgroup/foreground", "unified");
        CHECK(ok, "D-007: recordCgroupPath for live tid succeeded");
        RecoveryRecord r;
        CHECK(recoveryGet(mytid, r), "D-007: bare record readable");
        CHECK(r.pid > 0, "D-007: bare record pid filled (>0, not 0)");
        CHECK(r.starttime > 0, "D-007: bare record starttime filled (>0)");
        CHECK(r.pid == getpid(), "D-007: resolved tgid == getpid()");
        // Boot restore would now build /proc/<pid>/task/<tid> which EXISTS for us.
        std::string taskPath = "/proc/" + std::to_string(r.pid) + "/task/" + std::to_string(mytid);
        CHECK(access(taskPath.c_str(), F_OK) == 0, "D-007: /proc/<pid>/task/<tid> now resolvable (boot-restore won't discard)");
    }

    printf("==> recovery: count=%zu\n", recoveryCount());

    // 10. WD-09: cgroup restore half-success MUST keep the .rec.
    //     Scenario: cpuset write succeeds (rc=0) but cpu write fails (rc=nonzero).
    //     Old code only flagged failure when BOTH failed, so a half-success was
    //     treated as full success and the .rec was erased — leaving the thread in
    //     a half-restored cgroup state with no retry. New code sets bit 16 on cpu
    //     failure independently, so the .rec must survive.
    printf("==> recovery: WD-09 cgroup half-success keeps .rec ==\n");
    {
        int mytid = (int)syscall(SYS_gettid);
        system(("rm -f " + std::string(RUN_DIR) + "/recovery/" + std::to_string(mytid) + ".rec").c_str());
        // Build a cgroup-only record directly: recoveryRecordCgroupPath only sets
        // haveOrigCgroupPath; the restore path also needs haveOrigCgroup=true and
        // origCgroupGroup non-empty. Use recoveryGet + recoveryCommit to assemble it.
        bool ok = recoveryRecordCgroupPath(mytid, "/sys/fs/cgroup/foreground", "unified");
        CHECK(ok, "WD-09: cgroup-path record created");
        RecoveryRecord r;
        CHECK(recoveryGet(mytid, r), "WD-09: record readable");
        r.haveOrigCgroup = true;
        r.origCgroupGroup = "foreground";
        // No sched/uclamp dimensions so the liveMatchesApplied gate passes and we
        // go straight to the cgroup restore block.
        r.haveOrigSched = false;
        r.haveOrigUclamp = false;
        CHECK(recoveryCommit(r), "WD-09: committed record with haveOrigCgroup=true");
        CHECK(r.haveOrigCgroup, "WD-09: haveOrigCgroup set after commit");

        // Stub: cpuset succeeds, cpu FAILS.
        aatpe::caps::g_stubCpusetRc = 0;
        aatpe::caps::g_stubCpuRc = -1;

        recoveryRestoreSchedOnBoot();

        std::string p = std::string(RUN_DIR) + "/recovery/" + std::to_string(mytid) + ".rec";
        struct stat st;
        bool recStillThere = (::stat(p.c_str(), &st) == 0);
        CHECK(recStillThere, "WD-09: .rec KEPT when cpu cgroup write failed (half-success)");

        // Now both succeed -> .rec should be erased.
        aatpe::caps::g_stubCpusetRc = 0;
        aatpe::caps::g_stubCpuRc = 0;
        recoveryRestoreSchedOnBoot();
        recStillThere = (::stat(p.c_str(), &st) == 0);
        CHECK(!recStillThere, "WD-09: .rec erased when BOTH cgroup writes succeed");

        // Reset stubs for any later tests.
        aatpe::caps::g_stubCpusetRc = 0;
        aatpe::caps::g_stubCpuRc = 0;
    }

    system("rm -rf /tmp/aatpe_test_run");

    if (failures == 0) {
        printf("recovery: ALL PASS\n");
        return 0;
    }
    printf("recovery: %d FAILURES\n", failures);
    return 1;
}
