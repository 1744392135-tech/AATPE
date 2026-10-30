
#include "aatped_psi.h"
#include "psi_decision.h"
#include "aatpe_diag.h"
#include "aatpe_caps.h"
#include "aatpe_placement_mech.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::string g_tmp;
const char* STATE_DIR = nullptr;

static aatpe::diag::SystemPressure g_snap;
static bool g_coldIoPressure = false;

namespace aatpe {
namespace diag {
SystemPressure getSystemPressureSnapshot() { return g_snap; }
}
namespace caps {
DeviceCaps getCaps() {
    DeviceCaps c;
    c.cpuRoot = g_tmp + "/cpuctl";
    c.cgroupUclampScale = "percent";
    return c;
}
}
}

namespace aatpe {
void coldStartSetIoPressure(bool f) { g_coldIoPressure = f; }
}

namespace aatpe_priority {
int uclampSupportState() { return 1; }
}
namespace aatpe {
namespace caps {
int writeTidToCpuCgroupGroup(int , const std::string& ) { return 0; }
}
}

std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}
bool writeFile(const char* path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << content;
    return f.good();
}
int readInt(const char* path, int def) {
    std::string s = readFile(path);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}
void trim(std::string& s) {
    while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' '||s.back()=='\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i]==' '||s[i]=='\t')) ++i;
    s = s.substr(i);
}

static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " << #cond << std::endl; failures++; } } while (0)

static std::string nodePath(const std::string& group) {
    return g_tmp + "/cpuctl/" + group + "/cpu.uclamp.min";
}
static float readNodePct(const std::string& group) {
    std::string s = readFile(nodePath(group).c_str());
    trim(s);
    if (s.empty()) return -1.0f;
    return (float)atof(s.c_str());
}
static void writeNode(const std::string& group, const std::string& val) {
    std::filesystem::create_directories(g_tmp + "/cpuctl/" + group);
    writeFile(nodePath(group).c_str(), val);
}
static void writeState(const std::string& name, const std::string& val) {
    writeFile((std::string(STATE_DIR) + "/" + name).c_str(), val);
}

static int64_t g_uptime = 0;
static void step(float cpu10, float cpu60, float io10, float mem10,
                 float frameActualMs, bool psiAvail = true) {
    g_snap = aatpe::diag::SystemPressure{};
    g_snap.psiAvailable = psiAvail;
    g_snap.psiCpuSomePct = cpu10;
    g_snap.psiCpuSome60Pct = cpu60;
    g_snap.psiCpuSome300Pct = cpu10;
    g_snap.psiIoSomePct = io10;
    g_snap.psiMemSomePct = mem10;
    g_snap.sampleUptimeSec = ++g_uptime;
    writeState("vsync_refresh_rate", "60");
    if (frameActualMs > 0.0f) {
        writeState("sf_latency_valid", "1");
        writeState("sf_latency_avg", std::to_string((int)(frameActualMs * 1000.0f)));
    } else {
        writeState("sf_latency_valid", "0");
        writeState("sf_latency_avg", "0");
    }
    aatpe::psi::tick();
}

