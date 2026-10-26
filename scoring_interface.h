#ifndef AATPE_SCORING_INTERFACE_H
#define AATPE_SCORING_INTERFACE_H

#include <map>
#include <vector>

struct ThreadInfo;
struct Config;
struct CpuMetrics;

namespace aatpe_ml { struct ThreadFeatures; }

// P0-opt1: Pool snapshot hoisted from scoring.cpp (was static).
// Copied once under g_topologyRwMutex at decidePlacement entry, then reused
// for every (thread, cpu) candidate without re-taking the topology lock.
struct PoolSnapshot {
    std::vector<int> little;
    std::vector<int> mid;
    std::vector<int> big;
    // G7 R004-B2: PRIME (highest, super-big) cluster cpus. Empty on 2-cluster
    // devices (no prime). poolScore() rewards interactive threads landing here.
    std::vector<int> prime;
};

// P0-opt1: Full scoring context — all read-only snapshots taken ONCE per
// decidePlacement() call, then passed through the entire per-CPU scoring loop.
// This eliminates 40-60% of mutex lock/unlock traffic on the hot path:
//   - g_cpuMetricsMutex: was taken per cpuId in capacityScore/thermalScore/
//     contentionPenalty/extractMlFeatures (4× per candidate, ~32× per decision)
//   - g_topologyRwMutex: was taken per candidate in poolScore/migrationPenaltyCore/
//     ADRC block (3× per candidate, ~24× per decision)
//   - g_cfgMutex: was taken per candidate in frameScore/placementSuccessScore
//     (2× per candidate, ~16× per decision)
struct ScoringContext {
    const Config* cfg = nullptr;
    PoolSnapshot pools;
    std::map<int, CpuMetrics> cpuMetrics;  // snapshot of g_cpuMetrics

    // Convenience accessors (no locks — reads from snapshot)
    int getCapacity(int cpuId) const;
    int getTemp(int cpuId) const;
    int getUtil(int cpuId) const;
};

// Build a ScoringContext by snapshotting all needed globals once.
// Caller passes an already-snapshotted Config (decidePlacement receives it by ref).
ScoringContext buildScoringContext(const Config& cfg);

int computeScore(int cpuId, const ThreadInfo& ti);
// P0-opt1: Context-aware overload — uses pre-snapshotted pools/metrics/config
// instead of re-locking for every candidate.
int computeScore(int cpuId, const ThreadInfo& ti, int knownFromCpu, const ScoringContext& ctx);

// P0-opt1: Context-aware ML feature extraction (avoids re-taking g_cpuMetricsMutex).
aatpe_ml::ThreadFeatures extractMlFeaturesCtx(const ThreadInfo& ti, int targetCpu, const ScoringContext& ctx);

#endif
