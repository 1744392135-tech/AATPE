// D1 host test: .rec cgroup membership field + promoted_tids backfill + old .rec
// backward compatibility.
//
// Exercises the NEW logic in aatpe_recovery.cpp:
//   * recoveryBootload() backfills origCgroupGroup from STATE_DIR/promoted_tids
//     (field 3 == "grp=<group>") for matching tid+starttime.
//   * serialization round-trips orig_cgroup_group / have_orig_cgroup.
//   * an OLD .rec (no cgroup keys, no promoted_tids row) stays haveOrigCgroup=false
//     -> graceful fallback to the legacy restore path.
//   * a promoted_tids row with a mismatched starttime or a non-"grp=" field
//     (cpulist / "-") is NOT backfilled.
//
// The live cgroup write (caps::writeTidTo{Cpu,}CgroupGroup) requires a real
// /proc/<pid>/task/<tid> + sched_getscheduler; that path is validated on device.
// On host we link stubs for the caps writers so the TU resolves.
#include "aatpe_recovery.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

// --- host environment: redirect RUN_DIR / STATE_DIR to temp dirs -------------
const char* RUN_DIR    = "/tmp/aatpe_d1_run";
const char* STATE_DIR  = "/tmp/aatpe_d1_state";

// --- host stub logging -------------------------------------------------------
void logMsg(int, const char*, ...) {}
// Mechanical hardening: aatpe_recovery.cpp now calls aatpeStrError() (real in
// aatped_logging.cpp). Provide a hermetic fallback here.
#include <cstring>
#include <cstdio>
const char* aatpeStrError(int err, char* buf, size_t n) {
    const char* s = strerror(err);
    if (buf && n) { snprintf(buf, n, "%s", s ? s : "unknown"); return buf; }
    return s ? s : "unknown";
}

// --- host stub writeFileAtomic (tmp + rename) --------------------------------
bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fwrite(content.data(), 1, content.size(), f);
    fflush(f); fclose(f);
    if (rename(tmp.c_str(), path) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}

// --- host stubs for the caps cgroup writers (D1 restore path) ----------------
namespace aatpe { namespace caps {
static std::string g_lastCpusetGroup;
static std::string g_lastCpuGroup;
static int g_cpusetCalls = 0;
static int g_cpuCalls = 0;
int writeTidToCgroupGroup(int tid, const std::string& group) {
    g_cpusetCalls++; g_lastCpusetGroup = group; (void)tid; return 0;   // pretend success
}
int writeTidToCpuCgroupGroup(int tid, const std::string& group) {
    g_cpuCalls++; g_lastCpuGroup = group; (void)tid; return 0;
}
}}  // namespace

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

    // ---- 1. an OLD-STYLE .rec (no cgroup keys) for tid=5001, starttime=9999 ----
    // Written exactly as a v4.1.1 recovery would (no orig_cgroup_group line).
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

    // ---- 2. promoted_tids row that matches tid=5001 / starttime=9999, grp=foreground
    writeFile(std::string(STATE_DIR) + "/promoted_tids",
              "5001:9999:grp=foreground:2\n"
              "5002:8888:grp=background:2\n"        // tid 5002 has NO .rec -> ignored
              "5003:7777:0-7:0\n"                    // cpulist -> NOT a cgroup row
              "5004:6666:-:1\n");                    // "-" -> nothing to undo

    // second old .rec for tid=5002 (starttime=8888) -> SHOULD backfill to background
    writeFile(recDir + "/5002.rec", std::string(oldRec)
                  .replace(std::string(oldRec).find("tid=5001"), 9, "tid=5002")
                  .replace(0,0,""));  // placeholder, overwritten below
    writeFile(recDir + "/5002.rec",
        "schema_version=2\npid=42\ntid=5002\nstarttime=8888\n"
        "orig_affinity=0-7\napplied_affinity=4-7\n"
        "orig_policy=0\napplied_policy=1\norig_priority=0\napplied_priority=2\n"
        "orig_nice=0\napplied_nice=-10\norig_min=0\napplied_min=50\n"
        "orig_max=1024\napplied_max=1024\nhave_orig_sched=1\nhave_orig_uclamp=1\nts=1\n");

    // tid=5005: .rec exists, promoted_tids row exists BUT starttime mismatches.
    writeFile(recDir + "/5005.rec",
        "schema_version=2\npid=42\ntid=5005\nstarttime=11111\n"
        "orig_affinity=0-7\napplied_affinity=4-7\n"
        "orig_policy=0\napplied_policy=1\norig_priority=0\napplied_priority=2\n"
        "orig_nice=0\napplied_nice=-10\norig_min=0\napplied_min=50\n"
        "orig_max=1024\napplied_max=1024\nhave_orig_sched=1\nhave_orig_uclamp=1\nts=1\n");
    // add a mismatched-starttime promoted row for 5005 (starttime 5555 != 11111):
    {
        std::string pt;
        std::ifstream pf(std::string(STATE_DIR)+"/promoted_tids");
        std::stringstream ss; ss << pf.rdbuf();
        pt = ss.str() + "5005:5555:grp=system-background:2\n";
        writeFile(std::string(STATE_DIR)+"/promoted_tids", pt);
    }

    // ---- bootload: backfill should run ----
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

    // ---- 3. serialization: a fresh commit carries orig_cgroup_group ----
    // RecoveryGet returns a copy; commit it back and inspect the on-disk line.
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

    // ---- 4. round-trip: a freshly-bootloaded record with the cgroup keys present
    //        must NOT be overwritten by a later backfill (already self-contained).
    recoveryBootload();
    {
        RecoveryRecord w; recoveryGet(5001, w);
        CHECK(w.haveOrigCgroup && w.origCgroupGroup == "foreground",
              "D1: re-bootload preserves self-contained cgroup field (no dup)");
    }

    // ---- 5. legacy compatibility: a record with NO promoted_tids row at all ----
    // tid=6000: .rec exists, no promoted_tids entry -> haveOrigCgroup stays false.
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
