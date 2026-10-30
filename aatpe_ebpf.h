#ifndef AATPE_EBPF_H
#define AATPE_EBPF_H

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <functional>
#include <deque>
#include <thread>
#include <new>
#include <memory>

namespace aatpe_ebpf {

std::vector<int> parseCpuOnlineMask(const std::string& text);
std::vector<int> readOnlineCpuIds();

enum class EventType : uint32_t {
    FORK   = 1,
    EXIT   = 2,
    SWITCH = 3,
    EXEC   = 4,
    RUNQ   = 5,
};

struct EventHeader {
    uint32_t type = 0;
    uint32_t cpu = 0;
    uint64_t timestampNs = 0;
};

struct ForkEvent {
    EventHeader hdr;
    uint32_t parentPid = 0;
    uint32_t childPid = 0;
    uint32_t childTid = 0;
};

struct ExitEvent {
    EventHeader hdr;
    uint32_t pid = 0;
    uint32_t tid = 0;
};

struct SwitchEvent {
    EventHeader hdr;
    uint32_t prevPid = 0;
    uint32_t nextPid = 0;
    char prevComm[16] = {0};
    char nextComm[16] = {0};
};

struct ExecEvent {
    EventHeader hdr;
    uint32_t pid = 0;
    uint32_t tid = 0;
    char comm[16] = {0};
};

struct RunqEvent {
    EventHeader hdr;
    uint32_t pid = 0;
    uint32_t targetCpu = 0;
    uint32_t wakerTid = 0;
    uint32_t pad = 0;
    uint64_t delayNs = 0;
};

struct AnyEvent {
    EventHeader hdr;
    union Data {
        ForkEvent fork;
        ExitEvent exit;
        SwitchEvent sw;
        ExecEvent exec;
        RunqEvent runq;
        Data() { new (&fork) ForkEvent(); }
    } data;
    AnyEvent() : hdr{}, data{} {}
};

template <typename T>
class SpscBoundedQueue {
public:
    explicit SpscBoundedQueue(size_t capacity)
        : cap_(roundUpPow2(capacity)),
          mask_(cap_ - 1),
          buf_(new T[cap_]) {}

    bool push(const T& ev) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= cap_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[tail & mask_] = ev;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    size_t popBatch(std::vector<T>& out, size_t budget) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t avail = tail - head;
        if (avail == 0) return 0;
        size_t n = avail;
        if (budget > 0 && n > budget) n = budget;
        out.reserve(out.size() + n);
        for (size_t i = 0; i < n; ++i) {
            out.push_back(std::move(buf_[(head + i) & mask_]));
        }
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    uint64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }
    size_t sizeApprox() const {
        return tail_.load(std::memory_order_acquire) -
               head_.load(std::memory_order_acquire);
    }
    void clear() {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }

private:
    static size_t roundUpPow2(size_t v) {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }
    const size_t cap_;
    const size_t mask_;
    std::unique_ptr<T[]> buf_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::atomic<uint64_t> dropped_{0};
};

enum class EbpfLoaderType {
    NONE = 0,
    SYSCALL_PRECOMPILED = 1,
    BPFTOOL_PRECOMPILED = 2,
    BPFTOOL_RUNTIME = 3,
    FALLBACK = 4,
    EMBEDDED_SYSCALL = 5,
};

enum class EbpfState {
    UNINITIALIZED = 0,
    LOADING = 1,
    LOAD_TIMEOUT = 2,
    LOAD_FAILED = 3,
    ATTACHING = 4,
    ACTIVE = 5,
    DEGRADED = 6,
    UNLOADED = 7,
    EVENT_DRIVEN = 8,
    DEGRADED_RING = 9,
    POLLING = 10,
    DENIED_SELINUX = 11,
    NO_CAP = 12,
    KERNEL_UNSUPPORTED = 13,
    UNVERIFIED = 14,
};

enum class EbpfMode {
    POLLING = 0,
    EVENT_DRIVEN = 1,
    HYBRID = 2,
};

enum class TpOffsetStatus : uint8_t {
    UNKNOWN  = 0,
    VERIFIED = 1,
    DEGRADED = 2,
    NO_CTX   = 3,
};

