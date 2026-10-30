#include "aatpe_recovery.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

const char* RUN_DIR    = "/tmp/aatpe_d1_run";
const char* STATE_DIR  = "/tmp/aatpe_d1_state";

void logMsg(int, const char*, ...) {}
#include <cstring>
#include <cstdio>
const char* aatpeStrError(int err, char* buf, size_t n) {
    const char* s = strerror(err);
    if (buf && n) { snprintf(buf, n, "%s", s ? s : "unknown"); return buf; }
    return s ? s : "unknown";
}

bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fwrite(content.data(), 1, content.size(), f);
    fflush(f); fclose(f);
    if (rename(tmp.c_str(), path) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}

namespace aatpe { namespace caps {
static std::string g_lastCpusetGroup;
static std::string g_lastCpuGroup;
static int g_cpusetCalls = 0;
static int g_cpuCalls = 0;
int writeTidToCgroupGroup(int tid, const std::string& group) {
    g_cpusetCalls++; g_lastCpusetGroup = group; (void)tid; return 0;
}
int writeTidToCpuCgroupGroup(int tid, const std::string& group) {
    g_cpuCalls++; g_lastCpuGroup = group; (void)tid; return 0;
}
}}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

static void writeFile(const std::string& path, const std::string& content) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(content.data(), 1, content.size(), f); fclose(f); }
}

int main() {
    printf("==> D1 recovery: cgroup membership field + backfill + compat ==\n");
    system("rm -rf /tmp/aatpe_d1_run /tmp/aatpe_d1_state");
    ::mkdir(RUN_DIR, 0755);
    ::mkdir(STATE_DIR, 0755);

    using namespace aatpe_recovery;

    std::string recDir = std::string(RUN_DIR) + "/recovery";
    ::mkdir(recDir.c_str(), 0755);
    const char* oldRec =
        "schema_version=2\n"
        "pid=42\n"
        "tid=5001\n"
        "starttime=9999\n"
        "orig_affinity=0-7\n"
        "applied_affinity=4-7\n"
        "orig_policy=0\napplied_policy=1\n"
        "orig_priority=0\napplied_priority=2\n"
        "orig_nice=0\napplied_nice=-10\n"
        "orig_min=0\napplied_min=50\n"
        "orig_max=1024\napplied_max=1024\n"
        "have_orig_sched=1\nhave_orig_uclamp=1\n"
        "ts=1700000000\n";
    writeFile(recDir + "/5001.rec", oldRec);

    writeFile(std::string(STATE_DIR) + "/promoted_tids",
              "5001:9999:grp=foreground:2\n"
              "5002:8888:grp=background:2\n"
              "5003:7777:0-7:0\n"
              "5004:6666:-:1\n");

    writeFile(recDir + "/5002.rec", std::string(oldRec)
                  .replace(std::string(oldRec).find("tid=5001"), 9, "tid=5002")
                  .replace(0,0,""));
    writeFile(recDir + "/5002.rec",
        "schema_version=2\npid=42\ntid=5002\nstarttime=8888\n"
        "orig_affinity=0-7\napplied_affinity=4-7\n"
        "orig_policy=0\napplied_policy=1\norig_priority=0\napplied_priority=2\n"
        "orig_nice=0\napplied_nice=-10\norig_min=0\napplied_min=50\n"
        "orig_max=1024\napplied_max=1024\nhave_orig_sched=1\nhave_orig_uclamp=1\nts=1\n");

    writeFile(recDir + "/5005.rec",
        "schema_version=2\npid=42\ntid=5005\nstarttime=11111\n"
        "orig_affinity=0-7\napplied_affinity=4-7\n"
        "orig_policy=0\napplied_policy=1\norig_priority=0\napplied_priority=2\n"
        "orig_nice=0\napplied_nice=-10\norig_min=0\napplied_min=50\n"
        "orig_max=1024\napplied_max=1024\nhave_orig_sched=1\nhave_orig_uclamp=1\nts=1\n");
    {
        std::string pt;
        std::ifstream pf(std::string(STATE_DIR)+"/promoted_tids");
        std::stringstream ss; ss << pf.rdbuf();
        pt = ss.str() + "5005:5555:grp=system-background:2\n";
        writeFile(std::string(STATE_DIR)+"/promoted_tids", pt);
    }

    recoveryBootload();

    RecoveryRecord r;
    CHECK(recoveryGet(5001, r), "D1: rec 5001 loaded");
    CHECK(r.haveOrigCgroup, "D1: tid 5001 backfilled haveOrigCgroup=true");
    CHECK(r.origCgroupGroup == "foreground", "D1: tid 5001 origCgroupGroup=='foreground'");

    CHECK(recoveryGet(5002, r), "D1: rec 5002 loaded");
    CHECK(r.haveOrigCgroup && r.origCgroupGroup == "background",
          "D1: tid 5002 backfilled to 'background'");

    CHECK(recoveryGet(5005, r), "D1: rec 5005 loaded");
    CHECK(!r.haveOrigCgroup, "D1: tid 5005 starttime mismatch -> NOT backfilled (compat)");

    {
        RecoveryRecord w; recoveryGet(5001, w);
        CHECK(recoveryCommit(w), "D1: commit backfilled record");
        std::ifstream rf(recDir + "/5001.rec");
        std::string all((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
        CHECK(all.find("orig_cgroup_group=foreground") != std::string::npos,
              "D1: .rec persists orig_cgroup_group=foreground");
        CHECK(all.find("have_orig_cgroup=1") != std::string::npos,
              "D1: .rec persists have_orig_cgroup=1");
    }

    recoveryBootload();
    {
        RecoveryRecord w; recoveryGet(5001, w);
        CHECK(w.haveOrigCgroup && w.origCgroupGroup == "foreground",
              "D1: re-bootload preserves self-contained cgroup field (no dup)");
    }

    writeFile(recDir + "/6000.rec",
        "schema_version=2\npid=42\ntid=6000\nstarttime=4242\n"
        "orig_affinity=0-3\napplied_affinity=7\n"
        "orig_policy=0\napplied_policy=1\norig_priority=0\napplied_priority=2\n"
        "orig_nice=0\napplied_nice=5\norig_min=0\napplied_min=50\n"
        "orig_max=1024\napplied_max=1024\nhave_orig_sched=1\nhave_orig_uclamp=1\nts=1\n");
    recoveryBootload();
    {
        RecoveryRecord w; recoveryGet(6000, w);
        CHECK(!w.haveOrigCgroup, "D1: legacy .rec w/o promoted_tids -> haveOrigCgroup=false (old path)");
        CHECK(w.origCgroupGroup.empty(), "D1: legacy .rec origCgroupGroup empty");
    }

    printf("==> D1 recovery: count=%zu\n", recoveryCount());
    system("rm -rf /tmp/aatpe_d1_run /tmp/aatpe_d1_state");
    if (failures == 0) { printf("D1 recovery: ALL PASS\n"); return 0; }
    printf("D1 recovery: %d FAILURES\n", failures);
    return 1;
}
