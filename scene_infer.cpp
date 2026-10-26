// scene_infer.cpp — pure function implementation.
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

    // ---- Fail-closed chain (top-down) ----

    // 1. fgIsGame=false -> BACKGROUND (confidence 1.0).
    if (!f.fgIsGame) {
        out.scene = Scene::BACKGROUND;
        out.confidence01 = 1.0f;
        out.reason = "fg_not_game";
        addActive(out, "fg_not_game", 1.0f);
        // Update state.
        if (s.committedScene != Scene::BACKGROUND) {
            s.committedScene = Scene::BACKGROUND;
            s.sceneEnterMs = nowMs;
            s.sceneChangeCount++;
            s.lastChangeMs = nowMs;
        }
        // RC3: leaving the game releases the game-start anchor, so the next launch
        // re-arms the 60s warm-up window from scratch.
        s.gameStarted = false;
        s.enterMs = 0;
        return out;
    }

    // RC3: game-open edge latch. enterMs anchors the GAME-OPEN baseline (warm-up
    // window), NOT every scene switch. It is written exactly once on the
    // fgIsGame false->true transition and only if still unset, so host tests may
    // preset s.enterMs far in the past to bypass warm-up and that preset is never
    // clobbered by a later scene commit.
    if (!s.gameStarted) {
        s.gameStarted = true;
        if (s.enterMs == 0) {
            s.enterMs = nowMs;
        }
    }

    // 2. Frame axis unavailable -> degraded, fall back to simple logic.
    bool frameAxisOk = (f.fpsTarget > 0.0f);
    if (!frameAxisOk) {
        out.degraded = true;
        out.reason = "frame_axis_missing";
        // Fallback: use cpu + gpu only (old detectGameScene-like).
        if (f.cpuUtilFg < 30.0f && f.gpuBusyPct < 20.0f) {
            out.scene = Scene::MENU;
            out.confidence01 = 0.5f;
        } else if (f.cpuUtilFg > 60.0f && f.gpuBusyPct > 50.0f) {
            out.scene = Scene::COMBAT;
            out.confidence01 = 0.5f;
        }
        return out;
    }

    // 3. GPU unavailable -> weight 0 on GPU axis, mark degraded.
    bool gpuOk = (f.capsMask & kCapGPU) && (std::strcmp(f.gpuTag, "none") != 0);
    if (!gpuOk) {
        out.degraded = true;
    }

    // 4. PSI unavailable -> weight 0 on PSI, use iowait + runDelay proxy.
    bool psiOk = (f.capsMask & kCapPSI) && (f.psiCpuSome >= 0.0f);
    if (!psiOk) {
        out.degraded = true;
    }

    // ---- Normalization ----
    float fpsRatio = (f.fpsTarget > 0.0f) ? (f.fpsNow / f.fpsTarget) : 0.0f;
    // RC6 (A5): expose the real fps-recovery ratio on the decision so downstream
    // consumers (C2 envelope LOADING exit) use the genuine fps signal instead of
    // proxying confidence. Stays 0.0 when the frame axis is unavailable -> the
    // consumer fail-safe (do not exit on a missing signal) kicks in.
    out.fpsRatio01 = (fpsRatio < 0.0f) ? 0.0f : (fpsRatio > 1.0f ? 1.0f : fpsRatio);

    // Update lowFpsStreak.
    if (fpsRatio < 0.15f) {
        s.lowFpsStreak++;
    } else {
        s.lowFpsStreak = 0;
    }

    // ---- Scoring: each scene gets a score01 ----

    float s_menu = 0.0f;
    float s_loading = 0.0f;
    float s_combat = 0.0f;
    float s_teamfight = 0.0f;

    // MENU: low everything, fps normal.
    {
        float w_cpu = 1.0f - clamp01(f.cpuUtilFg / 50.0f);
        float w_gpu = gpuOk ? (1.0f - clamp01(f.gpuBusyPct / 40.0f)) : 0.5f;
        float w_io = psiOk ? (1.0f - clamp01(f.psiCpuSome / 20.0f))
                           : (1.0f - clamp01(f.iowaitPct / 15.0f));
        s_menu = (w_cpu * 0.4f + w_gpu * 0.3f + w_io * 0.3f);
        // Boost if fps is healthy (UI animation).
        if (fpsRatio > 0.7f) s_menu += 0.2f;
        s_menu = clamp01(s_menu);
    }

    // LOADING: fps≈0 + cpu/io busy + gpu idle.
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

    // COMBAT: fps on target, gpu medium-high, slopes flat.
    {
        if (fpsRatio > 0.9f && f.missRate < 0.02f) {
            // Flat-slope evidence (shared by both GPU-available and degraded paths).
            float flatTaps = (std::abs(f.dTaps_dt) < kTapsSlopeArm * 0.5f) ? 1.0f : 0.0f;
            float flatRunDelay = (std::abs(f.dRunDelay_dt) < kRunDelaySlopeArm * 0.5f) ? 1.0f : 0.0f;
            if (gpuOk) {
                float gpuMid = 0.0f;
                if (f.gpuBusyPct >= 50.0f && f.gpuBusyPct <= 85.0f) gpuMid = 1.0f;
                else if (f.gpuBusyPct > 30.0f) gpuMid = 0.5f;
                s_combat = clamp01(gpuMid * 0.5f + flatTaps * 0.25f + flatRunDelay * 0.25f);
            } else {
                // RC4 (W5 §5.5.3): GPU axis unavailable -> its 0.5 weight is dropped
                // and redistributed proportionally onto the touch/run-delay flat-slope
                // components (degrade to CPU+touch+frame). A truly flat game then
                // scores 1.0 instead of being capped at 0.75 by the old phantom
                // gpuMid=0.5, guaranteeing S_combat >= 0.7 on flat对局.
                s_combat = clamp01(flatTaps * 0.5f + flatRunDelay * 0.5f);
            }
        }
    }

    // TEAMFIGHT: fps still good, but slopes rising (early warning).
    {
        // Baseline warm-up: skip TEAMFIGHT detection in the first 60s after the
        // GAME opens (s.enterMs, written once on fgIsGame open — NOT rewritten by
        // per-scene commits). Host tests may preset s.enterMs far in the past to
        // bypass warm-up; that preset survives because a later scene commit never
        // touches enterMs (RC3).
        long long gameAgeMs = nowMs - s.enterMs;
        bool baselineWarm = (gameAgeMs < 60000);

        if (fpsRatio > 0.9f && f.missRate < 0.02f) {
            int slopeHits = 0;
            if (f.dTaps_dt > kTapsSlopeArm) slopeHits++;
            if (f.dRunDelay_dt > kRunDelaySlopeArm) slopeHits++;
            if (gpuOk && f.dGpuBusy_dt > kGpuSlopeArm) slopeHits++;

            // During warm-up, require 3 hits instead of 2 (stricter).
            int requiredHits = baselineWarm ? 3 : 2;
            if (slopeHits >= requiredHits) {
                s_teamfight = clamp01(0.5f + slopeHits * 0.15f);
                // Degraded: slightly higher threshold (+0.05 margin).
                if (out.degraded) {
                    s_teamfight -= 0.05f;
                }
            }
        }
    }

    // ---- Pick winner ----
    float bestScore = 0.0f;
    Scene best = Scene::UNKNOWN;

    if (s_menu > bestScore)    { best = Scene::MENU;      bestScore = s_menu; }
    if (s_loading > bestScore)  { best = Scene::LOADING;   bestScore = s_loading; }
    if (s_combat > bestScore)   { best = Scene::COMBAT;    bestScore = s_combat; }
    if (s_teamfight > bestScore){ best = Scene::TEAMFIGHT; bestScore = s_teamfight; }

    // ---- Hysteresis / debounce ----
    // Escalation (->LOADING / ->TEAMFIGHT): immediate.
    // De-escalation: require consecutive N ticks.
    //
    // RC1 ordering: the de-escalation gates run FIRST and settle `proposed`; the
    // ping-pong lock runs LAST and only on the residual changes. A de-escalation
    // that a gate already confirmed (exitConfirmed) is NEVER reverted by the lock,
    // so a legitimate exit out of LOADING/TEAMFIGHT cannot be blocked by ping-pong.
    Scene proposed = best;
    bool exitConfirmed = false;

    // De-escalation: TEAMFIGHT -> COMBAT requires 4 consecutive flat-slope ticks.
    // RC2: streak is a per-instance field (was a function-local static, polluted
    // across SceneState instances and survived resetSceneStateForTest).
    if (s.committedScene == Scene::TEAMFIGHT && proposed == Scene::COMBAT) {
        bool slopesFlat = (std::abs(f.dTaps_dt) < kTapsSlopeArm &&
                           std::abs(f.dRunDelay_dt) < kRunDelaySlopeArm &&
                           (!gpuOk || std::abs(f.dGpuBusy_dt) < kGpuSlopeArm));
        if (slopesFlat) {
            s.teamfightExitStreak++;
            if (s.teamfightExitStreak >= kTeamfightExitStreak) {
                s.teamfightExitStreak = 0;
                exitConfirmed = true;   // exit earned; let proposed through
            } else {
                proposed = Scene::TEAMFIGHT;
            }
        } else {
            s.teamfightExitStreak = 0;
            proposed = Scene::TEAMFIGHT;
        }
    }

    // De-escalation: leaving LOADING requires fpsRatio >= 0.5 for 3 consecutive
    // ticks. RC1: engage whenever best no longer says LOADING (it may be MENU
    // during the slow fps-recovery ramp, not yet COMBAT) — gating on
    // proposed==COMBAT left the exit gate never triggered and the ping-pong lock
    // then froze the scene in LOADING.
    if (s.committedScene == Scene::LOADING && proposed != Scene::LOADING) {
        if (fpsRatio >= kLoadingExitFpsRatio) {
            s.loadingConvergeStreak++;
            if (s.loadingConvergeStreak >= kLoadingExitStreak) {
                s.loadingConvergeStreak = 0;
                exitConfirmed = true;   // fps sustained recovery; exit earned
            } else {
                proposed = Scene::LOADING;
            }
        } else {
            s.loadingConvergeStreak = 0;
            proposed = Scene::LOADING;
        }
    }

    // Confidence check (fail-closed).
    if (bestScore < kMinConfidence && proposed != Scene::BACKGROUND) {
        proposed = Scene::UNKNOWN;
        out.reason = "low_confidence";
    }

    // Ping-pong protection (W5 §5.4): only a TRUE direction reversal — best
    // bouncing back to the scene we were in *before* the current committed one
    // (A->B->A) — counts toward the 5s window. Monotonic escalation/de-escalation
    // (UNKNOWN->MENU->LOADING->COMBAT) never counts. A gate-confirmed exit is
    // skipped entirely above.
    if (proposed != s.committedScene && !exitConfirmed) {
        bool reversal = (proposed == s.prevCommittedScene);
        if (reversal) {
            if (nowMs - s.lastChangeMs > (long long)kPingPongWindowMs) {
                s.changesInWindow = 0;
            }
            s.changesInWindow++;
            if (s.changesInWindow > kPingPongMaxFlips) {
                // Too many real oscillations; lock to the committed scene.
                proposed = s.committedScene;
                out.reason = "ping_pong_lock";
            }
        }
    }

    // ---- Commit ----
    if (proposed != s.committedScene) {
        s.prevCommittedScene = s.committedScene;
        s.committedScene = proposed;
        s.sceneEnterMs = nowMs;   // RC3: scene-entry timebox; enterMs (game start) is left untouched
        s.sceneChangeCount++;
        s.lastChangeMs = nowMs;
    }

    out.scene = proposed;
    out.confidence01 = bestScore;

    // Record active signals.
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

} // namespace scenefuse
} // namespace aatpe