class IEventConsumer {
public:
    virtual ~IEventConsumer() = default;
    virtual void onFork(const ForkEvent& ev) = 0;
    virtual void onExit(const ExitEvent& ev) = 0;
    virtual void onSwitch(const SwitchEvent& ev) = 0;
    virtual void onExec(const ExecEvent& ev) = 0;
    virtual void onRunqLatency(const RunqEvent& ev) = 0;
};

struct CpuSchedStats {
    uint64_t totalRuntimeNs = 0;
    uint64_t totalWaitTimeNs = 0;
    uint32_t switchCount = 0;
};

struct ThreadCpuRuntime {
    int tid = 0;
    int cpu = -1;
    uint64_t runTimeNs = 0;
    uint64_t timestamp = 0;
};

class ThreadCpuRuntimeAggregator {
public:
    ThreadCpuRuntimeAggregator();
    ~ThreadCpuRuntimeAggregator();
    uint64_t update(int tid, int cpu, uint64_t runTimeNs, uint64_t timestampNs);
    void getAllSnapshots(std::vector<ThreadCpuRuntime>& out) const;
    void clear();
private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, ThreadCpuRuntime> snapshots_;
};

class EbpfScheduler {
public:
    EbpfScheduler();
    ~EbpfScheduler();

    bool load(void);
    bool isActive(void) const { return active_; }
    EbpfLoaderType getLoaderType(void) const { return loaderType_; }
    EbpfState getState(void) const { return state_; }
    EbpfMode getMode(void) const { return mode_; }
    bool isEventDrivenLive(void) const {
        return active_.load(std::memory_order_acquire) &&
               !unloaded_.load(std::memory_order_acquire) &&
               pollThreadRunning_.load(std::memory_order_acquire) &&
               mode_.load(std::memory_order_acquire) == EbpfMode::EVENT_DRIVEN;
    }
    const char* getStateString(void) const;
    const char* getEbpfStateSevenString(void) const;
    const char* getLoaderString(void) const;
    const char* getModeString(void) const;

    std::string getOffsetVerifyResult(void) const {
        std::lock_guard<std::mutex> lk(offsetVerifyMutex_);
        return offsetVerifyResult_;
    }

    const char* getOffsetStatusSwitch(void) const { return tpOffsetStatusToString(offsetStatus_switch_.load(std::memory_order_relaxed)); }
    const char* getOffsetStatusFork(void)   const { return tpOffsetStatusToString(offsetStatus_fork_.load(std::memory_order_relaxed)); }
    const char* getOffsetStatusExec(void)   const { return tpOffsetStatusToString(offsetStatus_exec_.load(std::memory_order_relaxed)); }
    const char* getOffsetStatusExit(void)   const { return tpOffsetStatusToString(offsetStatus_exit_.load(std::memory_order_relaxed)); }
    const char* getOffsetStatusWakeup(void) const { return tpOffsetStatusToString(offsetStatus_wakeup_.load(std::memory_order_relaxed)); }
    bool isOffsetDegraded(void) const { return offsetDegraded_.load(std::memory_order_relaxed); }
    bool isOffsetUnverifiedButTrusted(void) const { return offsetUnverifiedButTrusted_.load(std::memory_order_relaxed); }
    bool isKnownKernelFamily(void) const { return knownKernelFamily_.load(std::memory_order_relaxed); }

