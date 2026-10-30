
#include "aatped_psi.h"

#include "psi_decision.h"
#include "aatpe_diag.h"
#include "aatpe_caps.h"
#include "aatpe_placement_mech.h"
#include "cold_start_rules.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>

extern const char* STATE_DIR;

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

std::mutex   g_actMutex;
bool         g_bgApplied = false;
bool         g_topApplied = false;

std::atomic<bool> g_pauseMigration{false};
std::atomic<bool> g_bigcoreDeprefer{false};

int64_t      g_lastSampleUptime = -1;

std::string statePath(const char* name) {
    return std::string(STATE_DIR) + "/" + name;
}

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
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", (double)pct);
    return std::string(buf);
}

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

}

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

    aatpe::diag::SystemPressure sp = aatpe::diag::getSystemPressureSnapshot();

    if (!sp.psiAvailable) {
        {
            std::lock_guard<std::mutex> lk(g_actMutex);
            releaseActuatorsLocked();
        }
        g_pauseMigration.store(false, std::memory_order_relaxed);
        g_bigcoreDeprefer.store(false, std::memory_order_relaxed);
        aatpe::coldStartSetIoPressure(false);
        g_lastSampleUptime = sp.sampleUptimeSec;
        return;
    }

    if (sp.sampleUptimeSec == g_lastSampleUptime) return;
    g_lastSampleUptime = sp.sampleUptimeSec;

    PsiSample s;
    s.cpu_some_avg10  = sp.psiCpuSomePct;
    s.cpu_some_avg60  = sp.psiCpuSome60Pct;
    s.cpu_some_avg300 = sp.psiCpuSome300Pct;
    s.io_some_avg10   = sp.psiIoSomePct;
    s.io_some_avg60   = sp.psiIoSome60Pct;
    s.mem_some_avg10  = sp.psiMemSomePct;
    s.io_full_avg10 = 0.0f;
    s.mem_full_avg10 = 0.0f;

    const int refreshHz   = readInt(statePath("vsync_refresh_rate").c_str(), 60);
    const int sfLatValid  = readInt(statePath("sf_latency_valid").c_str(), 0);
    const int sfLatAvgUs  = readInt(statePath("sf_latency_avg").c_str(), 0);
    s.frame_target_ms = (refreshHz > 0) ? (1000.0f / (float)refreshHz) : 0.0f;
    s.frame_actual_ms = (sfLatValid == 1) ? ((float)sfLatAvgUs / 1000.0f) : 0.0f;

    PsiDecision d;
    psi_decide(&s, &g_cfg, &g_state, &d);

    g_pauseMigration.store(d.pause_noncritical_migration != 0, std::memory_order_relaxed);
    g_bigcoreDeprefer.store(d.mem_bigcore_deprefer != 0, std::memory_order_relaxed);

    aatpe::coldStartSetIoPressure(d.io_pressure_downgrade != 0);

    {
        std::lock_guard<std::mutex> lk(g_actMutex);
        const auto caps = aatpe::caps::getCaps();

        if (d.downgrade_background) {
            if (!g_bgApplied) {
                if (readGroupUclampPct("background") >= 0.0f) {
                    aatpe_mech::cgUclampHold("background");
                    g_bgApplied = true;
                }
            }
            if (g_bgApplied) {
                const float origPct = aatpe_mech::cgUclampOriginalPct("background");
                if (origPct >= 0.0f) {
                    const float cut = d.bg_uclamp_reduction_pct;
                    const float target = origPct * (100.0f - cut) / 100.0f;
                    writeFile((caps.cpuRoot + "/background/cpu.uclamp.min").c_str(),
                              pctToNodeValue(target));
                }
            }
        } else if (g_bgApplied) {
            aatpe_mech::cgUclampReleaseHold("background");
            g_bgApplied = false;
        }

        if (d.boost == PSI_BOOST_MODERATE || d.boost == PSI_BOOST_AGGRESSIVE) {
            if (!g_topApplied) {
                if (readGroupUclampPct("top-app") >= 0.0f) {
                    aatpe_mech::cgUclampHold("top-app");
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

}
}
