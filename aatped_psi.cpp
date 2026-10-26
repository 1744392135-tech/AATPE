// AATPE v4.3.6 - PSI decision closed-loop wiring (see aatped_psi.h for contract).
//
// LAYER MAP (every side-effect below is a group-level node write or an existing
// hook; per-thread uclamp is deliberately NOT touched):
//   PsiDecision.pause_noncritical_migration -> g_pauseMigration (placement seam)
//   PsiDecision.mem_bigcore_deprefer         -> g_bigcoreDeprefer (placement seam)
//   PsiDecision.downgrade_background        -> "background" cpu.uclamp.min cut
//   PsiDecision.boost (MODERATE/AGGRESSIVE) -> "top-app"    cpu.uclamp.min raise
//   PsiDecision.io_pressure_downgrade       -> coldStartSetIoPressure()
//   reasons[]                               -> STATE_DIR/psi_decision
//
// The pure core psi_decision.{h,c} is reused byte-for-byte (verified C11,
// extern "C"); this file only feeds it and acts on its output.

#include "aatped_psi.h"

#include "psi_decision.h"
#include "aatpe_diag.h"
#include "aatpe_caps.h"
#include "aatpe_placement_mech.h"   // audit4 F3: shared cgroup-uclamp original ledger
#include "cold_start_rules.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>

// Defined in aatped_main.cpp; kept as an opaque extern here so this translation
// unit does not have to pull the whole daemon header graph.
extern const char* STATE_DIR;

// Global file utilities (defined in aatped_utils.cpp). Forward-declared here
// instead of including the heavy aatped_utils.h so this layer stays light and
// host-testable; signatures match aatped_utils.h exactly.
std::string readFile(const char* path);
bool        writeFile(const char* path, const std::string& content);
int         readInt(const char* path, int def);
void        trim(std::string& s);

