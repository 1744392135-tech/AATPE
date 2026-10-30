#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sys/stat.h>
#include "aatpe_placement_mech.h"
#include "aatpe_caps.h"
#include "aatpe_recovery.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

const char* RUN_DIR   = "/tmp/aatpe_h14e2e_run";
const char* STATE_DIR = "/tmp/aatpe_h14e2e_state";
bool writeFileAtomic(const char*, const std::string&) { return true; }
std::string readFile(const char* path) {
    std::ifstream f(path); if (!f) return "";
    std::string s, line;
    while (std::getline(f, line)) { if (!s.empty()) s += "\n"; s += line; }
    return s;
}
bool writeFile(const char*, const std::string&) { return true; }
bool atomicWriteFile(const char*, const std::string&) { return true; }
namespace aatpe { namespace caps {
DeviceCaps getCaps() { static DeviceCaps c; return c; }
int writeTidToCgroupGroup(int, const std::string&) { return 0; }
int writeTidToCpuCgroupGroup(int, const std::string&) { return 0; }
}}
namespace aatpe_priority { int uclampSupportState() { return 0; } }

static void writeFakeCgroup(int tid, const std::string& content) {
    std::string dir = "/tmp/aatpe_h14e2e_root/proc/" + std::to_string(tid);
    ::mkdir("/tmp/aatpe_h14e2e_root", 0755);
    ::mkdir("/tmp/aatpe_h14e2e_root/proc", 0755);
    ::mkdir(dir.c_str(), 0755);
    std::ofstream f(dir + "/cgroup", std::ios::trunc);
    f << content;
}

int main() {
    printf("==> H14 e2e: readThreadCgroupInfo v1/v2/bad + recovery round-trip ==\n");
    ::mkdir(RUN_DIR, 0755);
    ::mkdir(STATE_DIR, 0755);
    aatpe_mech::mechSetTestRoot("/tmp/aatpe_h14e2e_root");

    writeFakeCgroup(6001,
        "2:cpuset:/foreground\n"
        "3:cpu:/foreground\n"
        "4:schedtune:/\n");
    {
        std::string base, path, hier;
        bool ok = aatpe_mech::readThreadCgroupInfo(6001, &base, &path, &hier);
        CHECK(ok, "v1: parsed");
        CHECK(base == "foreground", "v1: basename=foreground");
        CHECK(path == "foreground", "v1: path=foreground");
        CHECK(hier == "v1-cpuset", "v1: hier=v1-cpuset");
    }

    writeFakeCgroup(6002,
        "2:cpuset:/a/b/foreground\n"
        "3:cpu:/a/b/foreground\n");
    {
        std::string base, path, hier;
        bool ok = aatpe_mech::readThreadCgroupInfo(6002, &base, &path, &hier);
        CHECK(ok, "v1-nested: parsed");
        CHECK(base == "foreground", "v1-nested: basename=foreground");
        CHECK(path == "a/b/foreground", "v1-nested: full path=a/b/foreground");
        CHECK(hier == "v1-cpuset", "v1-nested: hier=v1-cpuset");
    }

    writeFakeCgroup(6003, "0::/a/b/top-app\n");
    {
        std::string base, path, hier;
        bool ok = aatpe_mech::readThreadCgroupInfo(6003, &base, &path, &hier);
        CHECK(ok, "v2: parsed");
        CHECK(base == "top-app", "v2: basename=top-app");
        CHECK(path == "a/b/top-app", "v2: full path=a/b/top-app");
        CHECK(hier == "v2-unified", "v2: hier=v2-unified");
    }

    {
        std::string base, path, hier;
        bool ok = aatpe_mech::readThreadCgroupInfo(99999, &base, &path, &hier);
        CHECK(!ok, "bad: open failure returns false");
        CHECK(base.empty() && path.empty() && hier.empty(), "bad: outputs cleared");
    }

    {
        aatpe_recovery::RecoveryRecord r;
        r.pid = 1001; r.tid = 6002; r.starttime = 555;
        r.origCgroupGroup = "foreground";
        r.haveOrigCgroup = true;
        r.timestamp = 1;
        aatpe_recovery::recoveryCommit(r);
        CHECK(aatpe_recovery::recoveryRecordCgroupPath(6002, "a/b/foreground", "v1-cpuset"),
              "recovery: recordCgroupPath ok");
        aatpe_recovery::RecoveryRecord out;
        CHECK(aatpe_recovery::recoveryGet(6002, out), "recovery: get back");
        CHECK(out.origCgroupPath == "a/b/foreground", "recovery: path persisted");
        CHECK(out.origCgroupHier == "v1-cpuset", "recovery: hier persisted");
        CHECK(out.haveOrigCgroupPath, "recovery: havePath=true");
        CHECK(out.origCgroupGroup == "foreground", "recovery: basename still there");
    }

    {
        CHECK(!aatpe_recovery::recoveryRecordCgroupPath(6001, "", ""),
              "recovery: empty path no-op");
        aatpe_recovery::RecoveryRecord out;
        CHECK(!aatpe_recovery::recoveryGet(6001, out), "recovery: 6001 no record (safe)");
    }

    printf(failures ? "=== H14 e2e FAIL (%d) ===\n" : "=== H14 e2e PASS ===\n", failures);
    return failures ? 1 : 0;
}
