// pre_boost_predict.h - C1 predictive pre-boost core (PURE LOGIC, host-testable).
//
// Per W2 Part B spec: predict "about to enter heavy load" BEFORE jank actually
// happens, and arm a bounded time-boxed pre-boost. All signals are already
// available; this module is a pure decision layer with zero sysfs / global
// state in the predict() function itself. The time-box state machine keeps
// file-static state that is reset via resetForTest() under AATPE_HOST_TEST.
#ifndef AATPE_PRE_BOOST_PREDICT_H
#define AATPE_PRE_BOOST_PREDICT_H

#include <cstdint>
#include <vector>

namespace aatpe {
namespace preboost {

// Input features snapshot (all from existing state, zero new sysfs).
struct PreBoostFeatures {
    float fpsNow;           // rolling mean fps (from game_avg_fps)
    float fpsP99;           // p99 fps (from game_p99_fps)
    float fpsTarget;        // target fps (device config, default 60)
    float missRate;         // recent window missRate [0,1] (from game_frame_miss pct)
    float sfLatAvgUs;       // SurfaceFlinger latency mean (microseconds, from sf_latency_avg)
    float sfLatBudgetUs;    // frame budget (microseconds, default 16667 for 60fps)
    float cpuUtilSlope;     // OLS slope %/s on foreground game cores
    float headroom;         // [0,1] fraction distance to critical wall
    bool  thermalEmergency; // hard wall already tripped
    bool  passiveMode;      // user selected passive control mode
    bool  jankVoteActive;    // reactive jank vote already armed (hand off to it)
};

struct PreBoostDecision {
    bool        arm;            // whether to pre-arm
    int         uclampMinPct;   // target uclamp_min percent (0-100)
    int         tierTarget;     // expected cluster tier (0=LITTLE,1=MID,2=BIG,3=PRIME)
    int         holdMaxMs;      // time-box hard cap
    const char* reason;         // human-readable reason for the decision
};

// Pure function: given features -> decision. No sysfs, no global state.
// This is the host-testable core.
PreBoostDecision predictPreBoost(const PreBoostFeatures& f);

// ---- Pure helpers (host-testable, zero globals) ---------------------------

// Compute OLS slope from a CPU utilization history vector (samples in time order).
// Returns slope in %/s (per-sample delta). Returns 0.0 if fewer than 3 samples.
float computeCpuSlope(const std::vector<int>& history);

// Build PreBoostFeatures from raw state values.
// Correct state keys (per frame.cpp): game_avg_fps, game_p99_fps, game_frame_miss (pct),
// sf_latency_avg (us). This centralizes key-name mapping so tests can catch typos.
// cpuHistory: recent CPU utilization samples for slope computation.
// sampleIntervalSec: real seconds between consecutive samples (for %/s conversion).
// fpsTargetUs: target frame budget in microseconds (default 16667 for 60fps).
// headroomValid: false if headroom sensor is unavailable (fail-closed 0.0).
PreBoostFeatures buildFeatures(float gameAvgFps, float gameP99Fps,
                               int gameFrameMissPct, int sfLatencyAvgUs,
                               const std::vector<int>& cpuHistory,
                               float sampleIntervalSec,
                               float fpsTargetFps, float sfLatBudgetUs,
                               float headroom01, bool headroomValid,
                               bool thermalEmergency, bool passiveMode, bool jankVoteActive);

// ---- Executor seam (callback; host tests stub this) ------------------------
// Called when armed state transitions. On arm: level=uclampMinPct. On disarm: level=0.
typedef void (*BoostExecutorFn)(int level);
void setBoostExecutor(BoostExecutorFn fn);

// ---- Time-box state machine ------------------------------------------------
// Call once per decision cycle with nowMs.
// Returns true if the pre-boost should currently be active.
bool preBoostTick(long long nowMs, const PreBoostFeatures& f);

// Returns whether pre-boost is currently armed (read-only state).
bool isArmed();

// Returns count of rejected arms due to rate limiting in current window.
int rejectedCount();

#ifdef AATPE_HOST_TEST
// Test seams: reset all file-static state, inject a clock function.
void resetForTest();
void setNowForTest(long long (*fn)());
#endif

} // namespace preboost
} // namespace aatpe

#endif // AATPE_PRE_BOOST_PREDICT_H
