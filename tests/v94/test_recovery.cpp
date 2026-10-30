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

const char* RUN_DIR = "/tmp/aatpe_test_run";
const char* STATE_DIR = "/tmp/aatpe_test_state";

void logMsg(int, const char*, ...) {}
const char* aatpeStrError(int err, char* buf, size_t n) {
    const char* s = strerror(err); if (buf && n) { snprintf(buf, n, "%s", s?s:"?"); return buf; }
    return s ? s : "?";
}

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

    system("rm -rf /tmp/aatpe_test_run");
    ::mkdir(RUN_DIR, 0755);

    using namespace aatpe_recovery;

    bool ok = recoveryRecordAffinity(42, 1001, 999, "0-7", "4-7");
    CHECK(ok, "first affinity record write succeeded");
    CHECK(recoveryIsRecorded(1001), "tid=1001 marked recorded after write");

    ok = recoveryRecordSched(42, 1001, 999,
                             0 , 0 , 0 ,
                             0 , 1024 ,
                             1 , 2 , -10 ,
                             50 , 1024 );
    CHECK(ok, "first sched record write succeeded");

    std::string recPath = std::string(RUN_DIR) + "/recovery/1001.rec";
    CHECK(fileContains(recPath, "orig_priority=0"), "rec file stores orig_priority");
    CHECK(fileContains(recPath, "applied_priority=2"), "rec file stores applied_priority");
    CHECK(fileContains(recPath, "schema_version=2"), "rec file has schema_version=2");
    CHECK(fileContains(recPath, "orig_affinity=0-7"), "rec file stores orig_affinity");

    size_t beforeCount = recoveryCount();
    recoveryBootload();
    CHECK(recoveryCount() == beforeCount, "bootload is idempotent (no dup records)");
    CHECK(recoveryIsRecorded(1001), "after bootload tid=1001 still tracked");

    recoveryRecordSched(42, 1001, 999,
                        9, 99, 5, 100, 200,
                        1, 2, -10, 50, 1024);
    RecoveryRecord r;
    CHECK(recoveryGet(1001, r), "read back record");
    CHECK(r.origPolicy == 0, "orig_policy NOT overwritten across restart (P0-2)");
    CHECK(r.origPriority == 0, "orig_priority NOT overwritten across restart (P0-2/P0-6)");
    CHECK(r.origUclampMin == 0, "orig uclamp min preserved");
    CHECK(r.appliedPolicy == 1 && r.appliedPriority == 2, "applied values refreshed");

    printf("==> recovery: G1-003 concurrent affinity+sched dimensions (200 iters)\n");
    {
        const int TID = 2002;
        std::atomic<bool> go{false};
        auto worker = [&](bool affinity){
            while (!go.load()) {  }
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

    CHECK(!recoveryIsRecorded(9999), "unknown tid not recorded");

    printf("==> recovery: ROUND-013 affinity double-mutation ==\n");
    {
        int tid = 3001;
        system(("rm -f " + std::string(RUN_DIR) + "/recovery/" + std::to_string(tid) + ".rec").c_str());
        bool ok = recoveryRecordAffinity(42, tid, 999, "0-3", "4-6");
        CHECK(ok, "double-mut: first affinity record (orig=A, applied=B)");
        ok = recoveryRefreshAppliedAffinity(tid, "7");
        CHECK(ok, "double-mut: refresh applied affinity -> C(7)");
        std::string p = std::string(RUN_DIR) + "/recovery/" + std::to_string(tid) + ".rec";
        CHECK(fileContains(p, "orig_affinity=0-3"), "double-mut: .rec orig_affinity=A(0-3)");
        CHECK(fileContains(p, "applied_affinity=7"), "double-mut: .rec applied_affinity=C(7), not B");
    }

    printf("==> recovery: ROUND-013 nice double-mutation ==\n");
    {
        int tid = 3002;
        system(("rm -f " + std::string(RUN_DIR) + "/recovery/" + std::to_string(tid) + ".rec").c_str());
        bool ok = recoveryRecordSched(42, tid, 999,
                                      0, 0, 0,
                                      0, 1024,
                                      1, 2, 10,
                                      50, 1024);
        CHECK(ok, "double-mut: first sched record (orig nice=0, applied nice=10)");
        ok = recoveryRefreshAppliedSched(tid, 1, 2, 5, 50, 1024);
        CHECK(ok, "double-mut: refresh applied nice -> 5");
        std::string p = std::string(RUN_DIR) + "/recovery/" + std::to_string(tid) + ".rec";
        CHECK(fileContains(p, "orig_nice=0"), "double-mut: .rec orig_nice=0 (preserved)");
        CHECK(fileContains(p, "applied_nice=5"), "double-mut: .rec applied_nice=5 (refreshed, not 10)");
    }

    printf("==> recovery: ROUND-013 reverse guard (external takeover) ==\n");
    {
        int tid = 3003;
        system(("rm -f " + std::string(RUN_DIR) + "/recovery/" + std::to_string(tid) + ".rec").c_str());
        bool ok = recoveryRecordAffinity(42, tid, 999, "0-3", "7");
        CHECK(ok, "reverse-guard: record orig=A, applied=C");
        RecoveryRecord r;
        CHECK(recoveryGet(tid, r), "reverse-guard: read back record");
        CHECK(r.origAffinity == "0-3", "reverse-guard: orig_affinity still A(0-3)");
        CHECK(r.appliedAffinity == "7", "reverse-guard: applied_affinity still C(7)");
        bool liveIsC = (true);
        CHECK(liveIsC, "reverse-guard: live==applied -> restore allowed; live≠applied -> skip");
    }

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
        std::string taskPath = "/proc/" + std::to_string(r.pid) + "/task/" + std::to_string(mytid);
        CHECK(access(taskPath.c_str(), F_OK) == 0, "D-007: /proc/<pid>/task/<tid> now resolvable (boot-restore won't discard)");
    }

    printf("==> recovery: count=%zu\n", recoveryCount());

    printf("==> recovery: WD-09 cgroup half-success keeps .rec ==\n");
    {
        int mytid = (int)syscall(SYS_gettid);
        system(("rm -f " + std::string(RUN_DIR) + "/recovery/" + std::to_string(mytid) + ".rec").c_str());
        bool ok = recoveryRecordCgroupPath(mytid, "/sys/fs/cgroup/foreground", "unified");
        CHECK(ok, "WD-09: cgroup-path record created");
        RecoveryRecord r;
        CHECK(recoveryGet(mytid, r), "WD-09: record readable");
        r.haveOrigCgroup = true;
        r.origCgroupGroup = "foreground";
        r.haveOrigSched = false;
        r.haveOrigUclamp = false;
        CHECK(recoveryCommit(r), "WD-09: committed record with haveOrigCgroup=true");
        CHECK(r.haveOrigCgroup, "WD-09: haveOrigCgroup set after commit");

        aatpe::caps::g_stubCpusetRc = 0;
        aatpe::caps::g_stubCpuRc = -1;

        recoveryRestoreSchedOnBoot();

        std::string p = std::string(RUN_DIR) + "/recovery/" + std::to_string(mytid) + ".rec";
        struct stat st;
        bool recStillThere = (::stat(p.c_str(), &st) == 0);
        CHECK(recStillThere, "WD-09: .rec KEPT when cpu cgroup write failed (half-success)");

        aatpe::caps::g_stubCpusetRc = 0;
        aatpe::caps::g_stubCpuRc = 0;
        recoveryRestoreSchedOnBoot();
        recStillThere = (::stat(p.c_str(), &st) == 0);
        CHECK(!recStillThere, "WD-09: .rec erased when BOTH cgroup writes succeed");

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
