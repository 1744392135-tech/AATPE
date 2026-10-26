// Host regression test for the v4.3.6 PSI closed-loop wiring (aatped_psi.cpp).
//
// Compiles the REAL production TUs: psi_decision.c (verified core) + aatped_psi.cpp
// (wiring). The daemon's external deps (diag snapshot, caps, file utils, cold-start
// hook) are stubbed here against a temp cgroup tree + STATE_DIR so we can drive
// scenarios and assert exact node writes / seam state.
//
// Scenarios covered:
//   no-pressure, MILD (debounced), SEVERE, MILD->SEVERE escalation (no double-cut),
//   recovery (exit debounce), oscillation at threshold (no flap), frame overshoot
//   overlay (MODERATE boost), IO pressure -> cold-start hook, mem pressure ->
//   bigcore-deprefer seam, PSI-unavailable fail-safe, non-finite input -> no signal.

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

// ---- temp tree -------------------------------------------------------------
static std::string g_tmp;            // root (holds cpuctl/ and STATE files)
const char* STATE_DIR = nullptr;     // defined by aatped_psi.cpp as extern; we set it

// ---- stubbed daemon deps ---------------------------------------------------
static aatpe::diag::SystemPressure g_snap;
static bool g_coldIoPressure = false;

namespace aatpe {
namespace diag {
SystemPressure getSystemPressureSnapshot() { return g_snap; }
}  // namespace diag
namespace caps {
DeviceCaps getCaps() {
    DeviceCaps c;
    c.cpuRoot = g_tmp + "/cpuctl";
    c.cgroupUclampScale = "percent";
    return c;
}
}  // namespace caps
}  // namespace aatpe

namespace aatpe {
void coldStartSetIoPressure(bool f) { g_coldIoPressure = f; }
}  // namespace aatpe

// audit4 F3: placement_mech (shared cgroup-uclamp ledger) deps not used by the PSI
// scenarios. Stub them so the ledger links without a kernel / sched_setattr.
namespace aatpe_priority {
int uclampSupportState() { return 1; }
}  // namespace aatpe_priority
namespace aatpe {
namespace caps {
int writeTidToCpuCgroupGroup(int /*tid*/, const std::string& /*group*/) { return 0; }
}  // namespace caps
}  // namespace aatpe

// Real file utils against the temp tree (the wiring layer uses ::access(F_OK),
// so these must touch the real filesystem).
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

// ---- helpers ---------------------------------------------------------------
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

