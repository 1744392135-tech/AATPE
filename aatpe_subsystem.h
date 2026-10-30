#ifndef AATPE_SUBSYSTEM_H
#define AATPE_SUBSYSTEM_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aatpe {
namespace subsys {

enum class SubsystemState : int {
    INIT = 0,
    STARTING = 1,
    ACTIVE = 2,
    DEGRADED = 3,
    STOPPING = 4,
    STOPPED = 5,
    FAILED = 6
};

const char* subsystemStateName(SubsystemState s);

struct SubsystemHealth {
    std::atomic<SubsystemState> state{SubsystemState::INIT};
    std::atomic<time_t> lastSuccess{0};
    std::atomic<time_t> lastError{0};
    std::atomic<uint64_t> errorCount{0};
    std::atomic<uint64_t> recoveryCount{0};

    std::mutex reasonMutex;
    std::string lastErrorReason;

    void setState(SubsystemState s);
    void markSuccess();
    void markError(const std::string& reason);
    void markRecovery();
    SubsystemState getState() const { return state.load(std::memory_order_relaxed); }
    std::string getLastErrorReason();

    std::string toJson(const std::string& name);
};

class Registry {
public:
    static Registry& instance();

    SubsystemHealth& get(const std::string& name);

    void setState(const std::string& name, SubsystemState s);
    void markSuccess(const std::string& name);
    void markError(const std::string& name, const std::string& reason);
    void markRecovery(const std::string& name);

    std::vector<std::string> listNames();
    std::string toJson();
    void writeStateFile(const char* dir);

    int failedCount();
    int degradedCount();

private:
    Registry();
    std::mutex mMutex;
    std::map<std::string, std::unique_ptr<SubsystemHealth>> mEntries;
};

class StopToken {
public:
    StopToken();
    ~StopToken();
    StopToken(const StopToken&) = delete;
    StopToken& operator=(const StopToken&) = delete;

    bool open();
    void requestStop();
    bool shouldStop() const { return mStop.load(std::memory_order_acquire); }
    int fd() const { return mFd; }
    bool valid() const { return mFd >= 0; }
    void drain();
    void reset();

private:
    int mFd{-1};
    std::atomic<bool> mStop{false};
};

namespace safemode {

extern std::atomic<bool> gSafeModeActive;
extern std::atomic<int>  gSafeModeReason;
extern std::atomic<time_t> gSafeModeEntered;

enum SafeReason : int {
    NONE            = 0,
    EBPF_STREAK     = 1 << 0,
    SCHED_STREAK    = 1 << 1,
    CRASH_LOOP      = 1 << 2,
    MEM_PRESSURE    = 1 << 3,
    THERMAL_EMERG   = 1 << 4,
    SELF_OVERLOAD   = 1 << 5,
};

const char* reasonName(int bit);
std::string describeReasons(int bits);

void trigger(SafeReason r, const char* detail);
void resetStreak(SafeReason r);
void clearReason(SafeReason r);
void maybeRecover(std::time_t now);

bool isActive();
bool rtBoostAllowed();
bool mlPlacementAllowed();
bool aggressiveMigrationAllowed();

void writeStateFiles(const char* dir);

#ifdef AATPE_HOST_TEST
void setStreakThresholdForTest(int n);
void setRecoverCooldownSecForTest(int s);

extern int g_crashLoopObserveSecTestOverride;
#endif

}

namespace selfobs {

extern std::atomic<int> gSelfCpuPercent;
extern std::atomic<long> gSelfRssKb;
extern std::atomic<int> gSelfFdCount;
extern std::atomic<int> gSelfThreadCount;
extern std::atomic<bool> gThrottled;

void sampleSelf();

bool evaluateThrottle();

void writeStateFiles(const char* dir);

#ifdef AATPE_HOST_TEST
void setCpuBudgetPercentForTest(int pct);
void setSustainSecForTest(int s);
#include <string>
bool parseSelfStatCpuForTest(const std::string& afterParen,
                             long long& utimeOut, long long& stimeOut);
#endif

}

}
}

#endif
