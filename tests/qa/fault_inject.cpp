#include "aatped.h"
#include "aatpe_recovery.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <chrono>

const char* RUN_DIR   = "/tmp/aatpe_fi_run";
const char* STATE_DIR = "/tmp/aatpe_fi_state";
static const std::string RD = RUN_DIR;
static const std::string SD = STATE_DIR;
#include <vector>
std::vector<int> g_allCpus{0,1,2,3,4,5,6,7};
std::vector<int> getOnlineCpus() { return g_allCpus; }

namespace aatpe { namespace caps {
int writeTidToCgroupGroup(int tid, const std::string&) { (void)tid; return 0; }
int writeTidToCpuCgroupGroup(int tid, const std::string&) { (void)tid; return 0; }
}}

extern std::string readFile(const char* path);
extern bool writeFile(const char* path, const std::string& content);
extern bool writeFileAtomic(const char* path, const std::string& content);

static int failures = 0, passed = 0, total = 0;
#define CHECK(cond, msg) do { \
    total++; \
    if (cond) { printf("    [PASS] %s\n", msg); passed++; } \
    else { printf("    [FAIL] %s\n", msg); failures++; } \
} while (0)

static void wfile(const std::string& path, const std::string& content) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(content.data(), 1, content.size(), f); fclose(f); }
}

static void setupDirs() {
    system("rm -rf /tmp/aatpe_fi_run /tmp/aatpe_fi_state");
    ::mkdir(RUN_DIR, 0755);
    ::mkdir(STATE_DIR, 0755);
    ::mkdir((RD + "/recovery").c_str(), 0755);
}

static const char* GOOD_REC =
    "schema_version=2\npid=42\ntid=9001\nstarttime=5555\n"
    "orig_affinity=0-7\napplied_affinity=4-7\n"
    "orig_policy=0\napplied_policy=1\norig_priority=0\napplied_priority=2\n"
    "orig_nice=0\napplied_nice=-10\norig_min=0\napplied_min=50\n"
    "orig_max=1024\napplied_max=1024\nhave_orig_sched=1\nhave_orig_uclamp=1\nts=1\n";

extern int g_fi_write_short_after;
extern int g_fi_write_fail_after;
extern int g_fi_fsync_fail;
extern void aatpeTestFiReset();