namespace aatpe {
namespace psi {
namespace {

PsiConfig    g_cfg;
PsiState     g_state;
bool         g_inited = false;

// Group-uclamp actuator bookkeeping. Guarded by g_actMutex because tick() and
// the test reset both touch it. audit4 F3: the verbatim original node string is NO
// LONGER snapshotted here — placement_mech is the SINGLE owner of the original
// ledger (cgUclampRecordOriginal / cgUclampHold / cgUclampReleaseHold), so PSI and
// the spillover boost can no longer restore two different baselines. We only track
// whether WE currently hold each group.
std::mutex   g_actMutex;
bool         g_bgApplied = false;
bool         g_topApplied = false;

// Placement seams: lock-free atomics. Written by tick(), read by placement.cpp.
std::atomic<bool> g_pauseMigration{false};
std::atomic<bool> g_bigcoreDeprefer{false};

// New-sample gate: the diag snapshot refreshes on its own ~1-2s cadence; we only
// re-decide when sampleUptimeSec advances, so calling tick() every loop is cheap.
int64_t      g_lastSampleUptime = -1;

std::string statePath(const char* name) {
    return std::string(STATE_DIR) + "/" + name;
}

// Convert a 0..100 percent floor to the string the cgroup node expects.
// Mirrors aatpe_placement_mech.cpp:utilToCgroupPct (percent vs raw1024 scale
// probed at startup). Kept local so this layer does not link placement internals.
std::string pctToNodeValue(float pct) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    const std::string scale = aatpe::caps::getCaps().cgroupUclampScale;
    if (scale == "raw1024") {
        unsigned util = (unsigned)lroundf(pct * 1024.0f / 100.0f);
        if (util > 1024) util = 1024u;
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", util);
        return std::string(buf);
    }
    // default / "percent" / "unknown" -> percentage with 2 decimals.
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", (double)pct);
    return std::string(buf);
}

// Read a cgroup's current cpu.uclamp.min as a 0..100 percentage.
// Returns -1.0f when the node is absent or unreadable (caller skips the group).
float readGroupUclampPct(const std::string& group) {
    const auto caps = aatpe::caps::getCaps();
    const std::string path = caps.cpuRoot + "/" + group + "/cpu.uclamp.min";
    if (::access(path.c_str(), F_OK) != 0) return -1.0f;
    std::string s = readFile(path.c_str());
    trim(s);
    if (s.empty()) return -1.0f;
    double v = strtod(s.c_str(), nullptr);
    if (!std::isfinite(v)) return -1.0f;
    if (caps.cgroupUclampScale == "raw1024") v = v * 100.0 / 1024.0;
    if (v < 0.0) v = 0.0;
    if (v > 100.0) v = 100.0;
    return (float)v;
}

// Release any group uclamp hold this layer currently owns. Caller holds
// g_actMutex. audit4 F3: the actual node restore happens in the shared placement_mech
// ledger — cgUclampReleaseHold writes back the single recorded original only when
// the LAST holder (PSI OR the spillover boost) lets go.
void releaseActuatorsLocked() {
    if (g_bgApplied) {
        aatpe_mech::cgUclampReleaseHold("background");
        g_bgApplied = false;
    }
    if (g_topApplied) {
        aatpe_mech::cgUclampReleaseHold("top-app");
        g_topApplied = false;
    }
}

const char* reasonName(PsiReason r) {
    switch (r) {
    case PSI_REASON_NONE:              return "none";
    case PSI_REASON_CPU_STALL:         return "cpu_stall";
    case PSI_REASON_CPU_STALL_SEVERE:  return "cpu_stall_severe";
    case PSI_REASON_CPU_STALL_RISING:  return "cpu_stall_rising";
    case PSI_REASON_FRAME_OVERRUN:     return "frame_overrun";
    case PSI_REASON_IO_PRESSURE:       return "io_pressure";
    case PSI_REASON_MEM_PRESSURE:      return "mem_pressure";
    case PSI_REASON_HYSTERESIS_HOLD:   return "hysteresis_hold";
    case PSI_REASON_RELEASE:           return "release";
    }
    return "?";
}

}  // namespace

void init() {
    if (g_inited) return;
    psi_config_default(&g_cfg);
    psi_state_init(&g_state);
    g_inited = true;
}

bool noncriticalMigrationPaused() noexcept { return g_pauseMigration.load(std::memory_order_relaxed); }
bool bigcoreDeprefer() noexcept           { return g_bigcoreDeprefer.load(std::memory_order_relaxed); }

#ifdef AATPE_HOST_TEST
void resetForTest() {
    std::lock_guard<std::mutex> lk(g_actMutex);
    releaseActuatorsLocked();
    psi_state_init(&g_state);
    g_pauseMigration.store(false, std::memory_order_relaxed);
    g_bigcoreDeprefer.store(false, std::memory_order_relaxed);
    g_lastSampleUptime = -1;
}
#endif

void shutdown() {
    // audit4 F3: release every PSI-owned group-uclamp hold (the shared ledger writes
    // back the recorded original when the last holder lets go) and drop the placement
    // seams. Called from the daemon shutdown sequence; idempotent and safe to call
    // when nothing is applied.
    {
        std::lock_guard<std::mutex> lk(g_actMutex);
        releaseActuatorsLocked();
    }
    g_pauseMigration.store(false, std::memory_order_relaxed);
    g_bigcoreDeprefer.store(false, std::memory_order_relaxed);
    aatpe::coldStartSetIoPressure(false);
    g_lastSampleUptime = -1;
}

void tick() {
    if (!g_inited) init();

    // Snapshot the existing PSI collection (read-only, thread-safe).
    aatpe::diag::SystemPressure sp = aatpe::diag::getSystemPressureSnapshot();

    // Fail-safe: /proc/pressure missing -> act on nothing. Release anything we
    // previously applied and clear the seams. Never inject a decision.
    if (!sp.psiAvailable) {
        {
            std::lock_guard<std::mutex> lk(g_actMutex);
            releaseActuatorsLocked();
        }
        g_pauseMigration.store(false, std::memory_order_relaxed);
        g_bigcoreDeprefer.store(false, std::memory_order_relaxed);
        aatpe::coldStartSetIoPressure(false);
        g_lastSampleUptime = sp.sampleUptimeSec;  // don't re-run until a sample returns
        return;
    }

    // New-sample gate: skip the whole closed loop until diag refreshes.
    if (sp.sampleUptimeSec == g_lastSampleUptime) return;
    g_lastSampleUptime = sp.sampleUptimeSec;

    // --- fill the pure core's input from existing collection + frame timing ---
    PsiSample s;
    // diag already sanitises each bucket to finite [0,100]; pass through.
    s.cpu_some_avg10  = sp.psiCpuSomePct;
    s.cpu_some_avg60  = sp.psiCpuSome60Pct;
    s.cpu_some_avg300 = sp.psiCpuSome300Pct;
    s.io_some_avg10   = sp.psiIoSomePct;
    s.io_some_avg60   = sp.psiIoSome60Pct;
    s.mem_some_avg10  = sp.psiMemSomePct;
    // The current collector exposes only the "some" lines. The core takes the
    // max(some, full) for IO; without a "full" sample we wire 0 so it degrades
    // to the "some" line -- identical to production's existing io>20 trigger.
    s.io_full_avg10 = 0.0f;
    s.mem_full_avg10 = 0.0f;

    // Frame timing from already-written state files (decoupled; host-testable).
    // sf_latency_* are microseconds; invalid -> 0 (core disables the overrun check
    // when target<=0, and a 0 actual can never overrun).
    const int refreshHz   = readInt(statePath("vsync_refresh_rate").c_str(), 60);
    const int sfLatValid  = readInt(statePath("sf_latency_valid").c_str(), 0);
    const int sfLatAvgUs  = readInt(statePath("sf_latency_avg").c_str(), 0);
    s.frame_target_ms = (refreshHz > 0) ? (1000.0f / (float)refreshHz) : 0.0f;
    s.frame_actual_ms = (sfLatValid == 1) ? ((float)sfLatAvgUs / 1000.0f) : 0.0f;

    // --- pure decision (hysteresis + debounce live inside the core) ---
    PsiDecision d;
    psi_decide(&s, &g_cfg, &g_state, &d);

    // --- map decision -> placement seams (cheap, lock-free) ---
    g_pauseMigration.store(d.pause_noncritical_migration != 0, std::memory_order_relaxed);
    g_bigcoreDeprefer.store(d.mem_bigcore_deprefer != 0, std::memory_order_relaxed);

    // --- IO pressure: reuse the existing cold-start hook ---
    aatpe::coldStartSetIoPressure(d.io_pressure_downgrade != 0);

    // --- group-level uclamp actuator (shared ledger / single original owner) ---
    {
        std::lock_guard<std::mutex> lk(g_actMutex);
        const auto caps = aatpe::caps::getCaps();

        // Background downgrade: cut the bg group floor by the suggested pct. The cut
        // is always relative to the ORIGINAL floor recorded by the shared ledger on
        // first touch, so a MILD->SEVERE escalation does not double-cut.
        if (d.downgrade_background) {
            if (!g_bgApplied) {
                if (readGroupUclampPct("background") >= 0.0f) {
                    aatpe_mech::cgUclampHold("background");  // records original once
                    g_bgApplied = true;
                }
            }
            if (g_bgApplied) {
                const float origPct = aatpe_mech::cgUclampOriginalPct("background");
                if (origPct >= 0.0f) {
                    const float cut = d.bg_uclamp_reduction_pct;  // 0..100
                    const float target = origPct * (100.0f - cut) / 100.0f;
                    writeFile((caps.cpuRoot + "/background/cpu.uclamp.min").c_str(),
                              pctToNodeValue(target));
                }
            }
        } else if (g_bgApplied) {
            aatpe_mech::cgUclampReleaseHold("background");
            g_bgApplied = false;
        }

        // Critical boost: raise the top-app group floor to at least the suggested
        // pct. max(orig, boost, LIVE) so we only lift — never lower an existing
        // spillover boost on the shared top-app node.
        if (d.boost == PSI_BOOST_MODERATE || d.boost == PSI_BOOST_AGGRESSIVE) {
            if (!g_topApplied) {
                if (readGroupUclampPct("top-app") >= 0.0f) {
                    aatpe_mech::cgUclampHold("top-app");  // records original once
                    g_topApplied = true;
                }
            }
            if (g_topApplied) {
                const float origPct = aatpe_mech::cgUclampOriginalPct("top-app");
                const float livePct = readGroupUclampPct("top-app");
                float target = std::max(origPct, d.critical_uclamp_boost_pct);
                if (livePct >= 0.0f) target = std::max(target, livePct);
                writeFile((caps.cpuRoot + "/top-app/cpu.uclamp.min").c_str(),
                          pctToNodeValue(target));
            }
        } else if (g_topApplied) {
            aatpe_mech::cgUclampReleaseHold("top-app");
            g_topApplied = false;
        }
    }

    // --- reasons -> diag / WebUI ---
    {
        std::string line = "overload=" + std::to_string((int)d.overload) +
                          " boost=" + std::to_string((int)d.boost) +
                          " bg_down=" + std::to_string(d.downgrade_background) +
                          " pause_mig=" + std::to_string(d.pause_noncritical_migration) +
                          " io=" + std::to_string(d.io_pressure_downgrade) +
                          " mem_depref=" + std::to_string(d.mem_bigcore_deprefer) +
                          " reasons=";
        for (int i = 0; i < d.reason_count; ++i) {
            if (i) line += ",";
            line += reasonName(d.reasons[i]);
        }
        writeFile(statePath("psi_decision").c_str(), line);
    }
}

}  // namespace psi
}  // namespace aatpe
