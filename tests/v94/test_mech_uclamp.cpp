#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "aatpe_placement_mech.h"
#include "aatpe_caps.h"

using namespace aatpe_mech;

const char* STATE_DIR = "/tmp/aatpe_mech_uclamp";
const char* RUN_DIR = "/tmp/aatpe_mech_uclamp";

bool atomicWriteFile(const char* path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

static std::string g_stateDir;

namespace aatpe_priority { bool uclampSupportState() { return false; } }

bool writeFile(const char* path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    return true;
}
std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f) return "";
    std::string line, out;
    while (std::getline(f, line)) {
        if (!out.empty()) out += "\n";
        out += line;
    }
    return out;
}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static void mkdirp(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == '/') mkdir(cur.c_str(), 0755);
    }
    mkdir(cur.c_str(), 0755);
}

int main() {
    printf("=== test_mech_uclamp: cgroup uclamp mech layer ===\n");

    char tmpDir[] = "/tmp/test_mech_uclamp_XXXXXX";
    char* tmp = mkdtemp(tmpDir);
    g_stateDir = tmp;
    mkdirp(std::string(tmp) + "/state");

    bool ok = writeCgroupUclampMin(1234, 512, "top-app");
    CHECK(!ok, "writeCgroupUclampMin returns false when caps has no uclamp");

    ok = restoreTidFromCgroupUclamp(1234, "", "foreground");
    CHECK(!ok, "restoreTidFromCgroupUclamp returns false on empty boostGroup");

    restoreAllCgroupUclamp();
    CHECK(true, "restoreAllCgroupUclamp no crash on empty state");

    aatpe_mech::SchedCaps caps;
    caps.uclamp = true;
    caps.cgroupUclamp = true;
    auto m = aatpe_mech::selectBoostMechanism(caps);
    CHECK(m == aatpe_mech::BoostMechanism::UCLAMP, "selectBoostMechanism prefers UCLAMP");

    caps.uclamp = false;
    m = aatpe_mech::selectBoostMechanism(caps);
    CHECK(m == aatpe_mech::BoostMechanism::CGROUP_UCLAMP, "selectBoostMechanism falls to CGROUP_UCLAMP");

    CHECK(strcmp(aatpe_mech::mechanismName(aatpe_mech::BoostMechanism::UCLAMP), "uclamp") == 0, "mechanismName UCLAMP");
    CHECK(strcmp(aatpe_mech::mechanismName(aatpe_mech::BoostMechanism::CGROUP_UCLAMP), "cgroup_uclamp") == 0, "mechanismName CGROUP_UCLAMP");

    {
        mechSetTestRoot(tmp);
        int tid = 4321;
        mkdirp(std::string(tmp) + "/proc/" + std::to_string(tid));
        std::ofstream cf(std::string(tmp) + "/proc/" + std::to_string(tid) + "/cgroup");
        cf << "7:sched,cpu:/foo/top-app\n";
        cf.close();
        std::string base, path, hier;
        bool found = readThreadCgroupInfo(tid, &base, &path, &hier);
        CHECK(found, "A5: v1 sched,cpu cgroup line parsed");
        CHECK(hier == "v1-cpu", "A5: v1 sched,cpu -> hier==v1-cpu (off-by-one fixed)");
        CHECK(base == "top-app", "A5: v1 sched,cpu -> basename top-app");
        std::ofstream cf2(std::string(tmp) + "/proc/" + std::to_string(tid) + "/cgroup");
        cf2 << "3:cpuset:/a/b/top-app\n7:sched,cpu:/foo/top-app\n";
        cf2.close();
        std::string b2, p2, h2;
        readThreadCgroupInfo(tid, &b2, &p2, &h2);
        CHECK(h2 == "v1-cpuset", "A5: cpuset line preferred over cpu line");
    }

    {
        aatpeUclampFloorsResetForTest();
        aatpeUclampFloorSet(5555, AATPE_UCLAMP_TOUCH, 512);
        CHECK(aatpeUclampGetLastMin() == 512, "B2: old thread eff=512");
        aatpeUclampFloorReap(5555);
        aatpeUclampFloorSet(5555, AATPE_UCLAMP_SPILLOVER, 100);
        CHECK(aatpeUclampGetLastMin() == 100, "B2: tid reuse NOT wrongly lifted (eff=100, stale 512 reaped)");
        aatpeUclampFloorReap(7777);
        aatpeUclampFloorSet(7777, AATPE_UCLAMP_TOUCH, 200);
        CHECK(aatpeUclampGetLastMin() == 200, "B2: fresh tid eff=200");
        aatpeUclampFloorsResetForTest();
    }

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}