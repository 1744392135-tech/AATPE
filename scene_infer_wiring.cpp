#include "scene_infer_wiring.h"

#include <vector>
#include <string>
#include <cstring>

#ifndef AATPE_HOST_TEST
#include "aatped.h"
#include "c2_envelope.h"
#include "aatpe_diag.h"
#include "aatpe_caps.h"
#include "aatpe_managers.h"
#include "aatped_config.h"
#include "input_monitor.h"
#include <chrono>
#include <dirent.h>
#include <cstdio>
#endif

namespace aatpe { int aatpeThermalHalLevel(); }

namespace aatpe {
namespace scenewire {

scenefuse::SceneFeatures buildFeatures(const WiringInputs& in) {
    scenefuse::SceneFeatures f;
    f.fpsNow      = in.fpsNow;
    f.fpsTarget   = (in.fpsTarget > 0.0f) ? in.fpsTarget : 60.0f;
    f.missRate    = in.missRate;
    f.sfLatBudgetR = in.sfLatBudgetR;
    f.cpuUtilFg    = in.cpuUtilFg;
    f.runDelayPerMs = in.runDelayPerMs;
    f.dRunDelay_dt  = in.dRunDelay_dt;
    f.iowaitPct    = in.iowaitPct;
    f.cgThrottleDlt = 0.0f;
    f.psiCpuSome   = in.psiCpuSome;
    f.gpuBusyPct   = in.gpuBusyPct;
    f.dGpuBusy_dt  = in.dGpuBusy_dt;
    f.tapsPerSec   = in.tapsPerSec;
    f.dTaps_dt     = in.dTaps_dt;
    f.netRxPktsPs  = in.netRxPktsPs;
    f.dNetRx_dt    = in.dNetRx_dt;
    f.tempSlopeCps = 0.0f;
    f.headroom01   = in.headroom01;
    uint32_t caps = 0;
    if (in.psiAvail)   caps |= scenefuse::kCapPSI;
    if (in.gpuAvail)   caps |= scenefuse::kCapGPU;
    if (in.schedAvail) caps |= scenefuse::kCapSchedstat;
    f.capsMask  = caps;
    f.gpuTag    = in.gpuTag ? in.gpuTag : "none";
    f.fgIsGame  = in.fgIsGame;
    f.lowFpsStreak = 0;
    return f;
}

GameScene mapSceneToGameScene(scenefuse::Scene s) {
    switch (s) {
        case scenefuse::Scene::MENU:       return GameScene::MENU;
        case scenefuse::Scene::LOADING:    return GameScene::LOADING;
        case scenefuse::Scene::COMBAT:     return GameScene::COMBAT;
        case scenefuse::Scene::TEAMFIGHT:  return GameScene::COMBAT;
        case scenefuse::Scene::BACKGROUND: return GameScene::UNKNOWN;
        case scenefuse::Scene::UNKNOWN:
        default:                           return GameScene::UNKNOWN;
    }
}

SceneRoute chooseRoute(bool sceneInferEnabled) {
    return sceneInferEnabled ? SceneRoute::NEW_SCENEFUSE : SceneRoute::LEGACY_DETECTGAME;
}

bool shouldSuppressPreboost(bool engineOn, scenefuse::Scene committed) {
    return engineOn && (committed == scenefuse::Scene::LOADING);
}

TickSlopeOut computeTickSlopes(const TickSlopeState& prev,
                               float curGpuBusyPct, uint64_t curTouchEvents,
                               float curRunDelayPerMs, float elapsedSec,
                               TickSlopeState& outState) {
    TickSlopeOut out;
    outState = prev;
    outState.have = true;
    outState.gpuBusyPct   = curGpuBusyPct;
    outState.runDelayPerMs = curRunDelayPerMs;
    outState.touchEvents  = curTouchEvents;

    float dt = (elapsedSec > 0.05f) ? elapsedSec : 0.05f;

    if (!prev.have) {
        outState.tapsPerSec = 0.0f;
        out.tapsPerSec = 0.0f;
        return out;
    }

    double tapsDelta = (double)(curTouchEvents - prev.touchEvents);
    float tapsPerSec = (float)(tapsDelta / (double)dt);
    out.tapsPerSec = tapsPerSec;
    outState.tapsPerSec = tapsPerSec;

    out.dGpuBusy_dt  = (curGpuBusyPct - prev.gpuBusyPct) / dt;
    out.dTaps_dt     = (tapsPerSec - prev.tapsPerSec) / dt;
    out.dRunDelay_dt = (curRunDelayPerMs - prev.runDelayPerMs) / dt;
    return out;
}

static const char* kExternalSchedRegistry[] = {
    "scene_fas_ft_provider",
    "scene_swap_controller",
    "scenetoolbox",
    "scene_toolbox",
    "omnisched",
    "omni_scene",
};
constexpr int kExternalSchedRegistryCount =
    (int)(sizeof(kExternalSchedRegistry) / sizeof(kExternalSchedRegistry[0]));

#ifdef AATPE_HOST_TEST
int externalSchedRegistrySize() { return kExternalSchedRegistryCount; }
bool externalSchedRegistryHas(const char* fragment) {
    if (!fragment) return false;
    for (int i = 0; i < kExternalSchedRegistryCount; i++)
        if (std::strcmp(kExternalSchedRegistry[i], fragment) == 0) return true;
    return false;
}
#endif

ExternalSchedReport detectExternalSchedulers(const std::vector<std::string>& observed) {
    ExternalSchedReport out;
    bool warned = false;
    for (const std::string& name : observed) {
        bool hit = false;
        for (int i = 0; i < kExternalSchedRegistryCount; i++) {
            if (name.find(kExternalSchedRegistry[i]) != std::string::npos) { hit = true; break; }
        }
        if (!hit) continue;
        out.matchCount++;
        if (!warned) { out.matchedNames = name; warned = true; }
        else         out.matchedNames += "," + name;
    }
    out.detected = (out.matchCount > 0);
    return out;
}

#ifndef AATPE_HOST_TEST

namespace {
scenefuse::SceneState s_engine;
std::atomic<scenefuse::Scene> s_committed{scenefuse::Scene::UNKNOWN};

static TickSlopeState s_slope;
static long long      s_slopeLastMs = 0;

const char* classifyGpuTag(const ::aatpe::diag::SystemPressure& snap) {
    if (!snap.psiAvailable && snap.gpuUtilPct < 0) return "none";
    const std::string& dev = snap.gpuDevName;
    if (dev.find("kgsl") != std::string::npos) return "kgsl";
    if (dev.find("mali") != std::string::npos) return "mali";
    if (dev.find("ged") != std::string::npos)  return "ged";
    return "none";
}
}

bool loadingActiveSuppressesPreboost() {
    if (!sceneInferEnabled.load(std::memory_order_relaxed)) return false;
    return shouldSuppressPreboost(true, s_committed.load(std::memory_order_relaxed));
}

void auxTickSlot3(float avgCpuLoad) {
    using namespace scenefuse;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    int frameTimeUs = readInt((std::string(STATE_DIR) + "/target_frame_time").c_str(), 16667);
    if (frameTimeUs < 3000) frameTimeUs = 16667;
    WiringInputs in;
    in.fpsNow    = (float)readInt((std::string(STATE_DIR) + "/game_avg_fps").c_str(), 60);
    in.fpsTarget = 1e6f / (float)frameTimeUs;
    in.missRate  = (float)readInt((std::string(STATE_DIR) + "/game_frame_miss").c_str(), 0) / 100.0f;
    int sfLatUs  = readInt((std::string(STATE_DIR) + "/sf_latency_avg").c_str(), 0);
    in.sfLatBudgetR = (float)sfLatUs / (float)frameTimeUs;

    in.cpuUtilFg = avgCpuLoad;
    in.runDelayPerMs = 0.0f;
    in.dRunDelay_dt  = 0.0f;

    ::aatpe::diag::SystemPressure snap = ::aatpe::diag::getSystemPressureSnapshot();
    in.psiAvail    = snap.psiAvailable;
    in.psiCpuSome  = snap.psiAvailable ? snap.psiCpuSomePct : -1.0f;
    in.iowaitPct   = snap.psiIoSomePct;

    int gpuInt = g_gpuBusyPercent.load(std::memory_order_relaxed);
    in.gpuBusyPct = (gpuInt > 0) ? (float)gpuInt : 0.0f;
    ::aatpe::caps::DeviceCaps dcaps = ::aatpe::caps::getCaps();
    in.gpuAvail   = dcaps.gpuDetected;
    in.gpuTag     = classifyGpuTag(snap);

    long long slopeDeltaMs = nowMs - s_slopeLastMs;
    s_slopeLastMs = nowMs;
    float elapsedSec = (float)slopeDeltaMs / 1000.0f;
    if (elapsedSec <= 0.05f) elapsedSec = 0.05f;
    if (elapsedSec > 60.0f) elapsedSec = 60.0f;
    uint64_t touchNow = InputMonitor::instance().touchEventsTotal();
    TickSlopeState nextSlope;
    TickSlopeOut slopes = computeTickSlopes(s_slope,
 in.gpuBusyPct,
 touchNow,
 in.runDelayPerMs,
                                            elapsedSec, nextSlope);
    s_slope = nextSlope;
    in.tapsPerSec   = slopes.tapsPerSec;
    in.dGpuBusy_dt  = slopes.dGpuBusy_dt;
    in.dTaps_dt     = slopes.dTaps_dt;
    in.dRunDelay_dt = slopes.dRunDelay_dt;

    in.netRxPktsPs = 0.0f;
    in.dNetRx_dt   = 0.0f;

    int hrPct = g_thermalHeadroomPct.load(std::memory_order_relaxed);
    in.headroom01 = (hrPct >= 0) ? (float)hrPct / 100.0f : 0.0f;

    std::string pkg = ForegroundManager::instance().getPackage();
    in.fgIsGame = !pkg.empty() && isPackageInGameList(pkg);

    SceneFeatures f = buildFeatures(in);
    SceneDecision d = inferScene(f, s_engine, nowMs);
    s_committed.store(d.scene, std::memory_order_relaxed);

    GameScene gs = mapSceneToGameScene(d.scene);
    g_currentScene.store((int)gs, std::memory_order_release);
    {
        std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
        g_sceneState.currentScene = (int)gs;
    }

    if (c2EnvelopeEnabled.load(std::memory_order_relaxed)) {
        int halLevel = ::aatpe::aatpeThermalHalLevel();
        c2env::c2Tick(nowMs, d, in.headroom01, halLevel,
                      g_thermalEmergency.load(std::memory_order_relaxed));
    }

    {
        static int s_extScanTick = 0;
        if (++s_extScanTick >= 20) { s_extScanTick = 0; externalSchedulerScanOnce(); }
    }
}

void externalSchedulerScanOnce() {
    std::vector<std::string> observed;

    DIR* proc = opendir("/proc");
    if (proc) {
        struct dirent* de;
        while ((de = readdir(proc)) != nullptr) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
            std::string path = std::string("/proc/") + de->d_name + "/cmdline";
            FILE* f = fopen(path.c_str(), "re");
            if (!f) continue;
            char buf[256];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            if (n == 0) continue;
            buf[n] = '\0';
            std::string exe(buf);
            size_t slash = exe.find_last_of('/');
            if (slash != std::string::npos) exe = exe.substr(slash + 1);
            if (!exe.empty()) observed.push_back(exe);
        }
        closedir(proc);
    }

    DIR* mods = opendir("/data/adb/modules");
    if (mods) {
        struct dirent* de;
        while ((de = readdir(mods)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            observed.push_back(std::string(de->d_name));
        }
        closedir(mods);
    }

    ExternalSchedReport rep = detectExternalSchedulers(observed);

    atomicWriteFile((std::string(STATE_DIR) + "/external_scheduler_detected").c_str(),
                    rep.detected ? 1 : 0);
    atomicWriteFile((std::string(STATE_DIR) + "/external_scheduler_names").c_str(),
                    rep.matchedNames);

    static bool s_everDetected = false;
    if (rep.detected && !s_everDetected) {
        s_everDetected = true;
        LOG_WARN("[ext-sched] detected external scheduler(s) co-resident: %s. "
                 "AATPE will NOT fight them; verify in Scene/OmniScene whether they are "
                 "intentionally enabled, or disable one to avoid placement tug-of-war.",
                 rep.matchedNames.c_str());
    }
}

#endif

}
}
