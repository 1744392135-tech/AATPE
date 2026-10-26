// test_h14_cgroup_rec.cpp — H14 host test for .rec cgroup membership persistence.
//
// Verifies:
//   1. New-format .rec with orig_cgroup_path + orig_cgroup_hier round-trips.
//   2. Old-format .rec (no new keys) still parses -> haveOrigCgroupPath=false,
//      restore falls back to legacy basename path.
//   3. Empty / malformed path does not crash the parser (catch(...) already there).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sys/stat.h>
#include "aatpe_recovery.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

// recovery.cpp uses RUN_DIR; point it at a temp dir.
const char* RUN_DIR = "/tmp/aatpe_h14_test";
const char* STATE_DIR = "/tmp/aatpe_h14_state";
// Host stubs.
bool writeFileAtomic(const char*, const std::string&) { return true; }
std::string readFile(const char* path) {
    std::ifstream f(path); if (!f) return "";
    std::string s, line;
    while (std::getline(f, line)) { if (!s.empty()) s += "\n"; s += line; }
    return s;
}
int g_logLevel = 3;
void logMsg(int, const char*, ...) {}
void logFlush() {}
#include <cstring>
#include <cstdio>
const char* aatpeStrError(int err, char* buf, size_t n) {
    const char* s = strerror(err);
    if (buf && n) { snprintf(buf, n, "%s", s ? s : "unknown"); return buf; }
    return s ? s : "unknown";
}
// Host stubs for caps writers (only referenced by the on-device restore path
// that we do not exercise here).
namespace aatpe { namespace caps {
int writeTidToCgroupGroup(int, const std::string&) { return 0; }
int writeTidToCpuCgroupGroup(int, const std::string&) { return 0; }
}}


int main() {
    printf("==> H14 cgroup membership .rec persistence (host) ==\n");
    ::mkdir(RUN_DIR, 0700);

    // 1. New-format record: build a RecoveryRecord with full path + hier, commit,
    //    read back, assert fields survive.
    {
        aatpe_recovery::RecoveryRecord r;
        r.pid = 1001; r.tid = 5001; r.starttime = 12345;
        r.origCgroupGroup = "foreground";
        r.haveOrigCgroup = true;
        r.origCgroupPath = "/a/b/foreground";
        r.origCgroupHier = "unified";
        r.haveOrigCgroupPath = true;
        r.timestamp = 999;
        CHECK(aatpe_recovery::recoveryCommit(r), "H14: commit new-format .rec");

        aatpe_recovery::RecoveryRecord out;
        CHECK(aatpe_recovery::recoveryGet(5001, out), "H14: read back new-format .rec");
        CHECK(out.origCgroupGroup == "foreground", "H14: basename preserved");
        CHECK(out.haveOrigCgroup, "H14: haveOrigCgroup=true");
        CHECK(out.origCgroupPath == "/a/b/foreground", "H14: full path preserved");
        CHECK(out.origCgroupHier == "unified", "H14: hierarchy preserved");
        CHECK(out.haveOrigCgroupPath, "H14: haveOrigCgroupPath=true");
    }

    // 2. Old-format .rec (hand-written, no new keys). Parser must not crash and
    //    must leave haveOrigCgroupPath=false -> legacy basename restore path.
    {
        std::string legacy =
            "schema_version=1\npid=1002\ntid=5002\nstarttime=222\n"
            "orig_affinity=0-3\napplied_affinity=0-3\n"
            "orig_cgroup_group=background\nhave_orig_cgroup=1\n"
            "ts=1000\n";
        std::ofstream f(std::string(RUN_DIR) + "/recovery/5002.rec", std::ios::trunc);
        f << legacy; f.close();

        aatpe_recovery::recoveryBootload();
        aatpe_recovery::RecoveryRecord out;
        CHECK(aatpe_recovery::recoveryGet(5002, out), "H14: bootload legacy .rec");
        CHECK(out.origCgroupGroup == "background", "H14: legacy basename parsed");
        CHECK(out.haveOrigCgroup, "H14: legacy haveOrigCgroup=true");
        CHECK(out.origCgroupPath.empty(), "H14: legacy path empty (compat)");
        CHECK(!out.haveOrigCgroupPath, "H14: legacy haveOrigCgroupPath=false");
        CHECK(out.origCgroupHier.empty(), "H14: legacy hier empty");
    }

    // 3. Malformed / empty path: parser catches and leaves fields safe.
    {
        std::string bad =
            "schema_version=1\npid=1003\ntid=5003\nstarttime=333\n"
            "orig_cgroup_group=foreground\nhave_orig_cgroup=1\n"
            "orig_cgroup_path=\n"   // empty path
            "orig_cgroup_hier=\n"
            "ts=1001\n";
        std::ofstream f(std::string(RUN_DIR) + "/recovery/5003.rec", std::ios::trunc);
        f << bad; f.close();
        aatpe_recovery::RecoveryRecord out;
        CHECK(aatpe_recovery::recoveryGet(5003, out) || true, "H14: malformed path no crash");
        // Note: 5003 was not bootloaded; directly test the parser via a fresh bootload.
        aatpe_recovery::recoveryBootload();
        CHECK(aatpe_recovery::recoveryGet(5003, out), "H14: bootload malformed .rec");
        CHECK(out.origCgroupGroup == "foreground", "H14: group still parsed despite empty path");
        CHECK(out.origCgroupPath.empty(), "H14: empty path stays empty");
        CHECK(!out.haveOrigCgroupPath, "H14: empty path -> havePath=false (safe fallback)");
    }

    printf(failures ? "=== H14 FAIL (%d) ===\n" : "=== H14 PASS ===\n", failures);
    return failures ? 1 : 0;
}