    void g4TestEnterOffsetDegraded(void) {
        stopPollThread();
        offsetDegraded_.store(true, std::memory_order_relaxed);
        state_ = EbpfState::DEGRADED;
        mode_  = EbpfMode::POLLING;
    }
    void g4TestEnterRingLossHybrid(void) {
        offsetDegraded_.store(false, std::memory_order_relaxed);
        state_ = EbpfState::DEGRADED_RING;
        mode_  = EbpfMode::HYBRID;
        lastRingLossMs_.store(0, std::memory_order_relaxed);
    }
    void g4TestStampOldRingLoss(void) {
        uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        lastRingLossMs_.store(nowMs - 60000, std::memory_order_relaxed);
    }
    void g4TickRecovery(void) { recoverIfHealthy(); }
    void g4TestEnterAttached(void) {
        offsetDegraded_.store(false, std::memory_order_relaxed);
        offsetUnverifiedButTrusted_.store(false, std::memory_order_relaxed);
        state_ = EbpfState::ACTIVE;
        mode_  = EbpfMode::EVENT_DRIVEN;
        active_ = true;
        unloaded_.store(false, std::memory_order_relaxed);
    }
    void g4TestApplyOffsetStatus(TpOffsetStatus sw, bool knownFamily) {
        offsetStatus_switch_.store(sw, std::memory_order_relaxed);
        knownKernelFamily_.store(knownFamily, std::memory_order_relaxed);
        postLoadOffsetDegradeDecision();
    }
    void g4TestStopPollThread(void) { stopPollThread(); }
#ifdef AATPE_HOST_TEST
    void g4TestCallDegrade(const char* reason) { degrade(reason); }
    uint32_t g4TestDegradeWarnCount(void) const {
        return degradeWarnCount_.load(std::memory_order_relaxed);
    }
#endif
    bool g4ModeIsEventDriven(void) const { return mode_ == EbpfMode::EVENT_DRIVEN; }
    bool g4StateIsActive(void) const { return state_ == EbpfState::ACTIVE; }
    bool g4StateIsEventDriven(void) const { return state_ == EbpfState::EVENT_DRIVEN; }
    bool g4IsUnloaded(void) const { return unloaded_.load(std::memory_order_relaxed); }
    bool g4IsActive(void) const { return active_.load(); }

    bool g4StateIsDegradedRing(void) const { return state_ == EbpfState::DEGRADED_RING; }
    bool g4ModeIsPolling(void) const { return mode_ == EbpfMode::POLLING; }
    bool g4ProgsAttached(void) const { return !progFds_.empty(); }
    int  g4LoadFailureCount(void) const { return loadFailureCount_.load(std::memory_order_relaxed); }
    uint64_t g4NextRetryDelayMs(void) const { return nextRetryDelayMs_.load(std::memory_order_relaxed); }
    void g4TestFastForwardRetryClock(uint64_t ms) {
        uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        lastLoadAttemptMs_.store(nowMs - ms, std::memory_order_relaxed);
    }
    void g4TickRetryLoad(void) { retryLoadIfDue(); }
    void g4TestAttachSentinelProg(void) { progFds_.push_back(-1); }

    size_t g4TestCriticalQueueDepth(void) const { return criticalQueue_.sizeApprox(); }
    size_t g4TestNormalQueueDepth(void) const  { return normalQueue_.sizeApprox(); }
    size_t g4TestSwitchAggSize(void) const {
        std::lock_guard<std::mutex> lk(switchAggMutex_);
        return switchAgg_.size();
    }
    void g4TestPushFakeCriticalEvent(void) {
        AnyEvent ev;
        ev.hdr.type = (uint32_t)EventType::EXIT;
        ev.hdr.cpu = 0;
        ev.hdr.timestampNs = 0;
        ev.data.exit.hdr.type = ev.hdr.type;
        ev.data.exit.hdr.cpu = 0;
        ev.data.exit.hdr.timestampNs = 0;
        ev.data.exit.pid = 9999;
        ev.data.exit.tid = 9999;
        criticalQueue_.push(ev);
    }
    bool g4TestPollThreadRunning(void) const { return pollThreadRunning_.load(); }

#ifdef AATPE_HOST_TEST
    int hostTestParseRingBlob(const void* data, size_t dataBytes);
#endif
#ifdef AATPE_HOST_TEST
    bool hostTestParseElfFile(const std::string& path) {
        std::vector<std::vector<uint8_t>> progs;
        std::vector<std::string> tps;
        std::vector<std::vector<int>> mi;
        std::string lic;
        return parseElfPrograms(path, progs, tps, mi, lic);
    }
#endif
#ifdef AATPE_HOST_TEST
    bool hostTestOffsetWarnFired(char kind) const;
    void hostTestSetOffsetDegraded(char kind);
    void hostTestDeliverFork(const ForkEvent& ev) { deliverForkEvent(ev); }
    void hostTestDeliverSwitch(const SwitchEvent& ev) { deliverSwitchEvent(ev); }
    void hostTestDeliverRunq(const RunqEvent& ev) { deliverRunqEvent(ev); }
    void hostTestSetOffsetStatus(char kind, TpOffsetStatus s);
    static const std::vector<std::pair<std::string,std::string>>&
        hostTestBpftoolAttachTable();
    bool hostTestParseElfMapIndices(const std::string& path,
                                     std::vector<std::vector<int>>& outMapIndices);
    static int hostTestEmbeddedCpuBufferSlots(long possibleCpus);
#endif
    bool offsetDeliveryAllowed(TpOffsetStatus s) const {
        if (s == TpOffsetStatus::VERIFIED || s == TpOffsetStatus::NO_CTX) return true;
        if (s == TpOffsetStatus::UNKNOWN &&
            offsetUnverifiedButTrusted_.load(std::memory_order_relaxed)) return true;
        return false;
    }
    bool isSwitchOffsetTrusted(void) const {
        return offsetDeliveryAllowed(offsetStatus_switch_.load(std::memory_order_relaxed));
    }
    bool isForkOffsetTrusted(void) const {
        return offsetDeliveryAllowed(offsetStatus_fork_.load(std::memory_order_relaxed));
    }
    bool isExecOffsetTrusted(void) const {
        return offsetDeliveryAllowed(offsetStatus_exec_.load(std::memory_order_relaxed));
    }
    bool isExitOffsetTrusted(void) const {
        return offsetDeliveryAllowed(offsetStatus_exit_.load(std::memory_order_relaxed));
    }
    bool isWakeupOffsetTrusted(void) const {
        return offsetDeliveryAllowed(offsetStatus_wakeup_.load(std::memory_order_relaxed));
    }
    uint64_t getOffsetFiltered(void) const {
        return offsetFilteredCount_.load(std::memory_order_relaxed);
    }

