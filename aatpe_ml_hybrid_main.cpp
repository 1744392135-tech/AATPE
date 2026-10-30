#include "aatpe_ml_hybrid.h"
#include "aatped.h"
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <algorithm>
#include <unistd.h>
namespace aatpe_ml_hybrid {

HybridLearner::HybridLearner() {}
HybridLearner::~HybridLearner() {}

void HybridLearner::init(int numPools, int featureNumPools) {
    std::lock_guard<std::mutex> lock(mutex_);
    numPools_ = numPools;
    featureNumPools_ = (featureNumPools > 0) ? featureNumPools : numPools;
    poolDeltas_.clear(); poolDeltas_.resize(numPools);
    prevCpuRuntimeNs_.clear(); prevCpuRuntimeNs_.resize(0);
    prevSnapshots_.clear();
    prevTickTimestampNs_ = 0;
    alpha_ = 0.0f;
    initialized_ = true;
    LOG_INFO("[ML-HYBRID] initialized with %d pools (feature-normalizer=%d)",
             numPools, featureNumPools_);
}

void HybridLearner::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled != enabled_) {
        enabled_ = enabled;
        LOG_INFO("[ML-HYBRID] %s", enabled ? "ENABLED" : "DISABLED");
        if (!enabled) prevSnapshots_.clear();
    }
}

