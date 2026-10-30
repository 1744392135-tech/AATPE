#ifndef AATPE_PRE_BOOST_PREDICT_H
#define AATPE_PRE_BOOST_PREDICT_H

#include <cstdint>
#include <vector>

namespace aatpe {
namespace preboost {

struct PreBoostFeatures {
    float fpsNow;
    float fpsP99;
    float fpsTarget;
    float missRate;
    float sfLatAvgUs;
    float sfLatBudgetUs;
    float cpuUtilSlope;
    float headroom;
    bool  thermalEmergency;
    bool  passiveMode;
    bool  jankVoteActive;
};

struct PreBoostDecision {
    bool        arm;
    int         uclampMinPct;
    int         tierTarget;
    int         holdMaxMs;
    const char* reason;
};

PreBoostDecision predictPreBoost(const PreBoostFeatures& f);

float computeCpuSlope(const std::vector<int>& history);

PreBoostFeatures buildFeatures(float gameAvgFps, float gameP99Fps,
                               int gameFrameMissPct, int sfLatencyAvgUs,
                               const std::vector<int>& cpuHistory,
                               float sampleIntervalSec,
                               float fpsTargetFps, float sfLatBudgetUs,
                               float headroom01, bool headroomValid,
                               bool thermalEmergency, bool passiveMode, bool jankVoteActive);

typedef void (*BoostExecutorFn)(int level);
void setBoostExecutor(BoostExecutorFn fn);

bool preBoostTick(long long nowMs, const PreBoostFeatures& f);

bool isArmed();

int rejectedCount();

#ifdef AATPE_HOST_TEST
void resetForTest();
void setNowForTest(long long (*fn)());
#endif

}
}

#endif
