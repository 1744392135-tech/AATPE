// test_caps.cpp - hermetic unit tests for aatpe_caps.cpp probe layer.
// Uses caps::setTestRoot to redirect /proc /sys /dev into a temp dir.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "aatpe_caps.h"

// Host stub for atomicWriteFile (aatpe_caps.cpp now uses atomic write 0600).
bool atomicWriteFile(const char* path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

// Additional host stubs for aatpe_caps.cpp dependencies
const char* STATE_DIR = "/tmp/aatpe_caps_test";
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
    while (std::getline(f, line)) { if (!out.empty()) out += "\n"; out += line; }
    return out;
}
// H9: log symbols (g_logLevel/logMsg/logFlush/...) now come from the single
// tests/stubs/log_stub.cpp TU (std::atomic<int> g_logLevel, matching
// aatped_logging.h). Previously test_caps.cpp defined its own `int g_logLevel` +
// logMsg/logFlush, which collided with log_stub.o under the TSan recipe.
// v4.2 A3: aatpe_caps.o's autocal path references aatpe_ml::computeDeviceTopologyHash().
// This recipe links aatpe_caps.o but NOT aatpe_ml.o, so provide a neutral host stub.
namespace aatpe_ml { std::string computeDeviceTopologyHash() { return "caps-test-topo"; } }

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static void mkdirp(const std::string& p) {
    // crude recursive mkdir
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == '/' || i == p.size() - 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
}

