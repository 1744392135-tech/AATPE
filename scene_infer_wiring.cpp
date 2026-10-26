// scene_infer_wiring.cpp — see scene_infer_wiring.h.
// Pure mapping layer is always compiled + host-tested; production I/O glue is
// compiled only outside AATPE_HOST_TEST so the host link needs no daemon stubs.
#include "scene_infer_wiring.h"

#ifndef AATPE_HOST_TEST
// Production glue only (daemon build). These MUST be included at global scope,
// not inside namespace aatpe::scenewire, or libc++/daemon headers nest in the
// namespace and fail to compile in release (release-only NDK build bug).
#include "aatped.h"
#include "c2_envelope.h"
#include "aatpe_diag.h"
#include "aatpe_caps.h"
#include "aatpe_managers.h"
#include "aatped_config.h"
#include "input_monitor.h"
#include <chrono>
#endif

// Live thermal HAL accessor (defined in thermal.cpp). Forward-declared at the
// global ::aatpe scope; reopening it inside namespace aatpe::scenewire would
// shadow the real accessor and break release-only linkage.
namespace aatpe { int aatpeThermalHalLevel(); }

namespace aatpe {
namespace scenewire {

// ---- PURE mappings (no I/O, no globals) -----------------------------------

scenefuse::SceneFeatures buildFeatures(const WiringInputs& in) {
    scenefuse::SceneFeatures f;
    // frame axis
    f.fpsNow      = in.fpsNow;
    f.fpsTarget   = (in.fpsTarget > 0.0f) ? in.fpsTarget : 60.0f;
    f.missRate    = in.missRate;
    f.sfLatBudgetR = in.sfLatBudgetR;
    // cpu axis
    f.cpuUtilFg    = in.cpuUtilFg;
    f.runDelayPerMs = in.runDelayPerMs;
    f.dRunDelay_dt  = in.dRunDelay_dt;
    f.iowaitPct    = in.iowaitPct;
    f.cgThrottleDlt = 0.0f;
    f.psiCpuSome   = in.psiCpuSome;
    // gpu axis
    f.gpuBusyPct   = in.gpuBusyPct;
    f.dGpuBusy_dt  = in.dGpuBusy_dt;
    // touch / net axis
    f.tapsPerSec   = in.tapsPerSec;
    f.dTaps_dt     = in.dTaps_dt;
    f.netRxPktsPs  = in.netRxPktsPs;
    f.dNetRx_dt    = in.dNetRx_dt;
    // thermal axis
    f.tempSlopeCps = 0.0f;
    f.headroom01   = in.headroom01;
    // capabilities
    uint32_t caps = 0;
    if (in.psiAvail)   caps |= scenefuse::kCapPSI;
    if (in.gpuAvail)   caps |= scenefuse::kCapGPU;
    if (in.schedAvail) caps |= scenefuse::kCapSchedstat;
    f.capsMask  = caps;
    f.gpuTag    = in.gpuTag ? in.gpuTag : "none";
    f.fgIsGame  = in.fgIsGame;
    // lowFpsStreak is tracked inside the engine's own SceneState per tick.
    f.lowFpsStreak = 0;
    return f;
}

GameScene mapSceneToGameScene(scenefuse::Scene s) {
    switch (s) {
        case scenefuse::Scene::MENU:       return GameScene::MENU;
        case scenefuse::Scene::LOADING:    return GameScene::LOADING;
        case scenefuse::Scene::COMBAT:     return GameScene::COMBAT;
        case scenefuse::Scene::TEAMFIGHT:  return GameScene::COMBAT; // peak combat; no separate enum
        case scenefuse::Scene::BACKGROUND: return GameScene::UNKNOWN; // not in game
        case scenefuse::Scene::UNKNOWN:
        default:                           return GameScene::UNKNOWN;
    }
}

SceneRoute chooseRoute(bool sceneInferEnabled) {
    return sceneInferEnabled ? SceneRoute::NEW_SCENEFUSE : SceneRoute::LEGACY_DETECTGAME;
}

bool shouldSuppressPreboost(bool engineOn, scenefuse::Scene committed) {
    // W5 §6: only a genuine LOADING episode (engine on) may hold the C1 latency
    // boost back. Anything else -> no suppression.
    return engineOn && (committed == scenefuse::Scene::LOADING);
}

// RC6 (A2/A3): real wall-clock slope computation, pure (no I/O, no globals).
// Splits the old hardcoded-kIntervalSec=5.0f math out of the production glue so
// host tests can drive both the 5s aux cadence AND the real 25s production cadence.
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