int main() {
    g_tmp = std::filesystem::temp_directory_path() / ("aatpe_psi_test_" + std::to_string(::getpid()));
    std::filesystem::remove_all(g_tmp);
    std::filesystem::create_directories(g_tmp);
    STATE_DIR = g_tmp.c_str();

    int failures = 0;

    writeNode("background", "40.00");
    writeNode("top-app", "0.00");
    aatpe::psi::init();
    step(0, 0, 0, 0, 0);
    step(0, 0, 0, 0, 0);
    CHECK(readNodePct("background") == 40.0f);
    CHECK(readNodePct("top-app") == 0.0f);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());
    CHECK(!aatpe::psi::bigcoreDeprefer());
    CHECK(g_coldIoPressure == false);

    step(12, 5, 0, 0, 0);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());
    CHECK(readNodePct("background") == 40.0f);
    step(12, 5, 0, 0, 0);
    CHECK(readNodePct("background") == 32.0f);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());
    CHECK(readNodePct("top-app") == 0.0f);

    step(30, 20, 0, 0, 0);
    CHECK(aatpe::psi::noncriticalMigrationPaused());
    CHECK(readNodePct("background") == 20.0f);
    CHECK(readNodePct("top-app") == 80.0f);

    step(0, 0, 0, 0, 0);
    step(0, 0, 0, 0, 0);
    step(0, 0, 0, 0, 0);
    CHECK(aatpe::psi::noncriticalMigrationPaused());
    step(0, 0, 0, 0, 0);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());
    CHECK(readNodePct("background") == 40.0f);
    CHECK(readNodePct("top-app") == 0.0f);

    writeNode("background", "40.00");
    step(12, 5, 0, 0, 0);
    step(0, 0, 0, 0, 0);
    step(12, 5, 0, 0, 0);
    step(0, 0, 0, 0, 0);
    CHECK(readNodePct("background") == 40.0f);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());

    step(12, 5, 0, 0, 25.0f);
    step(12, 5, 0, 0, 25.0f);
    CHECK(readNodePct("background") == 32.0f);
    CHECK(readNodePct("top-app") == 60.0f);
    step(0, 0, 0, 0, 0); step(0, 0, 0, 0, 0);
    step(0, 0, 0, 0, 0); step(0, 0, 0, 0, 0);
    CHECK(readNodePct("top-app") == 0.0f);

    step(0, 0, 25, 0, 0);
    CHECK(g_coldIoPressure == true);
    step(0, 0, 5, 0, 0);
    CHECK(g_coldIoPressure == false);

    step(0, 0, 0, 10, 0);
    CHECK(aatpe::psi::bigcoreDeprefer());
    step(0, 0, 0, 0, 0);
    CHECK(!aatpe::psi::bigcoreDeprefer());

    step(30, 25, 0, 0, 0);
    step(30, 25, 0, 0, 0);
    CHECK(aatpe::psi::noncriticalMigrationPaused());
    CHECK(readNodePct("background") == 20.0f);
    step(0, 0, 0, 0, 0, false);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());
    CHECK(!aatpe::psi::bigcoreDeprefer());
    CHECK(readNodePct("background") == 40.0f);
    CHECK(readNodePct("top-app") == 0.0f);
    CHECK(g_coldIoPressure == false);

    aatpe::psi::resetForTest();
    writeNode("background", "40.00");
    step(NAN, NAN, 1000.0f, -5.0f, -999.0f);
    step(NAN, NAN, 1000.0f, -5.0f, -999.0f);
    CHECK(readNodePct("background") == 40.0f);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());

    aatpe::psi::resetForTest();
    writeNode("background", "40.00");
    writeNode("top-app", "0.00");
    {
        std::thread driver([] {
            for (int i = 0; i < 200; ++i) {
                float cpu = (i % 3 == 0) ? 30.0f : 0.0f;
                float io  = (i % 4 == 0) ? 25.0f : 0.0f;
                float mem = (i % 5 == 0) ? 10.0f : 0.0f;
                step(cpu, cpu, io, mem, (i % 2) ? 25.0f : 0.0f);
            }
        });
        std::vector<std::thread> readers;
        for (int r = 0; r < 3; ++r) {
            readers.emplace_back([] {
                volatile bool sink;
                for (int i = 0; i < 5000; ++i) {
                    sink = aatpe::psi::noncriticalMigrationPaused();
                    sink = aatpe::psi::bigcoreDeprefer();
                    (void)sink;
                }
            });
        }
        driver.join();
        for (auto& t : readers) t.join();
    }
    CHECK(!aatpe::psi::noncriticalMigrationPaused());

    aatpe::psi::resetForTest();
    aatpe_mech::cgUclampResetForTest();
    writeNode("background", "40.00");
    writeNode("top-app", "5.00");
    step(30, 25, 0, 0, 25.0f);
    step(30, 25, 0, 0, 25.0f);
    CHECK(readNodePct("top-app") == 80.0f);
    CHECK(aatpe_mech::cgUclampOriginalPct("top-app") == 5.0f);
    aatpe_mech::cgUclampHold("top-app");
    writeNode("top-app", "90.00");
    step(0, 0, 0, 0, 0); step(0, 0, 0, 0, 0);
    step(0, 0, 0, 0, 0); step(0, 0, 0, 0, 0);
    CHECK(readNodePct("top-app") == 90.0f);
    aatpe_mech::cgUclampReleaseHold("top-app");
    CHECK(readNodePct("top-app") == 5.0f);

    aatpe::psi::resetForTest();
    aatpe_mech::cgUclampResetForTest();
    writeNode("background", "40.00");
    writeNode("top-app", "0.00");
    step(30, 25, 0, 0, 25.0f);
    step(30, 25, 0, 0, 25.0f);
    CHECK(readNodePct("background") == 20.0f);
    CHECK(readNodePct("top-app") == 80.0f);
    CHECK(aatpe::psi::noncriticalMigrationPaused());
    aatpe::psi::shutdown();
    CHECK(readNodePct("background") == 40.0f);
    CHECK(readNodePct("top-app") == 0.0f);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());
    CHECK(!aatpe::psi::bigcoreDeprefer());

    aatpe_mech::aatpeUclampFloorsResetForTest();
    int tidF4 = 4321;
    aatpe_mech::aatpeUclampFloorSet(tidF4, aatpe_mech::AATPE_UCLAMP_TOUCH, 512u);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 512u);
    aatpe_mech::aatpeUclampFloorSet(tidF4, aatpe_mech::AATPE_UCLAMP_PRIORITY, 1024u);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 1024u);
    aatpe_mech::aatpeUclampSetMin(tidF4, 0u);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 1024u);
    aatpe_mech::aatpeUclampFloorSet(tidF4, aatpe_mech::AATPE_UCLAMP_SPILLOVER, 800u);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 1024u);
    aatpe_mech::aatpeUclampFloorClear(tidF4, aatpe_mech::AATPE_UCLAMP_PRIORITY);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 800u);
    aatpe_mech::aatpeUclampFloorClear(tidF4, aatpe_mech::AATPE_UCLAMP_SPILLOVER);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 0u);

    std::cout << "psi wiring: " << g_checks << " checks, "
              << failures << " failed" << std::endl;
    return failures ? 1 : 0;
}
