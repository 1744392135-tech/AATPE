#ifndef AATPE_SCORING_INTERFACE_H
#define AATPE_SCORING_INTERFACE_H

#include <map>
#include <vector>

struct ThreadInfo;
struct Config;
struct CpuMetrics;

namespace aatpe_ml { struct ThreadFeatures; }

struct PoolSnapshot {
    std::vector<int> little;
    std::vector<int> mid;
    std::vector<int> big;
    std::vector<int> prime;
};

struct ScoringContext {
    const Config* cfg = nullptr;
    PoolSnapshot pools;
    std::map<int, CpuMetrics> cpuMetrics;

    int getCapacity(int cpuId) const;
    int getTemp(int cpuId) const;
    int getUtil(int cpuId) const;
};

ScoringContext buildScoringContext(const Config& cfg);

int computeScore(int cpuId, const ThreadInfo& ti);
int computeScore(int cpuId, const ThreadInfo& ti, int knownFromCpu, const ScoringContext& ctx);

aatpe_ml::ThreadFeatures extractMlFeaturesCtx(const ThreadInfo& ti, int targetCpu, const ScoringContext& ctx);

#endif