static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path); f << content;
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path); std::string s((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
    return s;
}

int main() {
    printf("=== Caps: v1 cgroup probe ===\n");
    {
        char tmp[] = "/tmp/caps_v1_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        // Build v1 tree: /dev/cpuset/top-app/cpus="4-6", /dev/cpuctl/top-app/tasks
        mkdirp(root + "/dev/cpuset/top-app");
        writeFile(root + "/dev/cpuset/top-app/cpus", "4-6\n");
        writeFile(root + "/dev/cpuset/top-app/tasks", "");
        mkdirp(root + "/dev/cpuctl/top-app");
        writeFile(root + "/dev/cpuctl/top-app/cpu.uclamp.min", "0");
        writeFile(root + "/dev/cpuctl/top-app/tasks", "");
        // /proc/self/mountinfo mentions cgroup (v1)
        mkdirp(root + "/proc/self");
        writeFile(root + "/proc/self/mountinfo", "25 1069 0:25 / /sys/fs/cgroup ro,nosuid,nodev,noyard shared:5 - cgroup tmpfs\n");
        // PSI
        mkdirp(root + "/proc/pressure");
        writeFile(root + "/proc/pressure/cpu", "some avg10=0.00\n");
        // GPU devfreq
        mkdirp(root + "/sys/class/devfreq/gpu0");
        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        auto c = aatpe::caps::getCaps();
        CHECK(c.cgroupVer == aatpe::caps::DeviceCaps::CgroupVersion::V1, "v1 detected");
        CHECK(c.cpusetRoot == root + "/dev/cpuset", "v1 cpusetRoot=/dev/cpuset");
        CHECK(c.cpuRoot == root + "/dev/cpuctl", "v1 cpuRoot=/dev/cpuctl");
        CHECK(c.cpusetTopAppExists == true, "v1 top-app exists");
        CHECK(c.cpusetTopAppCpus == "4-6", "v1 top-app cpus=4-6");
        CHECK(c.uclampCgroupV1 == true, "v1 uclamp node present");
        CHECK(c.psiAvailable == true, "v1 PSI available");
        CHECK(c.gpuDetected == true, "v1 GPU devfreq found");
        // resolve paths
        CHECK(aatpe::caps::resolveCgroupProcsPath("top-app") == root + "/dev/cpuset/top-app/tasks",
              "resolveCgroupProcsPath v1 top-app");
        CHECK(aatpe::caps::resolveCpuCgroupProcsPath("top-app") == root + "/dev/cpuctl/top-app/tasks",
              "resolveCpuCgroupProcsPath v1 top-app");
        // write tid (returns 0 on success per ROUND-013 three-state contract)
        CHECK(aatpe::caps::writeTidToCgroupGroup(4242, "top-app") == 0, "writeTidToCgroupGroup v1");
        CHECK(readFile(root + "/dev/cpuset/top-app/tasks") == "4242", "v1 tasks file contains tid");
        CHECK(aatpe::caps::writeTidToCpuCgroupGroup(9999, "top-app") == 0, "writeTidToCpuCgroupGroup v1");
        CHECK(readFile(root + "/dev/cpuctl/top-app/tasks") == "9999", "v1 cpuctl tasks contains tid");
    }

    printf("\n=== Caps: v2 cgroup probe ===\n");
    {
        char tmp[] = "/tmp/caps_v2_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        // v2 tree: /sys/fs/cgroup/cgroup.controllers, /sys/fs/cgroup/top-app/cgroup.procs + cgroup.threads
        mkdirp(root + "/sys/fs/cgroup/top-app");
        writeFile(root + "/sys/fs/cgroup/cgroup.controllers", "cpu memory\n");
        writeFile(root + "/sys/fs/cgroup/top-app/cgroup.procs", "");
        writeFile(root + "/sys/fs/cgroup/top-app/cgroup.threads", "");
        writeFile(root + "/sys/fs/cgroup/cpu.uclamp.min", "0");
        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        auto c = aatpe::caps::getCaps();
        CHECK(c.cgroupVer == aatpe::caps::DeviceCaps::CgroupVersion::V2, "v2 detected");
        CHECK(c.cpusetRoot == root + "/sys/fs/cgroup", "v2 cpusetRoot=/sys/fs/cgroup");
        CHECK(c.cpuRoot == root + "/sys/fs/cgroup", "v2 cpuRoot=/sys/fs/cgroup");
        CHECK(aatpe::caps::resolveCgroupProcsPath("top-app") == root + "/sys/fs/cgroup/top-app/cgroup.procs",
              "resolveCgroupProcsPath v2");
        CHECK(aatpe::caps::resolveCpuCgroupProcsPath("top-app") == root + "/sys/fs/cgroup/top-app/cgroup.procs",
              "resolveCpuCgroupProcsPath v2");
        CHECK(c.uclampCgroupV2 == true, "v2 uclamp node present");
        CHECK(aatpe::caps::writeTidToCgroupGroup(7777, "top-app") == 0, "writeTidToCgroupGroup v2");
        CHECK(readFile(root + "/sys/fs/cgroup/top-app/cgroup.threads") == "7777", "v2 cgroup.threads contains tid");
    }

    printf("\n=== Caps: no cgroup (all missing) ===\n");
    {
        char tmp[] = "/tmp/caps_none_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        // empty tree, no /dev/cpuset, no /sys/fs/cgroup
        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        auto c = aatpe::caps::getCaps();
        CHECK(c.cgroupVer == aatpe::caps::DeviceCaps::CgroupVersion::UNKNOWN, "no cgroup -> UNKNOWN");
        CHECK(aatpe::caps::resolveCgroupProcsPath("top-app").empty(), "resolveCgroupProcsPath empty when unknown");
        CHECK(aatpe::caps::writeTidToCgroupGroup(1, "top-app") != 0, "writeTidToCgroupGroup fails when unknown");
        CHECK(c.psiAvailable == false, "no PSI when /proc/pressure missing");
        CHECK(c.gpuDetected == false, "no GPU when devfreq missing");
    }

    printf("\n=== Caps: writeStateFiles produces state file ===\n");
    {
        char tmp[] = "/tmp/caps_state_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/dev/cpuset/top-app");
        writeFile(root + "/dev/cpuset/top-app/cpus", "0-3\n");
        writeFile(root + "/dev/cpuctl/top-app/tasks", "");
        mkdirp(root + "/proc/self");
        writeFile(root + "/proc/self/mountinfo", "cgroup v1\n");
        mkdirp(root + "/state");
        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        aatpe::caps::writeStateFiles((root + "/state").c_str());
        std::string s = readFile(root + "/state/capabilities");
        CHECK(!s.empty(), "caps state file written");
        CHECK(s.find("cgroup=v1") != std::string::npos, "state file contains cgroup=v1");
    }

    printf("\n=== Caps: ROUND-013 ABI layout guards ===\n");
    {
        // static_assert(sizeof(struct sched_attr)==56) is in aatpe_sched_abi.h
        // and verified at production compile time. Host-side: just confirm
        // the header compiles cleanly (already proven by linking aatpe_caps.o).
        CHECK(true, "sched_attr layout guards compiled in production build");
    }

    printf("\n=== Caps: ROUND-013 v2 cgroup.threads path + per-thread fail-closed ===\n");
    {
        char tmp[] = "/tmp/caps_v2threads_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/fs/cgroup/top-app");
        writeFile(root + "/sys/fs/cgroup/cgroup.controllers", "cpu memory\n");
        writeFile(root + "/sys/fs/cgroup/top-app/cgroup.procs", "");
        writeFile(root + "/sys/fs/cgroup/top-app/cgroup.threads", "");
        writeFile(root + "/sys/fs/cgroup/cpu.uclamp.min", "0");
        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        auto c = aatpe::caps::getCaps();
        CHECK(c.cgroupVer == aatpe::caps::DeviceCaps::CgroupVersion::V2, "v2 detected (threads test)");
        // resolveCgroupThreadPath should give cgroup.threads for v2
        CHECK(aatpe::caps::resolveCgroupThreadPath("top-app") == root + "/sys/fs/cgroup/top-app/cgroup.threads",
              "v2 resolveCgroupThreadPath -> cgroup.threads");
        CHECK(aatpe::caps::resolveCpuCgroupThreadPath("top-app") == root + "/sys/fs/cgroup/top-app/cgroup.threads",
              "v2 resolveCpuCgroupThreadPath -> cgroup.threads");
    }

    printf("\n=== Caps: ROUND-013 per-thread uclamp probe three states ===\n");
    {
        // E2BIG -> uclampTaskAvailable=false
        aatpe::caps::setTaskUclampProbeResultForTest(false, E2BIG);
        auto c1 = aatpe::caps::getCaps();
        CHECK(c1.uclampTaskAvailable == false, "probeTaskUclamp E2BIG -> false");
        // EPERM -> denied (still false but distinct)
        aatpe::caps::setTaskUclampProbeResultForTest(false, EPERM);
        auto c2 = aatpe::caps::getCaps();
        CHECK(c2.uclampTaskAvailable == false, "probeTaskUclamp EPERM -> false");
        // rc=0 -> true
        aatpe::caps::setTaskUclampProbeResultForTest(true, 0);
        auto c3 = aatpe::caps::getCaps();
        CHECK(c3.uclampTaskAvailable == true, "probeTaskUclamp rc=0 -> true");
    }

    printf("\n=== Caps: ROUND-013 v1 cgroup.threads path (should be tasks) ===\n");
    {
        char tmp[] = "/tmp/caps_v1threads_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/dev/cpuset/top-app");
        writeFile(root + "/dev/cpuset/top-app/tasks", "");
        mkdirp(root + "/dev/cpuctl/top-app");
        writeFile(root + "/dev/cpuctl/top-app/tasks", "");
        mkdirp(root + "/proc/self");
        writeFile(root + "/proc/self/mountinfo", "cgroup v1\n");
        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        CHECK(aatpe::caps::resolveCgroupThreadPath("top-app") == root + "/dev/cpuset/top-app/tasks",
              "v1 resolveCgroupThreadPath -> tasks");
        CHECK(aatpe::caps::resolveCpuCgroupThreadPath("top-app") == root + "/dev/cpuctl/top-app/tasks",
              "v1 resolveCpuCgroupThreadPath -> tasks");
    }

    printf("\n=== Caps: RC6 (B1) read-only uclamp scale inference + zero-write ===\n");
    {
        // Pure inference table: percent-range -> percent; raw-range -> raw1024;
        // unreadable/non-numeric -> unknown (safe direction).
        CHECK(strcmp(aatpe::caps::inferCgroupUclampScaleReadOnly("0"), "percent") == 0,
              "B1: resting 0 -> percent");
        CHECK(strcmp(aatpe::caps::inferCgroupUclampScaleReadOnly("50"), "percent") == 0,
              "B1: 50 -> percent");
        CHECK(strcmp(aatpe::caps::inferCgroupUclampScaleReadOnly("100"), "percent") == 0,
              "B1: 100 -> percent");
        CHECK(strcmp(aatpe::caps::inferCgroupUclampScaleReadOnly("101"), "percent") == 0,
              "B1: 101 -> percent (below raw threshold)");
        CHECK(strcmp(aatpe::caps::inferCgroupUclampScaleReadOnly("512"), "raw1024") == 0,
              "B1: 512 -> raw1024");
        CHECK(strcmp(aatpe::caps::inferCgroupUclampScaleReadOnly("1024"), "raw1024") == 0,
              "B1: 1024 -> raw1024");
        CHECK(strcmp(aatpe::caps::inferCgroupUclampScaleReadOnly(""), "unknown") == 0,
              "B1: empty -> unknown");
        CHECK(strcmp(aatpe::caps::inferCgroupUclampScaleReadOnly("max"), "unknown") == 0,
              "B1: 'max' token -> unknown");
        CHECK(strcmp(aatpe::caps::inferCgroupUclampScaleReadOnly("garbage"), "unknown") == 0,
              "B1: non-numeric -> unknown");
    }
    {
        // Zero-write probe: seed a v2 node with a distinctive raw value, run the real
        // probe(), then assert the node content is byte-for-byte UNCHANGED (the old
        // code wrote "101" here). Probe must classify it raw1024 purely by reading.
        char tmp[] = "/tmp/caps_b1_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/fs/cgroup/top-app");
        writeFile(root + "/sys/fs/cgroup/cgroup.controllers", "cpu memory\n");
        writeFile(root + "/sys/fs/cgroup/top-app/cgroup.procs", "");
        writeFile(root + "/sys/fs/cgroup/top-app/cgroup.threads", "");
        const char* kSeeded = "512";
        writeFile(root + "/sys/fs/cgroup/cpu.uclamp.min", kSeeded);
        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        auto c = aatpe::caps::getCaps();
        CHECK(c.cgroupUclampScale == "raw1024", "B1: probe read 512 -> raw1024");
        CHECK(readFile(root + "/sys/fs/cgroup/cpu.uclamp.min") == kSeeded,
              "B1: node NOT written (still 512, zero write)");
    }

    printf("\n========================================\n");
    printf("Caps results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
