#include "pre_boost_predict.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <chrono>
#include <mutex>

namespace aatpe {
namespace preboost {

static constexpr int   kPreBoostMaxMs       = 15000;
static constexpr float kFrameMarginArmLo   = 0.0f;
static constexpr float kFrameMarginArmHi    = 0.10f;
static constexpr float kLatencyBudgetRatio  = 0.6f;
static constexpr float kCpuSlopeArm         = 1.0f;
static constexpr float kMissRateNoJank      = 0.02f;
static constexpr float kHeadroomArm         = 0.3f;
static constexpr float kHeadroomDisarm     = 0.15f;
static constexpr float kMissRateTakeover    = 0.05f;
static constexpr int   kFalseAlarmStreak   = 3;
static constexpr int   kUclampMinPct        = 80;
static constexpr int   kTierTarget          = 2;
static constexpr int   kArmsPerWindow       = 3;
static constexpr long long kArmWindowMs    = 60000;

static std::mutex     s_preboostMutex;
static bool           s_armed            = false;
static long long      s_armStartMs       = 0;
static int            s_falseAlarmStreak = 0;
static int            s_rejectedCount    = 0;
static std::deque<long long> s_armHistory;
static BoostExecutorFn s_executor        = nullptr;

#ifdef AATPE_HOST_TEST
static long long (*s_testNowFn)() = nullptr;
static long long realNowMs() {
    if (s_testNowFn) return s_testNowFn();
    return 0;
}
#else
static long long realNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

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
    f.missRate       = (float)gameFrameMissPct / 100.0f;
    f.sfLatAvgUs     = (float)sfLatencyAvgUs;
    f.sfLatBudgetUs  = (sfLatBudgetUs > 0.0f) ? sfLatBudgetUs : 16667.0f;
    f.cpuUtilSlope   = (sampleIntervalSec > 0.1f)
        ? computeCpuSlope(cpuHistory) / sampleIntervalSec : 0.0f;
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

PreBoostDecision predictPreBoost(const PreBoostFeatures& f)
{
    PreBoostDecision d{};
    d.arm          = false;
    d.uclampMinPct = 0;
    d.tierTarget   = 0;
    d.holdMaxMs    = kPreBoostMaxMs;
    d.reason       = "no-trigger";

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

    float frameMargin = 0.0f;
    if (f.fpsTarget > 0.0f) {
        frameMargin = (f.fpsTarget - f.fpsNow) / f.fpsTarget;
    }

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

bool preBoostTick(long long nowMs, const PreBoostFeatures& f)
{
    std::lock_guard<std::mutex> lk(s_preboostMutex);
    if (nowMs == 0) nowMs = realNowMs();

    while (!s_armHistory.empty() && (nowMs - s_armHistory.front()) > kArmWindowMs) {
        s_armHistory.pop_front();
    }

    PreBoostDecision dec = predictPreBoost(f);

    if (s_armed) {
        bool disarm = false;

        if ((nowMs - s_armStartMs) >= kPreBoostMaxMs) {
            disarm = true;
        }
        else if (f.missRate >= kMissRateTakeover) {
            disarm = true;
        }
        else if (f.headroom < kHeadroomDisarm) {
            disarm = true;
        }
        else if (f.jankVoteActive) {
            disarm = true;
        }
        else if (f.passiveMode || f.thermalEmergency) {
            disarm = true;
        }
        else {
            float frameMargin = (f.fpsTarget > 0.0f)
                ? (f.fpsTarget - f.fpsNow) / f.fpsTarget : 0.0f;
            bool latRecovered = (f.sfLatAvgUs < 0.5f * f.sfLatBudgetUs);
            bool slopeFlat    = (f.cpuUtilSlope < 0.5f && f.cpuUtilSlope > -0.5f);
            bool fpsStable    = (frameMargin < 0.05f);
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
            if (s_executor) s_executor(0);
        }
    } else {
        if (dec.arm) {
            if ((int)s_armHistory.size() >= kArmsPerWindow) {
                s_rejectedCount++;
            } else {
                s_armed = true;
                s_armStartMs = nowMs;
                s_falseAlarmStreak = 0;
                s_armHistory.push_back(nowMs);
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

}
}