    void setRingPages(int pages) { ringPages_ = pages; }
    void setPollFallbackEnabled(bool enabled) { pollFallbackEnabled_.store(enabled, std::memory_order_relaxed); }
    void setEventConsumer(IEventConsumer* consumer) { eventConsumer_ = consumer; }

    struct EbpfRuntimeConfig {
        bool enabled = true;
        int  ringPages = 64;
        bool pollFallback = true;
    };
    bool onConfigReload(const EbpfRuntimeConfig& cfg);
    void registerConfigReloadHook();

    int pollEvents(int maxEvents = 256);

    size_t dispatchQueuedEvents(void);
    uint64_t getQueueDropped(void) const;
    size_t getQueueSize(void) const;
    void setCriticalEventWakeHook(std::function<void()> fn) {
        criticalEventWakeHook_ = std::move(fn);
    }
    uint64_t getLastCycleEventsProcessed(void) const {
        return lastCycleEventsProcessed_.load(std::memory_order_relaxed);
    }
    static constexpr size_t kDispatchBudgetPerCycle = 256;
    static constexpr size_t kCriticalDispatchBudgetPerCycle = 1024;

    struct EventStats {
        uint64_t total = 0;
        uint64_t processed = 0;
        uint64_t dropped = 0;
        uint64_t ringOverflow = 0;
        uint64_t fallbackCount = 0;
        uint64_t consumerLag = 0;
        uint64_t windowLost = 0;
        double   eventRatePerSec = 0.0;
        uint64_t criticalDropped = 0;
        uint64_t normalDropped = 0;
        uint64_t criticalBacklog = 0;
        uint64_t eventsProcessedLastCycle = 0;
    };
    EventStats getEventStats(void) const;

    bool startPollThread(void);
    void stopPollThread(void);
    bool isPollThreadRunning(void) const { return pollThreadRunning_.load(); }

    uint64_t getTotalEvents(void) const { return eventCountTotal_.load(); }
    uint64_t getForkEvents(void) const { return eventCountFork_.load(); }
    uint64_t getExitEvents(void) const { return eventCountExit_.load(); }
    uint64_t getSwitchEvents(void) const { return eventCountSwitch_.load(); }
    uint64_t getExecEvents(void) const { return eventCountExec_.load(); }
    uint64_t getRingLost(void) const { return ringLost_.load(); }
    uint64_t getWindowLost(void) const { return windowLost_.load(); }
    uint64_t getWindowEvents(void) const { return windowEvents_.load(); }
    uint64_t getWindowStartMs(void) const { return windowStartMs_.load(); }
    void rotateWindowIfNeeded(void);
    double getEventRatePerSec(void) const;

