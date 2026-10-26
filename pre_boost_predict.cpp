// pre_boost_predict.cpp - C1 predictive pre-boost core implementation.
//
// Per W2 Part B: pure predict() + time-boxed state machine.
// All thresholds per W2 B.3/B.5/B.6 spec.
#include "pre_boost_predict.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <chrono>
#include <mutex>

namespace aatpe {
namespace preboost {

// ---- Tunables (per W2 spec) ----------------------------------------------
static constexpr int   kPreBoostMaxMs       = 15000;  // B.5 hard cap 15s
static constexpr float kFrameMarginArmLo   = 0.0f;   // B.3 arm: margin >= 0%
static constexpr float kFrameMarginArmHi    = 0.10f;  // B.3 arm: margin <= 10%
static constexpr float kLatencyBudgetRatio  = 0.6f;   // B.3 arm: lat > 60% budget
static constexpr float kCpuSlopeArm         = 1.0f;   // B.3 arm: slope > 1.0 %/s
static constexpr float kMissRateNoJank      = 0.02f;  // B.3 arm: miss < 2%
static constexpr float kHeadroomArm         = 0.3f;   // B.6: headroom > 30% to arm
static constexpr float kHeadroomDisarm     = 0.15f;  // B.6: headroom < 15% to disarm
static constexpr float kMissRateTakeover    = 0.05f;  // B.3: miss >= 5% -> hand off
static constexpr int   kFalseAlarmStreak   = 3;       // B.5: 3 consecutive false-alarm beats
static constexpr int   kUclampMinPct        = 80;     // B.4: 80% uclamp target
static constexpr int   kTierTarget          = 2;     // B.4: BIG tier (2)
static constexpr int   kArmsPerWindow       = 3;      // B.6: max 3 arms/minute
static constexpr long long kArmWindowMs    = 60000;  // B.6: 1 minute window

// ---- Time-box state (file-static; reset under test) ------------------------
// ML-05: the whole state machine used to be guarded only by "the main loop is
// the only caller". That informal contract is invisible to a future caller and
// gives TSan nothing to check against. A small fast mutex now serializes
// preBoostTick/isArmed/rejectedCount/setBoostExecutor; the lock is never held
// across blocking I/O (the executor callback is a one-way notification into the
// uclamp writer and never re-enters this module), so there is no deadlock cycle.
static std::mutex     s_preboostMutex;
static bool           s_armed            = false;
static long long      s_armStartMs       = 0;
static int            s_falseAlarmStreak = 0;
static int            s_rejectedCount    = 0;
static std::deque<long long> s_armHistory;  // timestamps of recent arms
static BoostExecutorFn s_executor        = nullptr;

#ifdef AATPE_HOST_TEST
static long long (*s_testNowFn)() = nullptr;
static long long realNowMs() {
    if (s_testNowFn) return s_testNowFn();
    return 0; // in host test without injected clock, caller must pass nowMs
}
#else
// R2-L1: release fallback must return a REAL monotonic clock. Previously this was
// `return 0`, so a production caller that omitted nowMs() left nowMs==0 forever:
// s_armStartMs (set from that same 0) made `(nowMs - s_armStartMs) >= kPreBoostMaxMs`
// evaluate to 0 (or negative), the 15s timebox NEVER fired, and the pre-boost arm
// leaked past its hard cap. steady_clock is wall-jump-proof and monotonic; when the
// caller DOES pass nowMs this fallback is never used, so behavior is unchanged.
static long long realNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

// ---- Pure helpers ----------------------------------------------------------

float computeCpuSlope(const std::vector<int>& history)
{
    int n = (int)history.size();
    if (n < 3) return 0.0f;
    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    for (int i = 0; i < n; i++) {
        float xi = (float)i;
        float yi = (float)history[i];
        sumX += xi;
        sumY += yi;
        sumXY += xi * yi;
        sumX2 += xi * xi;
    }
    float denom = (float)n * sumX2 - sumX * sumX;
    if (std::fabs(denom) < 0.001f) return 0.0f;
    float slope = ((float)n * sumXY - sumX * sumY) / denom;
    return slope;
}

PreBoostFeatures buildFeatures(float gameAvgFps, float gameP99Fps,
                               int gameFrameMissPct, int sfLatencyAvgUs,
                               const std::vector<int>& cpuHistory,
                               float sampleIntervalSec,
                               float fpsTargetFps, float sfLatBudgetUs,
                               float headroom01, bool headroomValid,
                               bool thermalEmergency, bool passiveMode, bool jankVoteActive)
{
    PreBoostFeatures f{};
    f.fpsNow         = gameAvgFps;
    f.fpsP99         = gameP99Fps;
    f.fpsTarget      = (fpsTargetFps > 0.0f) ? fpsTargetFps : 60.0f;
    f.missRate       = (float)gameFrameMissPct / 100.0f;  // pct -> [0,1]
    f.sfLatAvgUs     = (float)sfLatencyAvgUs;
    f.sfLatBudgetUs  = (sfLatBudgetUs > 0.0f) ? sfLatBudgetUs : 16667.0f;
    // Slope: per-sample OLS delta -> %/s using real sample interval.
    // Fail-safe: if interval invalid (<=0) or too few samples, slope=0 (don't arm).
    f.cpuUtilSlope   = (sampleIntervalSec > 0.1f)
        ? computeCpuSlope(cpuHistory) / sampleIntervalSec : 0.0f;
    // Headroom: if sensor invalid, fail-closed (0.0 = too hot to arm).
    f.headroom       = headroomValid ? headroom01 : 0.0f;
    f.thermalEmergency = thermalEmergency;
    f.passiveMode    = passiveMode;
    f.jankVoteActive = jankVoteActive;
    return f;
}

void setBoostExecutor(BoostExecutorFn fn)
{
    std::lock_guard<std::mutex> lk(s_preboostMutex);
    s_executor = fn;
}

// ---- Pure predict() --------------------------------------------------------
PreBoostDecision predictPreBoost(const PreBoostFeatures& f)
{
    PreBoostDecision d{};
    d.arm          = false;
    d.uclampMinPct = 0;
    d.tierTarget   = 0;
    d.holdMaxMs    = kPreBoostMaxMs;
    d.reason       = "no-trigger";

    // Safety gates (B.6): passive, thermal emergency, hard wall
    if (f.passiveMode) {
        d.reason = "passive";
        return d;
    }
    if (f.thermalEmergency) {
        d.reason = "thermal-emergency";
        return d;
    }
    if (f.jankVoteActive) {
        d.reason = "jank-vote-active";
        return d;
    }

    // Frame margin: (fpsTarget - fpsNow) / fpsTarget
    float frameMargin = 0.0f;
    if (f.fpsTarget > 0.0f) {
        frameMargin = (f.fpsTarget - f.fpsNow) / f.fpsTarget;
    }

    // Arm conditions (all must hold, B.3):
    bool condMargin = (frameMargin >= kFrameMarginArmLo) &&
                      (frameMargin <= kFrameMarginArmHi);
    bool condLat    = (f.sfLatBudgetUs > 0.0f) &&
                      (f.sfLatAvgUs > kLatencyBudgetRatio * f.sfLatBudgetUs);
    bool condSlope  = (f.cpuUtilSlope > kCpuSlopeArm);
    bool condMiss   = (f.missRate < kMissRateNoJank);
    bool condHeat   = (f.headroom > kHeadroomArm);

    if (condMargin && condLat && condSlope && condMiss && condHeat) {
        d.arm          = true;
        d.uclampMinPct = kUclampMinPct;
        d.tierTarget   = kTierTarget;
        d.reason       = "trend-worsening-no-jank-yet";
    }

    return d;
}

// ---- Time-box tick ---------------------------------------------------------
bool preBoostTick(long long nowMs, const PreBoostFeatures& f)
{
    std::lock_guard<std::mutex> lk(s_preboostMutex);  // ML-05
    // If no injected clock and no explicit nowMs, use realNowMs fallback.
    if (nowMs == 0) nowMs = realNowMs();

    // Rate limiting: prune old arm history, count recent arms
    while (!s_armHistory.empty() && (nowMs - s_armHistory.front()) > kArmWindowMs) {
        s_armHistory.pop_front();
    }

    PreBoostDecision dec = predictPreBoost(f);

    if (s_armed) {
        // Already armed — check disarm conditions (B.3 disarm / B.5 early release).
        bool disarm = false;

        // Time-box hard cap
        if ((nowMs - s_armStartMs) >= kPreBoostMaxMs) {
            disarm = true;
        }
        // Real jank -> hand off to reactive vote
        else if (f.missRate >= kMissRateTakeover) {
            disarm = true;
        }
        // Hot -> immediate disarm
        else if (f.headroom < kHeadroomDisarm) {
            disarm = true;
        }
        // Reactive vote took over
        else if (f.jankVoteActive) {
            disarm = true;
        }
        // Passive mode / emergency
        else if (f.passiveMode || f.thermalEmergency) {
            disarm = true;
        }
        // False alarm: we armed expecting load, but it never materialized.
        // Correct signals: latency dropped back to baseline, CPU slope flattened,
        // fps still near target (no actual jank). If fps drops significantly
        // (large frameMargin), that's REAL load, not false alarm.
        else {
            float frameMargin = (f.fpsTarget > 0.0f)
                ? (f.fpsTarget - f.fpsNow) / f.fpsTarget : 0.0f;
            bool latRecovered = (f.sfLatAvgUs < 0.5f * f.sfLatBudgetUs);
            bool slopeFlat    = (f.cpuUtilSlope < 0.5f && f.cpuUtilSlope > -0.5f);
            bool fpsStable    = (frameMargin < 0.05f);  // within 5% of target
            if (latRecovered && slopeFlat && fpsStable) {
                s_falseAlarmStreak++;
                if (s_falseAlarmStreak >= kFalseAlarmStreak) {
                    disarm = true;
                }
            } else {
                s_falseAlarmStreak = 0;
            }
        }

        if (disarm) {
            s_armed = false;
            s_falseAlarmStreak = 0;
            // Notify executor: disarm (level=0)
            if (s_executor) s_executor(0);
        }
    } else {
        // Not armed — check if we should arm
        if (dec.arm) {
            if ((int)s_armHistory.size() >= kArmsPerWindow) {
                // Rate-limited: refuse
                s_rejectedCount++;
            } else {
                s_armed = true;
                s_armStartMs = nowMs;
                s_falseAlarmStreak = 0;
                s_armHistory.push_back(nowMs);
                // Notify executor: arm with uclamp level
                if (s_executor) s_executor(dec.uclampMinPct);
            }
        }
    }

    return s_armed;
}

bool isArmed() {
    std::lock_guard<std::mutex> lk(s_preboostMutex);
    return s_armed;
}

int rejectedCount() {
    std::lock_guard<std::mutex> lk(s_preboostMutex);
    return s_rejectedCount;
}

#ifdef AATPE_HOST_TEST
void resetForTest()
{
    std::lock_guard<std::mutex> lk(s_preboostMutex);
    s_armed            = false;
    s_armStartMs       = 0;
    s_falseAlarmStreak = 0;
    s_rejectedCount    = 0;
    s_armHistory.clear();
    s_testNowFn        = nullptr;
    s_executor         = nullptr;
}

void setNowForTest(long long (*fn)())
{
    s_testNowFn = fn;
}
#endif

} // namespace preboost
} // namespace aatpe