void HybridLearner::tick(const std::vector<ThreadCpuSnapshot>& threadSnapshots,
                          const std::vector<uint64_t>& cpuRuntimeNs, int cpuCount,
                          const std::vector<float>& cpuThermalNorm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !initialized_ || poolDeltas_.empty()) return;
    if (aatpe_ml::g_mlTrainingPausedByThermal.load(std::memory_order_relaxed)) {
        return;
    }
    tickCount_++;

    struct ThreadDelta {
        int tid, cpu, pool;
        int priority, nice, role;
        bool migrated;
        uint64_t deltaRunNs,deltaTimeNs; float runRatio;
        uint64_t deltaSwitches;
    };
    std::vector<ThreadDelta> deltas;
    deltas.reserve(threadSnapshots.size());
    uint64_t nowNs = 0;
    for (const auto& snap : threadSnapshots) {
        if (snap.timestamp > nowNs) nowNs = snap.timestamp;
        auto it = prevSnapshots_.find(snap.tid);
        if (it != prevSnapshots_.end()) {
            const auto& prev = it->second;
            uint64_t dr = (snap.runTimeNs >= prev.runTimeNs) ? (snap.runTimeNs - prev.runTimeNs) : 0;
            uint64_t dt = (snap.timestamp > prev.timestamp) ? (snap.timestamp - prev.timestamp) : 1;
            if (dr > 0 && dt > 0) {
                ThreadDelta td; td.tid=snap.tid; td.cpu=snap.cpu; td.pool=snap.pool;
                td.priority = snap.priority;
                td.nice     = snap.nice;
                td.role     = snap.role;
                td.migrated = (prev.cpu >= 0 && snap.cpu >= 0 && prev.cpu != snap.cpu);
                td.deltaRunNs=dr; td.deltaTimeNs=dt;
                td.runRatio=std::min(1.0f,(float)dr/(float)dt);
                td.deltaSwitches = (snap.nrSwitches >= prev.nrSwitches)
                                 ? (snap.nrSwitches - prev.nrSwitches) : 0ULL;
                deltas.push_back(td);
            }
        }
        prevSnapshots_[snap.tid] = snap;
    }

    if (prevSnapshots_.size() > (size_t)kMaxTrackedThreads) {
        size_t toEvict = prevSnapshots_.size() - (size_t)kMaxTrackedThreads/2;
        std::vector<std::pair<uint64_t,int>> ordered;
        ordered.reserve(prevSnapshots_.size());
        for (const auto& kv : prevSnapshots_) ordered.push_back({kv.second.timestamp, kv.first});
        std::sort(ordered.begin(), ordered.end(),
                  [](const std::pair<uint64_t,int>& a, const std::pair<uint64_t,int>& b){ return a.first < b.first; });
        for (size_t e = 0; e < toEvict && e < ordered.size(); e++) {
            prevSnapshots_.erase(ordered[e].second);
        }
    }

    if ((int)prevCpuRuntimeNs_.size() < cpuCount) prevCpuRuntimeNs_.resize(cpuCount, 0);
    uint64_t wallNs = 500000000ULL;
    if (prevTickTimestampNs_ > 0 && nowNs > prevTickTimestampNs_) wallNs = nowNs - prevTickTimestampNs_;
    prevTickTimestampNs_ = nowNs;
    std::vector<float> cpuUtilDelta(cpuCount, 0.0f);
    for (int cpu=0; cpu<cpuCount && cpu<(int)cpuRuntimeNs.size(); cpu++) {
        if (cpu < (int)prevCpuRuntimeNs_.size()) {
            uint64_t prev=prevCpuRuntimeNs_[cpu], cur=cpuRuntimeNs[cpu];
            if (cur>=prev && wallNs>0) cpuUtilDelta[cpu]=std::min(1.0f,(float)(cur-prev)/(float)wallNs);
            prevCpuRuntimeNs_[cpu]=cur;
        }
    }

    int updatesThisTick = 0;
    for (const auto& td : deltas) {
        if (td.cpu<0||td.cpu>=cpuCount) continue;
        int pool = td.pool;
        if (pool<0||pool>=numPools_) pool=0;
        if (pool>=numPools_) pool=numPools_-1;

        float features[15]={0};
        features[0]=1.0f;
        features[1]=std::min(1.0f,(float)td.deltaRunNs/1000.0f/50000.0f);
        float swRate = (td.deltaTimeNs > 0)
            ? (float)td.deltaSwitches / ((float)td.deltaTimeNs / 1.0e9f) : 0.0f;
        features[2] = std::min(1.0f, std::max(0.0f, swRate / aatpe::hybrid_const::kSwitchesPerSecScale));
        features[3]=cpuUtilDelta[td.cpu];
        features[4]=std::min(1.0f,std::max(0.0f, 1.0f - (float)td.priority/99.0f));
        features[5]=std::min(1.0f,std::max(0.0f, 1.0f - ((float)td.nice + 20.0f)/39.0f));
        features[6]=(td.role==3||td.role==2)?1.0f:0.0f;
        float therm=0.5f;
        if (td.cpu>=0 && td.cpu<(int)cpuThermalNorm.size()) therm=cpuThermalNorm[td.cpu];
        features[7]=std::min(1.0f,std::max(0.0f, therm));
        features[8] = poolFeatureForTier(pool, featureNumPools_);
        features[9] = (((td.tid + tickCount_) & 3) == 0) ? 0.0f : 1.0f;
        features[10]=td.migrated?1.0f:0.0f;
        features[11]=features[1]*features[8];
        features[12]=features[1]*features[3];
        features[13]=features[8]*features[7];
        features[14]=features[1]*features[7];
        sgdUpdate(pool, features, 1.0f - td.runRatio);
        updatesThisTick++;
    }
    totalUpdates_ += updatesThisTick;
    recomputeAlphaLocked();
    if (tickCount_%20==0 && updatesThisTick>0)
        LOG_DEBUG("[ML-HYBRID] tick #%d: %d threads, %d updates, total=%d alpha=%.4f",
                  tickCount_, (int)deltas.size(), updatesThisTick, totalUpdates_, (double)alpha_);
}

void HybridLearner::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& d:poolDeltas_) { memset(d.weights,0,sizeof(d.weights)); d.updateCount=0; }
    totalUpdates_=0; tickCount_=0; prevSnapshots_.clear();
    alpha_=0.0f;
    LOG_INFO("[ML-HYBRID] reset all delta weights");
}

HybridPoolDelta HybridLearner::getPoolDelta(int pool) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pool>=0&&pool<(int)poolDeltas_.size()) return poolDeltas_[pool];
    return HybridPoolDelta();
}

std::string HybridLearner::getStatusString() const {
    std::lock_guard<std::mutex> lock(mutex_);
    char buf[256];
    snprintf(buf,sizeof(buf),"hybrid=%s pools=%d ticks=%d updates=%d alpha=%.4f",
             enabled_?"on":"off",(int)poolDeltas_.size(),tickCount_,totalUpdates_,(double)alpha_);
    return std::string(buf);
}

HybridLearner& instance() { static HybridLearner inst; return inst; }
}