    bool readCpuStats(std::vector<CpuSchedStats>& outStats);
    bool readEmbeddedCpuCounts(std::vector<uint64_t>& outCounts);
    static bool readFallbackSchedstat(std::vector<CpuSchedStats>& outStats);

    void degrade(const char* reason);
    bool shouldDegrade(void) const;
    void logDegradeSummaryIfDue(void);
    void recoverIfHealthy(void);
    void postLoadOffsetDegradeDecision(void);

    void dynamicRingAdjustIfDue(void);
    int getCurrentRingPages(void) const { return ringPages_; }

    void retryLoadIfDue(void);
    bool isLoadPermanentlyFailed(void) const { return loadPermanentFailed_; }

    static bool onlineMaskSetChanged(const std::vector<int>& oldIds, const std::string& newText);

    bool isPartiallyDegraded(void) const;
    const char* getDetailedStateString(void) const;

    void unload(void);

    static const uint8_t* getEmbeddedBytecode(size_t* outSize);
    static size_t getEmbeddedBytecodeSize(void);

private:
    std::atomic<bool> active_{false};
    std::atomic<bool> unloaded_{false};
    EbpfLoaderType loaderType_ = EbpfLoaderType::NONE;
    std::atomic<EbpfState> state_{EbpfState::UNVERIFIED};
    std::atomic<EbpfMode> mode_{EbpfMode::POLLING};
    std::string offsetVerifyResult_ = "unknown";
    mutable std::mutex offsetVerifyMutex_;
    std::mutex lifecycleMutex_;
    std::atomic<TpOffsetStatus> offsetStatus_switch_{TpOffsetStatus::UNKNOWN};
    std::atomic<TpOffsetStatus> offsetStatus_fork_{TpOffsetStatus::UNKNOWN};
    std::atomic<TpOffsetStatus> offsetStatus_exec_{TpOffsetStatus::NO_CTX};
    std::atomic<TpOffsetStatus> offsetStatus_exit_{TpOffsetStatus::NO_CTX};
    std::atomic<TpOffsetStatus> offsetStatus_wakeup_{TpOffsetStatus::UNKNOWN};
    std::atomic<bool> offsetDegraded_{false};
    std::atomic<bool> offsetUnverifiedButTrusted_{false};
    std::atomic<bool> knownKernelFamily_{false};
    std::atomic<uint64_t> offsetFilteredCount_{0};
    static const char* tpOffsetStatusToString(TpOffsetStatus s);
    IEventConsumer* eventConsumer_ = nullptr;
    int ringPages_ = 64;
    static constexpr int kRingPagesNormal = 64;
    static constexpr int kRingPagesHigh   = 128;
    static constexpr int kRingPagesLow    = 32;
    std::atomic<uint64_t> lastRingAdjustMs_{0};
    std::atomic<int> lowLossStreak_{0};
    static constexpr uint64_t kRingAdjustIntervalMs = 30000;
    static constexpr double kRingHighLossThreshold = 0.05;
    static constexpr double kRingLowLossThreshold  = 0.001;
    static constexpr uint64_t kRingHighLostBurst = 200;
    std::atomic<uint64_t> lostAtLastAdjust_{0};
    std::atomic<uint64_t> eventsAtLastAdjust_{0};

    std::atomic<uint64_t> lastHotplugRebuildMs_{0};
    static constexpr uint64_t kHotplugRebuildMinIntervalMs = 5000;
    std::atomic<uint64_t> hotplugRebuildAttempts_{0};
    std::atomic<uint64_t> hotplugRebuildFailures_{0};

    std::atomic<int> loadFailureCount_{0};
    std::atomic<uint64_t> lastLoadAttemptMs_{0};
    std::atomic<uint64_t> nextRetryDelayMs_{0};
    bool loadPermanentFailed_ = false;
    static constexpr uint64_t kBackoffBaseMs = 1000;
    static constexpr uint64_t kBackoffMaxMs = 300000;
    static constexpr int kBackoffMaxRetries = 10;