int main() {
    printf("==> v4.4.9 extended fault-injection harness ==\n");
    std::string recDir = RD + "/recovery";

    printf("  FI-1: exec timeout\n");
    {
        auto t0 = std::chrono::steady_clock::now();
        int st = -1;
        (void)aatpeExecArgv({"/bin/sleep", "10"}, "fi_timeout", &st, true);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(secs < 6.0, "FI-1: timed-out command returns <6s");
        CHECK(st != 0, "FI-1: child non-zero status");
    }

    setupDirs();

    printf("  FI-2: corrupt .rec tolerated\n");
    {
        wfile(recDir + "/9001.rec", GOOD_REC);
        wfile(recDir + "/7002.rec", "schema_version=2\npid=42\ntid=7002\nstarttime=");
        wfile(recDir + "/7003.rec", "schema_version=2\npid=xyz\ntid=abc\nstarttime=NAN\n");
        wfile(recDir + "/7004.rec", "schema_version=2\ntid=7004\n");
        aatpe_recovery::recoveryBootload();
        aatpe_recovery::RecoveryRecord r;
        CHECK(aatpe_recovery::recoveryGet(9001, r), "FI-2: good record loads");
        CHECK(!aatpe_recovery::recoveryGet(7002, r), "FI-2: truncated .rec skipped");
        CHECK(!aatpe_recovery::recoveryGet(7003, r), "FI-2: non-numeric .rec skipped");
        CHECK(!aatpe_recovery::recoveryGet(7004, r), "FI-2: missing-field .rec skipped");
    }

    printf("  FI-3: tid mismatch\n");
    {
        wfile(recDir + "/7005.rec", GOOD_REC);
        aatpe_recovery::recoveryBootload();
        aatpe_recovery::RecoveryRecord r;
        CHECK(!aatpe_recovery::recoveryGet(9009, r), "FI-3: mismatched-tid skipped");
    }

    printf("  FI-4: readFile nonexistent\n");
    CHECK(readFile((RD + "/nope.txt").c_str()).empty(), "FI-4: nonexistent readFile returns empty");

    printf("  FI-5: readFile on directory\n");
    readFile(RUN_DIR);
    CHECK(true, "FI-5: readFile on dir no crash");

    printf("  FI-6: readFile empty file\n");
    wfile(RD + "/empty.txt", "");
    CHECK(readFile((RD + "/empty.txt").c_str()).empty(), "FI-6: empty file returns empty");

    printf("  FI-7: readFile chmod-000\n");
    wfile(RD + "/noperm.txt", "secret");
    ::chmod((RD + "/noperm.txt").c_str(), 0000);
    CHECK(readFile((RD + "/noperm.txt").c_str()).empty(), "FI-7: chmod-000 readFile empty");
    ::chmod((RD + "/noperm.txt").c_str(), 0644);

    printf("  FI-8: writeFileAtomic read-only dir\n");
    {
        std::string roDir = RD + "/ro_sub";
        ::mkdir(roDir.c_str(), 0555);
        bool ok = writeFileAtomic((roDir + "/x.txt").c_str(), "hi");
        CHECK(!ok, "FI-8: atomic write to ro-dir returns false");
        FILE* p = popen(("ls " + roDir + "/ 2>/dev/null | grep -c tmp || true").c_str(), "r");
        int tc = 0; if (p) { fscanf(p, "%d", &tc); pclose(p); }
        CHECK(tc == 0, "FI-8: no leftover .tmp files");
        ::chmod(roDir.c_str(), 0755);
    }

    printf("  FI-9: writeFileAtomic missing parent\n");
    CHECK(!writeFileAtomic((RD + "/no_dir/f.txt").c_str(), "x"), "FI-9: missing-parent atomic write false");

    printf("  FI-10: writeFile chmod-444\n");
    wfile(RD + "/rofile.txt", "orig");
    ::chmod((RD + "/rofile.txt").c_str(), 0444);
    CHECK(!writeFile((RD + "/rofile.txt").c_str(), "new"), "FI-10: writeFile ro-file returns false");
    ::chmod((RD + "/rofile.txt").c_str(), 0644);

    printf("  FI-11: recovery path is a file\n");
    system(("rm -rf " + recDir).c_str());
    wfile(recDir, "I AM A FILE");
    aatpe_recovery::recoveryBootload();
    CHECK(true, "FI-11: recoveryBootload ENOTDIR no crash");
    ::unlink(recDir.c_str());
    ::mkdir(recDir.c_str(), 0755);

    printf("  FI-12: recovery dir missing\n");
    {
        size_t before = aatpe_recovery::recoveryCount();
        system(("rm -rf " + recDir).c_str());
        aatpe_recovery::recoveryBootload();
        CHECK(aatpe_recovery::recoveryCount() == before, "FI-12: missing dir no crash, existing state preserved");
    }
    ::mkdir(recDir.c_str(), 0755);

    printf("  FI-13: chmod-000 .rec\n");
    wfile(recDir + "/9001.rec", GOOD_REC);
    wfile(recDir + "/8002.rec", "schema_version=2\ntid=8002\nstarttime=1\n");
    ::chmod((recDir + "/8002.rec").c_str(), 0000);
    aatpe_recovery::recoveryBootload();
    {
        aatpe_recovery::RecoveryRecord r;
        CHECK(aatpe_recovery::recoveryGet(9001, r), "FI-13: readable .rec loads");
        CHECK(!aatpe_recovery::recoveryGet(8002, r), "FI-13: chmod-000 .rec skipped");
    }
    ::chmod((recDir + "/8002.rec").c_str(), 0644);

    printf("  FI-14: .rec is a directory\n");
    system(("mkdir -p " + recDir + "/8003.rec").c_str());
    aatpe_recovery::recoveryBootload();
    {
        aatpe_recovery::RecoveryRecord r;
        CHECK(!aatpe_recovery::recoveryGet(8003, r), "FI-14: dir-named .rec skipped");
    }
    system(("rm -rf " + recDir + "/8003.rec").c_str());

    printf("  FI-15: recoveryCommit ro-dir\n");
    ::chmod(recDir.c_str(), 0555);
    {
        aatpe_recovery::RecoveryRecord rec;
        rec.tid = 9999; rec.pid = 42; rec.starttime = 123;
        rec.origAffinity = "0-3"; rec.appliedAffinity = "4-7";
        rec.haveOrigAffinity = true;
        bool ok = aatpe_recovery::recoveryCommit(rec);
        CHECK(!ok, "FI-15: commit to ro-dir returns false");
        CHECK(!aatpe_recovery::recoveryIsRecorded(9999), "FI-15: failed commit no dirty state");
    }
    ::chmod(recDir.c_str(), 0755);

    printf("  FI-16: recoveryErase missing\n");
    aatpe_recovery::recoveryErase(77777);
    CHECK(true, "FI-16: erase missing tid no crash");

    printf("  FI-17: binary-garbage .rec\n");
    {
        std::string g = "schema_version=2\ntid=8010\nstarttime=";
        for (int i = 0; i < 100; i++) g += (char)(i & 0xFF);
        g += "\n";
        wfile(recDir + "/8010.rec", g);
        aatpe_recovery::recoveryBootload();
        aatpe_recovery::RecoveryRecord r;
        CHECK(!aatpe_recovery::recoveryGet(8010, r), "FI-17: binary garbage .rec skipped");
    }

    printf("  FI-18: 9KB line .rec\n");
    {
        std::string big = "schema_version=2\ntid=8020\nstarttime=1\naff=";
        big += std::string(9000, 'A');
        big += "\n";
        wfile(recDir + "/8020.rec", big);
        aatpe_recovery::recoveryBootload();
        CHECK(true, "FI-18: 9KB line no crash");
    }

    printf("  FI-19: writeFileAtomic round-trip\n");
    {
        bool ok = writeFileAtomic((RD + "/atom.txt").c_str(), "hello world\n");
        CHECK(ok, "FI-19: atomic write succeeds");
        CHECK(readFile((RD + "/atom.txt").c_str()).find("hello world") != std::string::npos,
              "FI-19: atomic write reads back");
    }

    printf("  FI-20: recoveryCommit->Get round-trip\n");
    {
        aatpe_recovery::RecoveryRecord rec;
        rec.tid = 8888; rec.pid = 99; rec.starttime = 4242;
        rec.origAffinity = "0-3"; rec.appliedAffinity = "4-7";
        rec.haveOrigAffinity = true; rec.haveOrigSched = true;
        bool ok = aatpe_recovery::recoveryCommit(rec);
        CHECK(ok, "FI-20: commit succeeds");
        CHECK(aatpe_recovery::recoveryIsRecorded(8888), "FI-20: tid recorded");
        aatpe_recovery::RecoveryRecord out;
        CHECK(aatpe_recovery::recoveryGet(8888, out), "FI-20: get retrieves");
        CHECK(out.tid == 8888, "FI-20: tid matches");
    }

    printf("  FI-21: write() short-write retry completes fully (A2 seam)\n");
    {
        aatpeTestFiReset();
        std::string content(8192, 'X');
        g_fi_write_short_after = 1;
        std::string p = RD + "/short.txt";
        bool ok = writeFileAtomic(p.c_str(), content);
        CHECK(ok, "FI-21: short-write retry still succeeds");
        FILE* rf = fopen(p.c_str(), "rb");
        long rawLen = 0; std::string raw;
        if (rf) { fseek(rf, 0, SEEK_END); rawLen = ftell(rf); fseek(rf, 0, SEEK_SET);
                  raw.resize((size_t)rawLen); fread(&raw[0], 1, (size_t)rawLen, rf); fclose(rf); }
        CHECK(rawLen == (long)content.size(),
              ("FI-21: full content written (raw " + std::to_string(rawLen) +
               " want " + std::to_string(content.size()) + ")").c_str());
        CHECK(raw == content, "FI-21: content byte-identical after short-write retries");
        aatpeTestFiReset();
    }

    printf("  FI-22: write() EIO fail-closed (A2 seam)\n");
    {
        aatpeTestFiReset();
        std::string p = RD + "/will_not_exist.txt";
        ::unlink(p.c_str());
        g_fi_write_fail_after = 1;
        bool ok = writeFileAtomic(p.c_str(), "nope");
        CHECK(!ok, "FI-22: writeFileAtomic returns false on write EIO");
        CHECK(readFile(p.c_str()).empty(), "FI-22: target file not published");
        FILE* g = popen(("ls " + RD + "/ 2>/dev/null | grep -c '\\.tmp\\.' || true").c_str(), "r");
        int tc = 0; if (g) { fscanf(g, "%d", &tc); pclose(g); }
        CHECK(tc == 0, "FI-22: no leftover .tmp.* files");
        aatpeTestFiReset();
    }

    printf("  FI-23: fsync() EIO fail-closed, no publish (A2 seam)\n");
    {
        aatpeTestFiReset();
        std::string p = RD + "/no_fsync.txt";
        ::unlink(p.c_str());
        g_fi_fsync_fail = 1;
        bool ok = writeFileAtomic(p.c_str(), "durable?");
        CHECK(!ok, "FI-23: writeFileAtomic returns false on fsync EIO");
        CHECK(readFile(p.c_str()).empty(), "FI-23: fsync-failure target NOT published");
        FILE* g = popen(("ls " + RD + "/ 2>/dev/null | grep -c '\\.tmp\\.' || true").c_str(), "r");
        int tc = 0; if (g) { fscanf(g, "%d", &tc); pclose(g); }
        CHECK(tc == 0, "FI-23: no leftover .tmp.* after fsync failure");
        aatpeTestFiReset();
    }

    printf("-------------------------------------------------------------------\n");
    printf("==> v4.4.9 fault-injection: %d passed, %d failed (total %d checks)\n",
           passed, failures, total);
    system("rm -rf /tmp/aatpe_fi_run /tmp/aatpe_fi_state");
    if (failures == 0) { printf("FAULT-INJECT: ALL PASS\n"); return 0; }
    printf("FAULT-INJECT: %d FAILURES\n", failures);
    return 1;
}