// Drive one PSI sample. Each call advances sampleUptimeSec so tick() re-decides.
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
    // frame timing via state files (60Hz -> 16.67ms target)
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
    STATE_DIR = g_tmp.c_str();  // NOLINT: set before any tick

    int failures = 0;

    // =====================================================================
    // Scenario 0: no pressure. Nothing moves.
    // =====================================================================
    writeNode("background", "40.00");
    writeNode("top-app", "0.00");
    aatpe::psi::init();
    step(0, 0, 0, 0, 0);
    step(0, 0, 0, 0, 0);
    CHECK(readNodePct("background") == 40.0f);     // untouched
    CHECK(readNodePct("top-app") == 0.0f);        // untouched
    CHECK(!aatpe::psi::noncriticalMigrationPaused());
    CHECK(!aatpe::psi::bigcoreDeprefer());
    CHECK(g_coldIoPressure == false);

    // =====================================================================
    // Scenario 1: MILD overload (cpu10=12, below severe=25). enter_debounce=2
    // -> must NOT escalate on the first sample, MUST on the second.
    // Background floor 40 -> cut 20% => 32. Boost stays NONE (no frame overrun).
    // =====================================================================
    step(12, 5, 0, 0, 0);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());   // 1st sample: still OK
    CHECK(readNodePct("background") == 40.0f);            // not yet applied
    step(12, 5, 0, 0, 0);
    CHECK(readNodePct("background") == 32.0f);            // 40*(1-0.20)
    CHECK(!aatpe::psi::noncriticalMigrationPaused());    // MILD, not SEVERE
    CHECK(readNodePct("top-app") == 0.0f);               // no boost without overrun

    // =====================================================================
    // Scenario 2: escalate MILD -> SEVERE (cpu10=30). In MILD the consec_above
    // counter is already warmed at enter_debounce, so the first sample above the
    // severe threshold escalates immediately (core hysteresis design). SEVERE:
    // pause migration, bg cut 50% (from ORIGINAL 40 -> 20, not double-cut from
    // the 32 MILD left), top-app raised to 80.
    // =====================================================================
    step(30, 20, 0, 0, 0);
    CHECK(aatpe::psi::noncriticalMigrationPaused());     // already SEVERE on 1st severe
    CHECK(readNodePct("background") == 20.0f);           // 40*(1-0.50), no double-cut
    CHECK(readNodePct("top-app") == 80.0f);              // AGGRESSIVE boost to 80

    // =====================================================================
    // Scenario 3: recovery. cpu10=0. exit_debounce=4 below severe_out=21.
    // After 4 quiet samples everything must restore to originals.
    // =====================================================================
    step(0, 0, 0, 0, 0);
    step(0, 0, 0, 0, 0);
    step(0, 0, 0, 0, 0);
    CHECK(aatpe::psi::noncriticalMigrationPaused());     // still holding on 3rd
    step(0, 0, 0, 0, 0);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());   // released after 4
    CHECK(readNodePct("background") == 40.0f);           // restored verbatim
    CHECK(readNodePct("top-app") == 0.0f);               // restored

    // =====================================================================
    // Scenario 4: oscillation at threshold (cpu10=12 one step, 0 next). Never
    // 2 consecutive above -> never escalates, no writes.
    // =====================================================================
    writeNode("background", "40.00");
    step(12, 5, 0, 0, 0);   // above mild
    step(0, 0, 0, 0, 0);    // back below -> resets consec_above
    step(12, 5, 0, 0, 0);
    step(0, 0, 0, 0, 0);
    CHECK(readNodePct("background") == 40.0f);           // never cut
    CHECK(!aatpe::psi::noncriticalMigrationPaused());

    // =====================================================================
    // Scenario 5: frame overshoot overlay. MILD (cpu10=12, debounced) +
    // frameActual=25ms > target 16.67*1.10=18.33 -> MODERATE boost -> top-app 60.
    // =====================================================================
    step(12, 5, 0, 0, 25.0f);
    step(12, 5, 0, 0, 25.0f);
    CHECK(readNodePct("background") == 32.0f);            // MILD bg cut
    CHECK(readNodePct("top-app") == 60.0f);               // MODERATE boost 60
    // recover
    step(0, 0, 0, 0, 0); step(0, 0, 0, 0, 0);
    step(0, 0, 0, 0, 0); step(0, 0, 0, 0, 0);
    CHECK(readNodePct("top-app") == 0.0f);

    // =====================================================================
    // Scenario 6: IO pressure -> coldStartSetIoPressure(true). Independent of cpu.
    // =====================================================================
    step(0, 0, 25, 0, 0);
    CHECK(g_coldIoPressure == true);
    step(0, 0, 5, 0, 0);
    CHECK(g_coldIoPressure == false);

    // =====================================================================
    // Scenario 7: memory pressure -> bigcore-deprefer seam.
    // =====================================================================
    step(0, 0, 0, 10, 0);
    CHECK(aatpe::psi::bigcoreDeprefer());
    step(0, 0, 0, 0, 0);
    CHECK(!aatpe::psi::bigcoreDeprefer());

    // =====================================================================
    // Scenario 8: PSI unavailable -> fail-safe, no action, release any applied.
    // First drive SEVERE again, then kill PSI.
    // =====================================================================
    step(30, 25, 0, 0, 0);
    step(30, 25, 0, 0, 0);
    CHECK(aatpe::psi::noncriticalMigrationPaused());
    CHECK(readNodePct("background") == 20.0f);
    step(0, 0, 0, 0, 0, /*psiAvail=*/false);
    CHECK(!aatpe::psi::noncriticalMigrationPaused());
    CHECK(!aatpe::psi::bigcoreDeprefer());
    CHECK(readNodePct("background") == 40.0f);           // restored even on fail-safe
    CHECK(readNodePct("top-app") == 0.0f);
    CHECK(g_coldIoPressure == false);

    // =====================================================================
    // Scenario 9: non-finite / garbage input -> treated as no signal, no action.
    // (The core sanitises NaN/inf; the wiring passes diag-sanitised values.)
    // =====================================================================
    aatpe::psi::resetForTest();
    writeNode("background", "40.00");
    step(NAN, NAN, 1000.0f, -5.0f, -999.0f);
    step(NAN, NAN, 1000.0f, -5.0f, -999.0f);
    CHECK(readNodePct("background") == 40.0f);            // no bogus overload
    CHECK(!aatpe::psi::noncriticalMigrationPaused());

    // =====================================================================
    // Scenario 10: concurrency smoke (TSan). One thread drives tick() with a
    // pressure sweep; other threads read the placement seams concurrently. The
    // seams are atomics and the actuator is mutex-guarded -> must be race-free.
    // =====================================================================
    aatpe::psi::resetForTest();
    writeNode("background", "40.00");
    writeNode("top-app", "0.00");
    {
        std::thread driver([] {
            for (int i = 0; i < 200; ++i) {
                // alternate idle / overload / io / mem
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
    CHECK(!aatpe::psi::noncriticalMigrationPaused());  // last sample was idle

    // =====================================================================
    // audit4 F3: single-owner cgroup-uclamp ledger. The TRUE original is 5.00.
    // PSI boosts top-app; then the spillover boost "holds" top-app too. The
    // ledger must capture 5.00 exactly once (not the boosted value), restore
    // nothing while BOTH hold, and restore 5.00 only after the LAST holder
    // lets go. Previously each layer snapshotted its own baseline -> they
    // overwrote each other on release.
    // =====================================================================
    aatpe::psi::resetForTest();
    aatpe_mech::cgUclampResetForTest();   // ledger is process-global; fresh start
    writeNode("background", "40.00");
    writeNode("top-app", "5.00");
    // SEVERE overload + frame overshoot -> top-app boost to 80, bg cut.
    // (psi_decide core needs two consecutive above-threshold samples to escalate.)
    step(30, 25, 0, 0, 25.0f);
    step(30, 25, 0, 0, 25.0f);
    CHECK(readNodePct("top-app") == 80.0f);                 // PSI boost applied
    CHECK(aatpe_mech::cgUclampOriginalPct("top-app") == 5.0f); // TRUE original, not 80
    // Spillover boost also wants top-app held (refcount 2). It writes its own higher
    // floor (90% > PSI's 80%); uclamp is capped at 100 so use a valid value.
    aatpe_mech::cgUclampHold("top-app");
    writeNode("top-app", "90.00");
    // PSI recovers -> releases its hold (refcount 2->1). Must NOT restore while
    // spillover still holds.
    step(0, 0, 0, 0, 0); step(0, 0, 0, 0, 0);
    step(0, 0, 0, 0, 0); step(0, 0, 0, 0, 0);
    CHECK(readNodePct("top-app") == 90.0f);                 // not restored early
    // Spillover releases (refcount 1->0) -> restore the TRUE original 5.00.
    aatpe_mech::cgUclampReleaseHold("top-app");
    CHECK(readNodePct("top-app") == 5.0f);                  // true original restored

    // =====================================================================
    // audit4 F3: shutdown() releases any applied group-uclamp hold + seams.
    // Drive a cut/boost, then call shutdown directly (as the daemon shutdown
    // sequence does) and verify the nodes return to their originals.
    // =====================================================================
    aatpe::psi::resetForTest();
    aatpe_mech::cgUclampResetForTest();
    writeNode("background", "40.00");
    writeNode("top-app", "0.00");
    step(30, 25, 0, 0, 25.0f);
    step(30, 25, 0, 0, 25.0f);
    CHECK(readNodePct("background") == 20.0f);              // bg cut active
    CHECK(readNodePct("top-app") == 80.0f);                 // top boost active
    CHECK(aatpe::psi::noncriticalMigrationPaused());
    aatpe::psi::shutdown();
    CHECK(readNodePct("background") == 40.0f);              // restored on shutdown
    CHECK(readNodePct("top-app") == 0.0f);                  // restored on shutdown
    CHECK(!aatpe::psi::noncriticalMigrationPaused());
    CHECK(!aatpe::psi::bigcoreDeprefer());

    // =====================================================================
    // audit4 F4: per-tid uclamp floor ledger. touch RELEASE (write 0) must not
    // stomp the RT priority floor. On host issueSetMinSyscall records the last
    // value, so we assert aatpeUclampGetLastMin().
    // =====================================================================
    aatpe_mech::aatpeUclampFloorsResetForTest();
    int tidF4 = 4321;
    aatpe_mech::aatpeUclampFloorSet(tidF4, aatpe_mech::AATPE_UCLAMP_TOUCH, 512u);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 512u);     // touch active
    aatpe_mech::aatpeUclampFloorSet(tidF4, aatpe_mech::AATPE_UCLAMP_PRIORITY, 1024u);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 1024u);    // max(512,1024)=1024
    // touch boost window expires -> legacy release path writes 0. Must recompute
    // max(0, spillover=0, priority=1024)=1024, NOT stomp to 0.
    aatpe_mech::aatpeUclampSetMin(tidF4, 0u);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 1024u);    // priority floor preserved
    // spillover also wants 800 -> max(0,800,1024)=1024 still.
    aatpe_mech::aatpeUclampFloorSet(tidF4, aatpe_mech::AATPE_UCLAMP_SPILLOVER, 800u);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 1024u);
    // priority restores -> max(0,800,0)=800 (spillover keeps its floor).
    aatpe_mech::aatpeUclampFloorClear(tidF4, aatpe_mech::AATPE_UCLAMP_PRIORITY);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 800u);
    // spillover demotes -> nothing left -> 0.
    aatpe_mech::aatpeUclampFloorClear(tidF4, aatpe_mech::AATPE_UCLAMP_SPILLOVER);
    CHECK(aatpe_mech::aatpeUclampGetLastMin() == 0u);

    std::cout << "psi wiring: " << g_checks << " checks, "
              << failures << " failed" << std::endl;
    return failures ? 1 : 0;
}