    // Guard against a non-positive/absurd delta (caller clamps, but stay safe).
    float dt = (elapsedSec > 0.05f) ? elapsedSec : 0.05f;

    if (!prev.have) {
        // First observation: no delta yet. tapsPerSec is 0 (we need a previous
        // cumulative count to compute a rate); slopes are 0.
        outState.tapsPerSec = 0.0f;
        out.tapsPerSec = 0.0f;
        return out;
    }

    // Touch rate over the REAL elapsed seconds.
    double tapsDelta = (double)(curTouchEvents - prev.touchEvents);
    float tapsPerSec = (float)(tapsDelta / (double)dt);
    out.tapsPerSec = tapsPerSec;
    outState.tapsPerSec = tapsPerSec;

    // Slopes over the REAL elapsed seconds.
    out.dGpuBusy_dt  = (curGpuBusyPct - prev.gpuBusyPct) / dt;
    out.dTaps_dt     = (tapsPerSec - prev.tapsPerSec) / dt;
    out.dRunDelay_dt = (curRunDelayPerMs - prev.runDelayPerMs) / dt;
    return out;
}

// ---- Production I/O glue (daemon only; host test compiles this out) ---------
#ifndef AATPE_HOST_TEST

// RC6 (A1): live thermal HAL level produced by thermal.cpp. The old glue hardcoded
// halLevel=0, making c2_envelope's "HAL>=1 -> clear" thermal gate a dead branch in
// production. Read it here; the read itself is the same live source gpuBoostTick()
// and placement already consume. Fail-safe direction is unchanged because the
// headroom<0.15 and emergency gates below still close independently.

namespace {
// Persistent engine state — only loopAuxScan auxSlot3 (single slow thread) calls
// auxTickSlot3, so these statics need no lock (same discipline as detectGameScene).
scenefuse::SceneState s_engine;
// A2: s_committed is written by auxTickSlot3 (the ~25s aux slot) but read by
// loadingActiveSuppressesPreboost(), which the pre-boost predict / c4-envelope path
// can call from a different thread. Make it an atomic enum (word-sized) so the
// cross-thread read is well-defined; relaxed is fine for a soft decision hint.
std::atomic<scenefuse::Scene> s_committed{scenefuse::Scene::UNKNOWN};

// RC6 (A2/A3): slope state advanced by the PURE computeTickSlopes() over the REAL
// steady_clock delta. The old code divided the raw deltas by a hardcoded 5.0s even
// though auxSlot3 fires every ~25s (5-slot rotation), inflating dynamic features ~5x.
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
} // namespace

bool loadingActiveSuppressesPreboost() {
    // Off path: the gate read is the only decision; when off we never suppress.
    if (!sceneInferEnabled.load(std::memory_order_relaxed)) return false;
    return shouldSuppressPreboost(true, s_committed.load(std::memory_order_relaxed));
}

void auxTickSlot3(float avgCpuLoad) {
    using namespace scenefuse;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Frame axis from STATE_DIR (same nodes the 1s preboost block reads).
    int frameTimeUs = readInt((std::string(STATE_DIR) + "/target_frame_time").c_str(), 16667);
    if (frameTimeUs < 3000) frameTimeUs = 16667;
    WiringInputs in;
    in.fpsNow    = (float)readInt((std::string(STATE_DIR) + "/game_avg_fps").c_str(), 60);
    in.fpsTarget = 1e6f / (float)frameTimeUs;
    in.missRate  = (float)readInt((std::string(STATE_DIR) + "/game_frame_miss").c_str(), 0) / 100.0f;
    int sfLatUs  = readInt((std::string(STATE_DIR) + "/sf_latency_avg").c_str(), 0);
    in.sfLatBudgetR = (float)sfLatUs / (float)frameTimeUs;

    // CPU axis.
    in.cpuUtilFg = avgCpuLoad;
    in.runDelayPerMs = 0.0f;   // schedstat per-app run-delay not aggregated here -> honest 0
    in.dRunDelay_dt  = 0.0f;

    // diag snapshot: PSI + GPU util + dev name.
    ::aatpe::diag::SystemPressure snap = ::aatpe::diag::getSystemPressureSnapshot();
    in.psiAvail    = snap.psiAvailable;
    in.psiCpuSome  = snap.psiAvailable ? snap.psiCpuSomePct : -1.0f;
    in.iowaitPct   = snap.psiIoSomePct; // IO "some" doubles as io-wait proxy

    // GPU axis.
    int gpuInt = g_gpuBusyPercent.load(std::memory_order_relaxed);
    in.gpuBusyPct = (gpuInt > 0) ? (float)gpuInt : 0.0f;
    ::aatpe::caps::DeviceCaps dcaps = ::aatpe::caps::getCaps();
    in.gpuAvail   = dcaps.gpuDetected;
    in.gpuTag     = classifyGpuTag(snap);

    // Slopes over the REAL elapsed wall time (RC6 A2/A3). auxSlot3 fires every
    // ~25s on a 5-slot rotation; the old hardcoded 5.0s divided raw deltas by 5 and
    // inflated dynamic features ~5x. All math is in the PURE computeTickSlopes();
    // here we only feed the steady_clock delta and current samples.
    long long slopeDeltaMs = nowMs - s_slopeLastMs;
    s_slopeLastMs = nowMs;
    float elapsedSec = (float)slopeDeltaMs / 1000.0f;
    if (elapsedSec <= 0.05f) elapsedSec = 0.05f;   // clamp absurd/zero gaps
    if (elapsedSec > 60.0f) elapsedSec = 60.0f;    // clamp a long stall back to sane
    uint64_t touchNow = InputMonitor::instance().touchEventsTotal();
    TickSlopeState nextSlope;
    TickSlopeOut slopes = computeTickSlopes(s_slope,
                                            /*curGpuBusyPct=*/in.gpuBusyPct,
                                            /*curTouchEvents=*/touchNow,
                                            /*curRunDelayPerMs=*/in.runDelayPerMs,
                                            elapsedSec, nextSlope);
    s_slope = nextSlope;
    in.tapsPerSec   = slopes.tapsPerSec;
    in.dGpuBusy_dt  = slopes.dGpuBusy_dt;
    in.dTaps_dt     = slopes.dTaps_dt;
    in.dRunDelay_dt = slopes.dRunDelay_dt;

    // Network axis: no wlan0 rx-pkt monitor on this build -> honest degraded 0.
    in.netRxPktsPs = 0.0f;
    in.dNetRx_dt   = 0.0f;

    // Thermal axis.
    int hrPct = g_thermalHeadroomPct.load(std::memory_order_relaxed);
    in.headroom01 = (hrPct >= 0) ? (float)hrPct / 100.0f : 0.0f;

    // Foreground: is the resumed package a known game?
    std::string pkg = ForegroundManager::instance().getPackage();
    in.fgIsGame = !pkg.empty() && isPackageInGameList(pkg);

    // Run the pure mapping + engine.
    SceneFeatures f = buildFeatures(in);
    SceneDecision d = inferScene(f, s_engine, nowMs);
    s_committed.store(d.scene, std::memory_order_relaxed);

    // Publish to the external GameScene observable (same sites detectGameScene).
    GameScene gs = mapSceneToGameScene(d.scene);
    g_currentScene.store((int)gs, std::memory_order_release);
    {
        std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
        g_sceneState.currentScene = (int)gs;
    }
    // ML-01: do NOT write STATE_DIR/current_game_scene here. The legacy detector
    // (aatped_gamescene.cpp) and this NEW_SCENEFUSE route are mutually exclusive by
    // routing, but both previously wrote the same file. The main-loop republish
    // tick is now the single owner-writer; in-memory state above is the source of
    // truth.

    // C2 envelope (independent gate).
    if (c2EnvelopeEnabled.load(std::memory_order_relaxed)) {
        // RC6 (A1): pass the REAL thermal HAL level (was hardcoded 0).
        int halLevel = ::aatpe::aatpeThermalHalLevel();
        c2env::c2Tick(nowMs, d, in.headroom01, halLevel,
                      g_thermalEmergency.load(std::memory_order_relaxed));
    }
}

#endif // AATPE_HOST_TEST

} // namespace scenewire
} // namespace aatpe
