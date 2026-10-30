#include "scene_infer.h"
#include <cmath>
#include <cstring>

namespace aatpe {
namespace scenefuse {

static inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static inline void addActive(SceneDecision& d, const char* name, float strength) {
    if (d.activeCount < 6) {
        d.active[d.activeCount].name = name;
        d.active[d.activeCount].strength01 = strength;
        d.activeCount++;
    }
}

SceneDecision inferScene(const SceneFeatures& f, SceneState& s, long long nowMs) {
    SceneDecision out;
    out.scene = Scene::UNKNOWN;
    out.confidence01 = 0.0f;
    out.activeCount = 0;
    out.degraded = false;
    out.reason = "init";

    if (!f.fgIsGame) {
        out.scene = Scene::BACKGROUND;
        out.confidence01 = 1.0f;
        out.reason = "fg_not_game";
        addActive(out, "fg_not_game", 1.0f);
        if (s.committedScene != Scene::BACKGROUND) {
            s.committedScene = Scene::BACKGROUND;
            s.sceneEnterMs = nowMs;
            s.sceneChangeCount++;
            s.lastChangeMs = nowMs;
        }
        s.gameStarted = false;
        s.enterMs = 0;
        return out;
    }

    if (!s.gameStarted) {
        s.gameStarted = true;
        if (s.enterMs == 0) {
            s.enterMs = nowMs;
        }
    }

    bool frameAxisOk = (f.fpsTarget > 0.0f);
    if (!frameAxisOk) {
        out.degraded = true;
        out.reason = "frame_axis_missing";
        if (f.cpuUtilFg < 30.0f && f.gpuBusyPct < 20.0f) {
            out.scene = Scene::MENU;
            out.confidence01 = 0.5f;
        } else if (f.cpuUtilFg > 60.0f && f.gpuBusyPct > 50.0f) {
            out.scene = Scene::COMBAT;
            out.confidence01 = 0.5f;
        }
        return out;
    }

    bool gpuOk = (f.capsMask & kCapGPU) && (std::strcmp(f.gpuTag, "none") != 0);
    if (!gpuOk) {
        out.degraded = true;
    }

    bool psiOk = (f.capsMask & kCapPSI) && (f.psiCpuSome >= 0.0f);
    if (!psiOk) {
        out.degraded = true;
    }

    float fpsRatio = (f.fpsTarget > 0.0f) ? (f.fpsNow / f.fpsTarget) : 0.0f;
    out.fpsRatio01 = (fpsRatio < 0.0f) ? 0.0f : (fpsRatio > 1.0f ? 1.0f : fpsRatio);

    if (fpsRatio < 0.15f) {
        s.lowFpsStreak++;
    } else {
        s.lowFpsStreak = 0;
    }

    float s_menu = 0.0f;
    float s_loading = 0.0f;
    float s_combat = 0.0f;
    float s_teamfight = 0.0f;

    {
        float w_cpu = 1.0f - clamp01(f.cpuUtilFg / 50.0f);
        float w_gpu = gpuOk ? (1.0f - clamp01(f.gpuBusyPct / 40.0f)) : 0.5f;
        float w_io = psiOk ? (1.0f - clamp01(f.psiCpuSome / 20.0f))
                           : (1.0f - clamp01(f.iowaitPct / 15.0f));
        s_menu = (w_cpu * 0.4f + w_gpu * 0.3f + w_io * 0.3f);
        if (fpsRatio > 0.7f) s_menu += 0.2f;
        s_menu = clamp01(s_menu);
    }

    {
        if (s.lowFpsStreak >= kLoadingMinStreak) {
            float cpuBusy = clamp01(f.cpuUtilFg / 70.0f);
            float ioBusy = psiOk ? clamp01(f.psiCpuSome / 20.0f)
                                 : clamp01(f.iowaitPct / 15.0f);
            float gpuIdle = gpuOk ? (1.0f - clamp01(f.gpuBusyPct / 30.0f)) : 0.5f;
            s_loading = (cpuBusy * 0.4f + ioBusy * 0.4f + gpuIdle * 0.2f);
            s_loading = clamp01(s_loading);
        }
    }

    {
        if (fpsRatio > 0.9f && f.missRate < 0.02f) {
            float flatTaps = (std::abs(f.dTaps_dt) < kTapsSlopeArm * 0.5f) ? 1.0f : 0.0f;
            float flatRunDelay = (std::abs(f.dRunDelay_dt) < kRunDelaySlopeArm * 0.5f) ? 1.0f : 0.0f;
            if (gpuOk) {
                float gpuMid = 0.0f;
                if (f.gpuBusyPct >= 50.0f && f.gpuBusyPct <= 85.0f) gpuMid = 1.0f;
                else if (f.gpuBusyPct > 30.0f) gpuMid = 0.5f;
                s_combat = clamp01(gpuMid * 0.5f + flatTaps * 0.25f + flatRunDelay * 0.25f);
            } else {
                s_combat = clamp01(flatTaps * 0.5f + flatRunDelay * 0.5f);
            }
        }
    }

    {
        long long gameAgeMs = nowMs - s.enterMs;
        bool baselineWarm = (gameAgeMs < 60000);

        if (fpsRatio > 0.9f && f.missRate < 0.02f) {
            int slopeHits = 0;
            if (f.dTaps_dt > kTapsSlopeArm) slopeHits++;
            if (f.dRunDelay_dt > kRunDelaySlopeArm) slopeHits++;
            if (gpuOk && f.dGpuBusy_dt > kGpuSlopeArm) slopeHits++;

            int requiredHits = baselineWarm ? 3 : 2;
            if (slopeHits >= requiredHits) {
                s_teamfight = clamp01(0.5f + slopeHits * 0.15f);
                if (out.degraded) {
                    s_teamfight -= 0.05f;
                }
            }
        }
    }

    float bestScore = 0.0f;
    Scene best = Scene::UNKNOWN;

    if (s_menu > bestScore)    { best = Scene::MENU;      bestScore = s_menu; }
    if (s_loading > bestScore)  { best = Scene::LOADING;   bestScore = s_loading; }
    if (s_combat > bestScore)   { best = Scene::COMBAT;    bestScore = s_combat; }
    if (s_teamfight > bestScore){ best = Scene::TEAMFIGHT; bestScore = s_teamfight; }

    Scene proposed = best;
    bool exitConfirmed = false;

    if (s.committedScene == Scene::TEAMFIGHT && proposed == Scene::COMBAT) {
        bool slopesFlat = (std::abs(f.dTaps_dt) < kTapsSlopeArm &&
                           std::abs(f.dRunDelay_dt) < kRunDelaySlopeArm &&
                           (!gpuOk || std::abs(f.dGpuBusy_dt) < kGpuSlopeArm));
        if (slopesFlat) {
            s.teamfightExitStreak++;
            if (s.teamfightExitStreak >= kTeamfightExitStreak) {
                s.teamfightExitStreak = 0;
                exitConfirmed = true;
            } else {
                proposed = Scene::TEAMFIGHT;
            }
        } else {
            s.teamfightExitStreak = 0;
            proposed = Scene::TEAMFIGHT;
        }
    }

    if (s.committedScene == Scene::LOADING && proposed != Scene::LOADING) {
        if (fpsRatio >= kLoadingExitFpsRatio) {
            s.loadingConvergeStreak++;
            if (s.loadingConvergeStreak >= kLoadingExitStreak) {
                s.loadingConvergeStreak = 0;
                exitConfirmed = true;
            } else {
                proposed = Scene::LOADING;
            }
        } else {
            s.loadingConvergeStreak = 0;
            proposed = Scene::LOADING;
        }
    }

    if (bestScore < kMinConfidence && proposed != Scene::BACKGROUND) {
        proposed = Scene::UNKNOWN;
        out.reason = "low_confidence";
    }

    if (proposed != s.committedScene && !exitConfirmed) {
        bool reversal = (proposed == s.prevCommittedScene);
        if (reversal) {
            if (nowMs - s.lastChangeMs > (long long)kPingPongWindowMs) {
                s.changesInWindow = 0;
            }
            s.changesInWindow++;
            if (s.changesInWindow > kPingPongMaxFlips) {
                proposed = s.committedScene;
                out.reason = "ping_pong_lock";
            }
        }
    }

    if (proposed != s.committedScene) {
        s.prevCommittedScene = s.committedScene;
        s.committedScene = proposed;
        s.sceneEnterMs = nowMs;
        s.sceneChangeCount++;
        s.lastChangeMs = nowMs;
    }

    out.scene = proposed;
    out.confidence01 = bestScore;

    if (proposed == Scene::MENU) {
        addActive(out, "low_cpu_gpu_io", s_menu);
    } else if (proposed == Scene::LOADING) {
        addActive(out, "low_fps_streak", (float)s.lowFpsStreak / 5.0f);
        addActive(out, "cpu_io_busy", s_loading);
    } else if (proposed == Scene::COMBAT) {
        addActive(out, "fps_on_target", fpsRatio);
        addActive(out, "gpu_medium", s_combat);
    } else if (proposed == Scene::TEAMFIGHT) {
        addActive(out, "slope_rising", s_teamfight);
        if (f.dTaps_dt > kTapsSlopeArm) addActive(out, "taps_rising", f.dTaps_dt);
        if (f.dRunDelay_dt > kRunDelaySlopeArm) addActive(out, "run_delay_rising", f.dRunDelay_dt);
    }

    return out;
}

#ifdef AATPE_HOST_TEST
void resetSceneStateForTest(SceneState& s) {
    s.committedScene = Scene::UNKNOWN;
    s.prevCommittedScene = Scene::UNKNOWN;
    s.stableStreak = 0;
    s.lowFpsStreak = 0;
    s.tapsBaseline = 0.0f;
    s.netBaseline = 0.0f;
    s.enterMs = 0;
    s.sceneEnterMs = 0;
    s.gameStarted = false;
    s.teamfightExitStreak = 0;
    s.sceneChangeCount = 0;
    s.lastChangeMs = 0;
    s.changesInWindow = 0;
    s.loadingConvergeStreak = 0;
}
#endif

}
}