    std::atomic<uint64_t> criticalEventsInWindow_{0};
    std::atomic<uint64_t> criticalWindowStartMs_{0};
    std::atomic<uint64_t> criticalDroppedByRate_{0};
    static constexpr int kMaxCriticalPerSec = 200;
    bool rateLimitCriticalEvent(void);
    std::atomic<bool> pollFallbackEnabled_{true};

    std::atomic<bool> pollThreadRunning_{false};
    std::atomic<bool> pollThreadStop_{false};
    std::thread pollThread_;
    void pollThreadFunc(void);
    int pollWakeupReadFd_ = -1;
    int pollWakeupWriteFd_ = -1;

    SpscBoundedQueue<AnyEvent> criticalQueue_{256};
    SpscBoundedQueue<AnyEvent> normalQueue_{16384};
    bool queueEnabled_ = false;
    void setEventQueueEnabled(bool en) { queueEnabled_ = en; }
    std::function<void()> criticalEventWakeHook_;
    std::atomic<uint64_t> lastCycleEventsProcessed_{0};
    void deliverForkEvent(const ForkEvent& ev);
    void deliverExitEvent(const ExitEvent& ev);
    void deliverSwitchEvent(const SwitchEvent& ev);
    void deliverExecEvent(const ExecEvent& ev);
    void deliverRunqEvent(const RunqEvent& ev);

    int eventsMapFd_ = -1;
    int countersMapFd_ = -1;
    int threadCacheMapFd_ = -1;
    std::vector<int> progFds_;
    std::vector<int> mapFds_;

    struct PerfRing {
        int perfFd = -1;
        void* base = nullptr;
        size_t mmapSize = 0;
        int cpu = -1;
    };
    std::vector<PerfRing> rings_;
    std::vector<int> tracepointFds_;
    std::vector<std::string> kprobeEventNames_;
    std::vector<int> rawTracepointLinks_;
    std::vector<int> onlineCpuIds_;
    std::unordered_map<std::string, std::pair<std::string,int>> kprobeIdCache_;
    int activeProgType_ = -1;
    std::string activeAttachMethod_;

    int perfFd_ = -1;
    int perfMmapSize_ = 0;
    void* perfRingBase_ = nullptr;

    std::atomic<uint64_t> eventCountTotal_{0};
    std::atomic<uint64_t> eventCountFork_{0};
    std::atomic<uint64_t> eventCountExit_{0};
    std::atomic<uint64_t> eventCountSwitch_{0};
    std::atomic<uint64_t> switchSampleCounter_{0};
    std::atomic<uint64_t> switchSampledOut_{0};
    std::atomic<uint64_t> eventCountExec_{0};
    std::atomic<uint64_t> eventCountRunq_{0};
    std::atomic<uint64_t> runqMaxDelayNs_{0};
    std::atomic<uint64_t> ringLost_{0};
    std::atomic<uint64_t> fallbackCount_{0};
    std::atomic<uint64_t> degradeEventCount_{0};
    std::atomic<uint64_t> degradeSummaryLastMs_{0};
#ifdef AATPE_HOST_TEST
    std::atomic<uint32_t> degradeWarnCount_{0};
#endif
    std::atomic<uint64_t> lastRingLossMs_{0};
    std::atomic<uint64_t> lastRecoverMs_{0};
    std::atomic<uint64_t> windowLost_{0};
    std::atomic<uint64_t> windowEvents_{0};
    std::atomic<uint64_t> windowStartMs_{0};
    static constexpr uint64_t kWindowRotateMs = 60000;
    std::atomic<uint64_t> ringConsumedBytes_{0};
    std::atomic<uint64_t> lastRateCheckNs_{0};
    std::atomic<uint64_t> eventsAtLastCheck_{0};
    std::atomic<double> eventRate_{0.0};
    std::atomic<int> ringsMappedCount_{0};
    std::atomic<int> ringsTotalCount_{0};
    int getRingsMapped(void) const { return ringsMappedCount_.load(std::memory_order_relaxed); }
    int getRingsTotal(void) const { return ringsTotalCount_.load(std::memory_order_relaxed); }

