#include "aatpe_federated.h"
#include "aatped.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
namespace aatpe_federated {

FederatedLearner::FederatedLearner() {}
FederatedLearner::~FederatedLearner() {}

void FederatedLearner::saveReplayStateLocked(void) {
    std::string path = exchangeDir_ + "/replay_state";
    std::string tmpPath = path + ".tmp";
    int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    std::string out;
    out += std::to_string(lastAcceptedGeneration_);
    out += "\n";
    for (const auto& kv : lastAcceptedBySource_) {
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
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        if (::unlink(tmpPath.c_str()) != 0) {}
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
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return;
    FILE* fp = ::fdopen(fd, "r");
    if (!fp) { ::close(fd); return; }
    unsigned long v = 0;
    if (fscanf(fp, "%lu", &v) == 1 && v > 0 && v < 0xFFFFFFFFu) {
        lastAcceptedGeneration_ = (uint32_t)v;
    }
    char name[256];
    unsigned long gen = 0;
    while (fscanf(fp, "%255s %lu", name, &gen) == 2) {
        if (gen > 0 && gen < 0xFFFFFFFFu) {
            lastAcceptedBySource_[name] = (uint32_t)gen;
        }
    }
    if (fclose(fp) != 0) {  }
}

void FederatedLearner::init(const std::string& exchangeDir) {
    std::lock_guard<std::mutex> lk(mutex_);
    exchangeDir_ = exchangeDir;
    loadReplayStateLocked();
}

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

void FederatedLearner::setEnabled(bool e) {
    std::lock_guard<std::mutex> lk(mutex_);
    enabled_ = e;
    if (e) ensureDirsLocked();
}

bool FederatedLearner::isEnabled() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return enabled_;
}

void FederatedLearner::exportLocalModel(void) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!enabled_) return;
}

void FederatedLearner::checkAndImportModels(void) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!enabled_) return;
}

}
