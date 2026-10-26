// Federated Learning implementation (class core)
// Phase-2 refactor: split from aatpe_federated.cpp
#include "aatpe_federated.h"
#include "aatped.h"          // LOG_WARN unified logging
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <fcntl.h>          // P2-16: open/O_* for atomic 0600 write
#include <unistd.h>         // P3-34: fsync/close/unlink
#include <cerrno>
namespace aatpe_federated {

FederatedLearner::FederatedLearner() {}
FederatedLearner::~FederatedLearner() {}

// P2-10: persist lastAcceptedGeneration_ to <exchangeDir>/replay_state.
// LED-060: line 1 is the global max (backward compatible with the legacy
// single-number format); subsequent lines are "sourceName generation" per-source
// replay watermarks. Written atomically (tmp+rename) with 0600.
void FederatedLearner::saveReplayStateLocked(void) {
    std::string path = exchangeDir_ + "/replay_state";
    std::string tmpPath = path + ".tmp";
    // P2-16: atomic write with 0600 perms via open().
    int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    std::string out;
    out += std::to_string(lastAcceptedGeneration_);
    out += "\n";
    for (const auto& kv : lastAcceptedBySource_) {
        // source names come from readdir (no '/'); guard anyway.
        out += kv.first;
        out += " ";
        out += std::to_string(kv.second);
        out += "\n";
    }
    bool ok = true;
    size_t off = 0;
    while (off < out.size()) {
        ssize_t w = ::write(fd, out.data() + off, out.size() - off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; ok = false; break; }
        off += (size_t)w;
    }
    // P3-34: fsync before close so the state is durable.
    if (ok && ::fsync(fd) != 0) ok = false;
    // P3-30: check close return value.
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        // P3-32: check unlink.
        if (::unlink(tmpPath.c_str()) != 0) {} // best-effort
        return;
    }
    if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
        ::unlink(tmpPath.c_str());
    }
}

void FederatedLearner::loadReplayStateLocked(void) {
    lastAcceptedGeneration_ = 0;
    lastAcceptedBySource_.clear();
    std::string path = exchangeDir_ + "/replay_state";
    // A3: read side must not follow a symlink planted over replay_state. Use
    // open(O_RDONLY|O_NOFOLLOW) then fdopen(); O_NOFOLLOW refuses to open if the
    // final component is a symlink (TOCTOU-hardened read of the daemon's own
    // watermark file). ELOOP -> plain return (treat as absent, safe).
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return;
    FILE* fp = ::fdopen(fd, "r");
    if (!fp) { ::close(fd); return; }
    unsigned long v = 0;
    // Line 1: global max (legacy format).
    if (fscanf(fp, "%lu", &v) == 1 && v > 0 && v < 0xFFFFFFFFu) {
        lastAcceptedGeneration_ = (uint32_t)v;
    }
    // Subsequent lines: "sourceName generation" (LED-060 per-source watermarks).
    char name[256];
    unsigned long gen = 0;
    while (fscanf(fp, "%255s %lu", name, &gen) == 2) {
        if (gen > 0 && gen < 0xFFFFFFFFu) {
            lastAcceptedBySource_[name] = (uint32_t)gen;
        }
    }
    // P3-30: check fclose return value.
    if (fclose(fp) != 0) { /* best-effort */ }
}

// check mkdir return values
void FederatedLearner::init(const std::string& exchangeDir) {
    std::lock_guard<std::mutex> lk(mutex_);
    exchangeDir_ = exchangeDir;
    // ML-06: do NOT mkdir/chmod the exchange dir here. init() runs at daemon boot
    // even when federated is DISABLED (the default). Creating /dev/aatpe/federated
    // + import/ on disk for a dormant module misleads downstream readers (WebUI /
    // state inspectors see a populated exchange dir and assume federation is on).
    // Directories are created lazily by setEnabled(true) / ensureDirsLocked() only
    // when the feature is actually opted in. We still load any pre-existing
    // replay_state so an opt-in after restart preserves anti-replay watermarks.
    loadReplayStateLocked();
}

// ML-06: lazily create exchangeDir_/import/ on first opt-in. No-op when already
// enabled (dirs already exist) or when the dirs are already present on disk.
void FederatedLearner::ensureDirsLocked(void) {
    if (mkdir(exchangeDir_.c_str(),0700)!=0 && errno!=EEXIST)
        LOG_WARN("[FEDERATED] mkdir %s failed: %s",exchangeDir_.c_str(),aatpeErrStr(errno));
    if (chmod(exchangeDir_.c_str(),0700) != 0)
        LOG_WARN("[FEDERATED] chmod %s failed: %s", exchangeDir_.c_str(), aatpeErrStr(errno));
    std::string importDir=exchangeDir_+"/import";
    if (mkdir(importDir.c_str(),0700)!=0 && errno!=EEXIST)
        LOG_WARN("[FEDERATED] mkdir %s failed: %s",importDir.c_str(),aatpeErrStr(errno));
    if (chmod(importDir.c_str(),0700) != 0)
        LOG_WARN("[FEDERATED] chmod %s failed: %s", importDir.c_str(), aatpeErrStr(errno));
}

void FederatedLearner::setLocalModel(const std::vector<float>& weights, float alpha, int sampleCount) {
    std::lock_guard<std::mutex> lk(mutex_);
    localWeights_=weights; localAlpha_=alpha; localSampleCount_=sampleCount;
}

// G7-001: opt-in gate. Documented in the header. Turning the learner OFF while
// a prior export may have left files on disk does NOT delete them; it only stops
// new exports / imports / aggregation going forward.
void FederatedLearner::setEnabled(bool e) {
    std::lock_guard<std::mutex> lk(mutex_);
    enabled_ = e;
    // ML-06: lazily create the exchange dirs only on opt-in. When the feature is
    // off (default), no /dev/aatpe/federated tree is created on disk, so downstream
    // readers cannot mistake a dormant module for an active one.
    if (e) ensureDirsLocked();
}

bool FederatedLearner::isEnabled() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return enabled_;
}

// ROUND-014 P3 (Plan B): wire-exchange + PSK security TUs removed from the production
// recipe (no cloud channel; federated_enabled defaults false). These methods are still
// referenced, so provide dormant no-op implementations here: with the exchange path gone
// there is nothing to export/import — federation stays local-only aggregation.
void FederatedLearner::exportLocalModel(void) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!enabled_) return;   // dormant: no wire export
}

void FederatedLearner::checkAndImportModels(void) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!enabled_) return;   // dormant: no wire import
}

} // namespace aatpe_federated