    struct SwitchAggregate {
        uint32_t nextPid = 0;
        uint64_t count = 0;
        uint64_t lastTs = 0;
        char nextComm[16] = {0};
    };
    std::unordered_map<uint32_t, SwitchAggregate> switchAgg_;
    mutable std::mutex switchAggMutex_;
    static constexpr int kSwitchAggregateThreshold = 10;
    std::atomic<uint64_t> lastSwitchAggCleanupMs_{0};
    static constexpr uint64_t kSwitchAggCleanupIntervalMs = 300000;
    static constexpr uint64_t kSwitchAggEntryTtlMs = 300000;
    static constexpr size_t kMaxSwitchAggEntries = 10000;
    static constexpr uint64_t kSwitchAggLazyEvictInserts = 512;
    uint64_t switchAggInsertCount_ = 0;
    void cleanupSwitchAgg(uint64_t nowMs);
    void evictStaleSwitchAggLocked(uint64_t nowMs);

    bool tryLoadEmbeddedSyscall(void);
    bool tryLoadSyscallPrecompiled(void);
    bool tryLoadBpftoolPrecompiled(void);

    int bpfSyscall(int cmd, void* attr, unsigned int size);
    int createMap(uint32_t type, uint32_t keySize, uint32_t valueSize, uint32_t maxEntries);
    int loadBpfProgFromBytes(const uint8_t* bytes, size_t len,
                              const char* license, const std::vector<int>& mapFds,
                              int progType);
    int loadBpfProgFromBytesWithRefs(const uint8_t* bytes, size_t len,
                                       const char* license,
                                       const std::vector<int>& mapFds,
                                       const std::vector<int>& mapIndices,
                                       int progType);
    int loadBpfProgFromBytesWithExplicitRefs(const uint8_t* bytes, size_t len,
                                               const char* license,
                                               const int* refOffsets,
                                               const int* refMapIndices,
                                               int refCount,
                                               const std::vector<int>& mapFds,
                                               int progType);
    int loadBpfProgFromBytes(const uint8_t* bytes, size_t len,
                              const char* license, int mapFd, int progType);
    int loadBpfProgFromBytes(const uint8_t* bytes, size_t len,
                              const char* license, int mapFd);
    bool attachTracepoint(int progFd, int tpId, int cpu, int& outPerfFd);
    int readTracepointId(const char* tpPath);

    bool openRingBufferOnlyEvent(int tpId, int cpu, int& outPerfFd);
    int setupPerCpuRingBuffers(int tpId, int attachFd, int attachCpu, int ncpus, int progIndex);

    std::vector<int> hotplugRebuildIfDue(void);

    enum class KprobeAttachResult {
        OK = 0,
        CREATE_FAILED = 1,
        ID_NOT_FOUND = 2,
        PERF_OPEN_FAILED = 3,
        SET_BPF_FAILED = 4,
        ENABLE_FAILED = 5,
    };
    bool attachKprobe(int progFd, const char* funcName, bool isKretprobe,
                      int cpu, int& outPerfFd, std::string& outEventName);
    int ensureKprobeEvent(const char* funcName, bool isKretprobe,
                          std::string& outEventName);
    int createKprobeEvent(const char* funcName, bool isKretprobe,
                           std::string& outEventName);
    int readKprobeEventId(const std::string& eventName);
    void cleanupKprobeEvents(void);

    bool extractBpfTextFromElf(const std::string& path,
                                 std::vector<uint8_t>& outText,
                                 std::string& outLicense);
    bool parseElfPrograms(const std::string& path,
                           std::vector<std::vector<uint8_t>>& outPrograms,
                           std::vector<std::string>& outTpPaths,
                           std::vector<std::vector<int>>& outMapIndices,
                           std::string& outLicense);

    bool mmapPerfRing(int perfFd, size_t pageCount);
    bool mmapPerfRingForCpu(int cpu, int perfFd, size_t pageCount);
    bool updateEventsMapWithVerify(int cpu, int perfFd);
    void unmapPerfRing(void);
    void unmapAllRings(void);
    int processRingData(PerfRing& ring, int maxEvents);

    void detectEnvironment(void);
    void runCapabilityDetection(void);
    bool checkDebugfsTracefs(void);
    int readKernelVersion(void);
    void verifyTracepointOffsets(void);
};

}

#endif
