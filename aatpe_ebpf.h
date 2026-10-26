// AATPE eBPF Event-Driven Architecture
// Comprehensive eBPF loader + perf ring buffer event monitor.
// Replaces polling-based thread/app monitoring with real-time events.
// Supports: embedded_syscall, syscall_precompiled, bpftool_precompiled, fallback.
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
#include <new>   // v94 fix / 问题46: placement-new for AnyEvent default ctor
#include <memory>  // v95 issues 6/12: std::unique_ptr for SPSC ring storage

namespace aatpe_ebpf {

// #24: pure parser for /sys/devices/system/cpu/online mask text
// ("0-1,4-5" -> [0,1,4,5]). Exposed for host unit tests.
std::vector<int> parseCpuOnlineMask(const std::string& text);
// Read sysfs online mask; falls back to 0..ncpus-1 on read failure.
std::vector<int> readOnlineCpuIds();

// ============================================================================
// Event types (must match eBPF program)
// ============================================================================
enum class EventType : uint32_t {
    FORK   = 1,
    EXIT   = 2,
    SWITCH = 3,
    EXEC   = 4,
    RUNQ   = 5,   // P1-1: runqueue (wakeup->run) latency sample
};

// ============================================================================
// Event structures (must match eBPF program layout)
// ============================================================================
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

// P1-1: runqueue latency (wakeup -> scheduled-in). Only long samples and a
// 1/8 sample of short samples are shipped; emitted from the sched_switch BPF
// program after matching a pending sched_wakeup.
// P2-4: wakerTid = the thread that issued the wakeup (1-hop dependency edge).
// P2-2: targetCpu = the CPU the waker intended (from sched_wakeup ctx+36),
//        NOT the CPU that actually picked the task (hdr.cpu).
struct RunqEvent {
    EventHeader hdr;
    uint32_t pid = 0;
    uint32_t targetCpu = 0;
    uint32_t wakerTid = 0;
    uint32_t pad = 0;
    uint64_t delayNs = 0;
};

// Union of all events for queueing
struct AnyEvent {
    EventHeader hdr;
    // the union members carry in-class default member
    // initializers (= 0), which makes their default constructors non-trivial
    // and therefore DELETES the implicit default constructor of an anonymous
    // union. `AnyEvent a;` in deliver*Event() then failed to compile on g++.
    // Wrap in a NAMED union with a user-provided default ctor that explicitly
    // activates the first member via placement-new; the call sites immediately
    // overwrite the active member via assignment. Members are all trivially
    // destructible, so the implicit union dtor is fine.
    //
    // L-fix (type-punning contract): every union member is a standard-layout
    // trivially-copyable POD whose FIRST member is `EventHeader hdr`. The
    // producer (deliver*Event) activates exactly one member with `a.data.X = ev;`
    // and stamps `a.hdr.type` to the matching EventType. The consumer
    // (dispatchQueuedEvents) reads the object representation back with memcpy
    // (NOT by naming a.data.X) so it does not rely on which union member the
    // compiler considers "active" — reading a trivially-copyable object's bytes
    // out of union storage via memcpy is well-defined regardless.
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

// ============================================================================
// lock-free bounded SPSC ring buffer (replaces mutex+deque)
//
// The poll->queue->dispatch path is a TRUE single-producer / single-consumer
// scenario:
//   producer = dedicated eBPF poll thread (deliver*Event -> push)
//   consumer = daemon main loop        (dispatchQueuedEvents -> popBatch)
// No other thread ever touches a given queue, so a mutex + std::deque was
// pure overhead: it serialized two threads that never actually contended,
// forced a heap allocation on every push (deque block growth), and took a
// userspace lock on every SWITCH sample.
//
// Concurrency discipline (classic DPDK-style SPSC):
//   - only the producer writes tail_; only the consumer writes head_.
//   - producer: read head_ (acquire), write slot, store tail_ (release).
//   - consumer: read tail_ (acquire), read slots, store head_ (release).
//   - head_/tail_ sit on separate cache lines (alignas(64)) to avoid false
//     sharing between the poll thread and the daemon main loop.
//
// Overflow policy: DROP-NEWEST.
//   The producer detects a full ring (next tail == head) and increments
//   dropped_ WITHOUT touching head_. Drop-oldest would require the producer
//   to advance the consumer-owned head_, which races with the consumer's
//   in-flight reads and is not legal in a lock-free SPSC ring.
//
// This is semantically correct for our two queues:
//   - normalQueue (SWITCH): events are already threshold-aggregated on the
//     delivery path (kSwitchAggregateThreshold=10); losing a marginal sample
//     is harmless.
//   - criticalQueue (FORK/EXEC/EXIT): 256 slots; lifecycle events are rare
//     (tens/sec at most). Overflow only happens if the main loop stalls
//     catastrophically, in which case dropping one newest critical event
//     vs. one oldest is immaterial — the periodic /proc calibration scan is
//     the backstop.
//
// Dual-queue priority (issues 6/7): FORK/EXEC/EXIT go to criticalQueue_,
// SWITCH goes to normalQueue_. SWITCH events therefore can NEVER evict a
// lifecycle event (they live in separate rings), and dispatchQueuedEvents()
// drains criticalQueue_ completely before touching normalQueue_.
// ============================================================================
template <typename T>
class SpscBoundedQueue {
public:
    explicit SpscBoundedQueue(size_t capacity)
        : cap_(roundUpPow2(capacity)),
          mask_(cap_ - 1),
          buf_(new T[cap_]) {}

    // Producer-only (poll thread). Copies `ev` into the ring. Returns true if
    // enqueued; false if the ring was full and the event was dropped-newest.
    // CRITICAL: head_/tail_ are MONOTONIC (never wrap at cap_). The ring index
    // is computed via `pos & mask_`. Wrap-around of the position counters only
    // happens at 2^64 (impossible in practice). Storing a wrapped tail (e.g.
    // `tail = (tail+1) & mask`) breaks `avail = tail - head` in popBatch:
    // after wrap, tail(0) - head(>0) underflows to ~2^64, causing the consumer
    // to spin billions of times → daemon hang → watchdog SIGKILL.
    bool push(const T& ev) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= cap_) {  // full (cap_ slots, one reserved for empty/full distinction)
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buf_[tail & mask_] = ev;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer-only (daemon main loop). Pops up to `budget` events into `out`
    // (budget == 0 means unbounded). Returns the number actually popped.
    size_t popBatch(std::vector<T>& out, size_t budget) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t avail = tail - head;  // unsigned wrap is correct modulo 2^64
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

    // Cumulative events dropped because the ring was full.
    uint64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }
    // Approximate current depth (racy by design; diagnostics only).
    size_t sizeApprox() const {
        return tail_.load(std::memory_order_acquire) -
               head_.load(std::memory_order_acquire);
    }
    // EBPF-03 fix: reset head_/tail_ so the ring is empty after unload().
    // Caller must guarantee no concurrent producer/consumer (in unload() the
    // poll thread is already stopped and the main loop has drained).
    void clear() {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }

private:
    static size_t roundUpPow2(size_t v) {
        size_t p = 1;
        while (p < v) p <<= 1;
        // P3-10: p starts at 1 and only left-shifts, so p is always >= 1 here;
        // the old `p ? p : 16` fallback was dead code.
        return p;
    }
    const size_t cap_;
    const size_t mask_;
    std::unique_ptr<T[]> buf_;
    alignas(64) std::atomic<size_t> head_{0};  // consumer-owned (daemon main)
    alignas(64) std::atomic<size_t> tail_{0};  // producer-owned (poll thread)
    alignas(64) std::atomic<uint64_t> dropped_{0};
};

// ============================================================================
// Loader types and states
// ============================================================================
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
    // ROUND-014 P3 C36: seven-state diagnostic ladder (written to ebpf_state key).
    EVENT_DRIVEN = 8,       // active + ringbuf readable
    DEGRADED_RING = 9,      // prog loaded but ringbuf read fails
    POLLING = 10,           // fallback /proc sweep
    DENIED_SELINUX = 11,    // map attach EACCES/EPERM
    NO_CAP = 12,            // missing CAP_BPF/perf
    KERNEL_UNSUPPORTED = 13,// no BPF/tracepoint support
    UNVERIFIED = 14,        // not yet probed
};

enum class EbpfMode {
    POLLING = 0,       // pure polling (eBPF unavailable)
    EVENT_DRIVEN = 1,  // eBPF events primary, polling as 30s validation
    HYBRID = 2,        // eBPF degraded, polling primary with event best-effort
};

// ============================================================================
// per-tracepoint tracepoint-ctx byte-offset verification status.
//
// The BPF programs in sched_events.bpf.c read sched_switch / sched_process_fork
// ctx at HARDCODED byte offsets derived from the Linux 4.14 generic layout.
// verifyTracepointOffsets() reads the live
//   /sys/kernel/[debug/]tracing/events/sched/<event>/format
// at load() and compares. Previously a mismatch only produced a
// "continuing anyway" WARN and the daemon kept reading ctx at the wrong
// offsets, silently corrupting next_pid / prev_comm / child_pid etc. This enum
// gives us a per-tracepoint verdict so the delivery path can FILTER the
// affected tracepoint's events instead of feeding garbage downstream.
//
//   UNKNOWN  : the format file was unreadable (debugfs/tracefs not mounted on
//              the locked production ROM). We have no evidence of a mismatch,
//              so we DO NOT filter (preserves the pre-v94-r3 "continuing
//              anyway" behavior).
//   VERIFIED : the live format matches the hardcoded offsets. Safe to consume.
//   DEGRADED : the live format DOES NOT match (field missing or at a different
//              offset). deliver*Event() MUST drop events from this tracepoint.
//              For sched_switch this is load-bearing and triggers a full
//              eBPF->polling degrade().
//   NO_CTX   : this tracepoint does not read tracepoint ctx at all (exec /
//              exit use bpf_get_current_comm / bpf_get_current_pid_tgid).
//              Always safe; no filtering.
// ============================================================================
enum class TpOffsetStatus : uint8_t {
    UNKNOWN  = 0,
    VERIFIED = 1,
    DEGRADED = 2,
    NO_CTX   = 3,
};

// ============================================================================
// Event callback interface
// ============================================================================
class IEventConsumer {
public:
    virtual ~IEventConsumer() = default;
    virtual void onFork(const ForkEvent& ev) = 0;
    virtual void onExit(const ExitEvent& ev) = 0;
    virtual void onSwitch(const SwitchEvent& ev) = 0;
    virtual void onExec(const ExecEvent& ev) = 0;
    // P1-1: runqueue latency sample (cheap; may be called on poll thread).
    virtual void onRunqLatency(const RunqEvent& ev) = 0;
};

// ============================================================================
// CPU scheduling stats (legacy support)
// ============================================================================
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

// ============================================================================
// EbpfScheduler — comprehensive eBPF event-driven monitor
// ============================================================================
class EbpfScheduler {
public:
    EbpfScheduler();
    ~EbpfScheduler();

    // --- Loading ---
    bool load(void);
    bool isActive(void) const { return active_; }
    EbpfLoaderType getLoaderType(void) const { return loaderType_; }
    EbpfState getState(void) const { return state_; }
    EbpfMode getMode(void) const { return mode_; }
    // G2 (v4.4.9): true iff eBPF event-driven monitoring is ACTUALLY live.
    //
    // load() deliberately returns `true` even when it cannot attach anything and
    // lands in the non-privileged POLLING fallback — that is a handled outcome,
    // not a crash, and the daemon main loop must keep running. `isActive()` (and
    // load()'s bool) therefore means "the eBPF subsystem is up, possibly in
    // polling-fallback sense". The daemon's g_ebpfActive must instead reflect
    // whether event-driven ring monitoring is live, so a device whose BPF is
    // unavailable does not report eBPF as ACTIVE in the state file / WebUI.
    //
    // Healthy devices (EVENT_DRIVEN, or HYBRID with rings still attached) report
    // live; the pure POLLING fallback (NO_CAP / ENOSYS / EPERM / no rings)
    // reports NOT live. This does not change healthy-device behavior: on a
    // device where load() attaches, mode_==EVENT_DRIVEN and this returns true.
    // EBPF-05 fix: HYBRID means ring-loss degraded and the poll thread has been
    // STOPPED (degrade() calls stopPollThread()). In that state no events are
    // being drained, so isEventDrivenLive() must NOT report true. We require
    // mode_==EVENT_DRIVEN AND pollThreadRunning_==true. POLLING fallback and
    // HYBRID both report NOT live, which is the truth.
    bool isEventDrivenLive(void) const {
        return active_.load(std::memory_order_acquire) &&
               !unloaded_.load(std::memory_order_acquire) &&
               pollThreadRunning_.load(std::memory_order_acquire) &&
               mode_.load(std::memory_order_acquire) == EbpfMode::EVENT_DRIVEN;
    }
    const char* getStateString(void) const;
    // ROUND-014 P3: seven-state string (event_driven/denied_selinux/...).
    const char* getEbpfStateSevenString(void) const;
    const char* getLoaderString(void) const;
    const char* getModeString(void) const;

    // result of the runtime tracepoint ctx-offset check.
    //   "true (...)"   : live /sys/.../events/sched/*/format offsets match the
    //                    hardcoded Linux-4.14 layout compiled into sched_events.bpf.c.
    //   "false (...)"  : a mismatch was detected (we keep running but WARN).
    //   "unknown (...)": debugfs/tracefs not mounted/readable on this build.
    // The daemon writes this verbatim to <state_dir>/ebpf_offset_verified.
    // P2-6: offsetVerifyResult_ is a std::string written by verifyTracepointOffsets()
    // during load() and read here from the WebUI / state-writer thread. Returning
    // a std::string by value without a lock is a data race on the string's internal
    // buffer. Guard with a small mutex; the writer holds it only for the three
    // assignment sites in verifyTracepointOffsets().
    std::string getOffsetVerifyResult(void) const {
        std::lock_guard<std::mutex> lk(offsetVerifyMutex_);
        return offsetVerifyResult_;
    }

    // per-tracepoint offset-verdict accessors. The daemon writes
    // these to <state_dir>/ebpf_offset_status_{switch,fork,exec,exit} and the
    // WebUI surfaces them so the user can see which lifecycle events are
    // degraded when the hardcoded offsets don't match the on-device kernel.
    // Strings are short stable tokens ("verified" / "degraded" / "unknown" /
    // "no_ctx"). See TpOffsetStatus for the semantics.
    const char* getOffsetStatusSwitch(void) const { return tpOffsetStatusToString(offsetStatus_switch_.load(std::memory_order_relaxed)); }
    const char* getOffsetStatusFork(void)   const { return tpOffsetStatusToString(offsetStatus_fork_.load(std::memory_order_relaxed)); }
    const char* getOffsetStatusExec(void)   const { return tpOffsetStatusToString(offsetStatus_exec_.load(std::memory_order_relaxed)); }
    const char* getOffsetStatusExit(void)   const { return tpOffsetStatusToString(offsetStatus_exit_.load(std::memory_order_relaxed)); }
    const char* getOffsetStatusWakeup(void) const { return tpOffsetStatusToString(offsetStatus_wakeup_.load(std::memory_order_relaxed)); }
    // G4-001: true while sched_switch offsets are unverified and the daemon must
    // NOT auto-recover to EVENT_DRIVEN. Surfaced to the state key / WebUI.
    bool isOffsetDegraded(void) const { return offsetDegraded_.load(std::memory_order_relaxed); }
    // ROUND-003: true when built-in offsets are trusted because tracefs format was
    // unreadable on a known-kernel family (no evidence against built-in layout).
    // This is NOT a degradation; recoverIfHealthy/retryLoadIfDue treat it as healthy.
    bool isOffsetUnverifiedButTrusted(void) const { return offsetUnverifiedButTrusted_.load(std::memory_order_relaxed); }
    bool isKnownKernelFamily(void) const { return knownKernelFamily_.load(std::memory_order_relaxed); }

    // ---- G4-001 host regression seams (deterministic state-machine tests) ----
    // These let tests/v94/test_ebpf.cpp exercise the offset-vs-ring-loss
    // degradation split without a real bpf/tracefs environment on the host.
    void g4TestEnterOffsetDegraded(void) {
        stopPollThread();
        offsetDegraded_.store(true, std::memory_order_relaxed);
        state_ = EbpfState::DEGRADED;
        mode_  = EbpfMode::POLLING;
    }
    void g4TestEnterRingLossHybrid(void) {
        offsetDegraded_.store(false, std::memory_order_relaxed);
        state_ = EbpfState::DEGRADED_RING;  // P1 round7: matches production degrade()
        mode_  = EbpfMode::HYBRID;
        lastRingLossMs_.store(0, std::memory_order_relaxed);  // seedable clock
    }
    void g4TestStampOldRingLoss(void) {
        uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        lastRingLossMs_.store(nowMs - 60000, std::memory_order_relaxed);  // >30s ago (60s avoids boundary flakiness)
    }
    void g4TickRecovery(void) { recoverIfHealthy(); }
    // ROUND-003: simulate the post-attach precondition (production load() sets
    // mode_=EVENT_DRIVEN/state_=ACTIVE on a successful attach BEFORE calling the
    // offset decision). The test matrix then drives the offset verdict on top.
    void g4TestEnterAttached(void) {
        offsetDegraded_.store(false, std::memory_order_relaxed);
        offsetUnverifiedButTrusted_.store(false, std::memory_order_relaxed);
        state_ = EbpfState::ACTIVE;
        mode_  = EbpfMode::EVENT_DRIVEN;
        active_ = true;          // ROUND-004: simulate a live attach
        unloaded_.store(false, std::memory_order_relaxed);
    }
    // ROUND-003: host seam to drive the post-load offset decision for the
    // UNKNOWN(trust vs conservative) / MISMATCH(always degrade) matrix. Sets the
    // sched_switch offset verdict and the known-family flag, then runs the same
    // decision the production load() lambda uses (postLoadOffsetDegradeDecision()).
    void g4TestApplyOffsetStatus(TpOffsetStatus sw, bool knownFamily) {
        offsetStatus_switch_.store(sw, std::memory_order_relaxed);
        knownKernelFamily_.store(knownFamily, std::memory_order_relaxed);
        postLoadOffsetDegradeDecision();
    }
    void g4TestStopPollThread(void) { stopPollThread(); }  // clean teardown in host test
#ifdef AATPE_HOST_TEST
    // v4.4.9 R3-F01: drive the private degrade() and read how many times its
    // one-time WARN branch fired, so the host test can assert repeated degrade()
    // calls dedup to a single WARN/stopPoll transition. Release compiles out.
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

    // ---- E-001 host regression seams (production load-failure retry) ----
    // Drive the SAME state the production load() failure paths (all-3-phases
    // fallback and the fail-closed integrity-gate refusal) land in:
    //   mode_=POLLING + state_=DEGRADED_RING + no programs attached. The old gate
    // checked for the never-written DEGRADED(6) state, so retries never fired.
    // These seams let the host test assert retryLoadIfDue() honours the backoff
    // clock and eventually re-invokes load() — without a real bpf/tracefs device.
    bool g4StateIsDegradedRing(void) const { return state_ == EbpfState::DEGRADED_RING; }
    bool g4ModeIsPolling(void) const { return mode_ == EbpfMode::POLLING; }
    bool g4ProgsAttached(void) const { return !progFds_.empty(); }
    int  g4LoadFailureCount(void) const { return loadFailureCount_.load(std::memory_order_relaxed); }
    uint64_t g4NextRetryDelayMs(void) const { return nextRetryDelayMs_.load(std::memory_order_relaxed); }
    // Fast-forward the backoff timestamp so the next retryLoadIfDue() sees the
    // current nextRetryDelayMs_ as already elapsed.
    void g4TestFastForwardRetryClock(uint64_t ms) {
        uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        lastLoadAttemptMs_.store(nowMs - ms, std::memory_order_relaxed);
    }
    void g4TickRetryLoad(void) { retryLoadIfDue(); }
    // Host seam: record that a program is attached (pushes a sentinel fd that
    // unload() safely skips because it guards on fd >= 0). Used to prove the
    // retry gate does NOT reload a load that already succeeded (ring-loss /
    // offset degrade keeps progFds_ non-empty).
    void g4TestAttachSentinelProg(void) { progFds_.push_back(-1); }

    // EBPF-03 test seams: expose queue depth and switchAgg size after unload().
    size_t g4TestCriticalQueueDepth(void) const { return criticalQueue_.sizeApprox(); }
    size_t g4TestNormalQueueDepth(void) const  { return normalQueue_.sizeApprox(); }
    size_t g4TestSwitchAggSize(void) const {
        std::lock_guard<std::mutex> lk(switchAggMutex_);
        return switchAgg_.size();
    }
    // EBPF-03 test seam: push a fake event into the critical queue to simulate
    // residual events from before unload().
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
    // EBPF-05 test seam: poll thread running state.
    bool g4TestPollThreadRunning(void) const { return pollThreadRunning_.load(); }

#ifdef AATPE_HOST_TEST
    // G2 host seam: feed a raw ring-buffer DATA-PAGE blob (everything AFTER the
    // 4KiB perf_event_mmap_page header) through processRingData() without a real
    // perf fd. On a host there is no tracefs, so we mark all tracepoint offsets
    // NO_CTX (trusted) so the deliver*Event gates reach the attached consumer.
    // Release build (AATPE_HOST_TEST undefined) compiles this out — zero symbols.
    // Returns the number of records processRingData() consumed.
    int hostTestParseRingBlob(const void* data, size_t dataBytes);
#endif
#ifdef AATPE_HOST_TEST
    // G2 host seam: expose parseElfPrograms() so the corrupt/zero-object fault
    // (fault class ②) can be proven to reject a bad ELF without crashing.
    // Release build compiles this out — zero symbols. Returns whatever
    // parseElfPrograms returns (false on any invalid/short/corrupt object).
    bool hostTestParseElfFile(const std::string& path) {
        std::vector<std::vector<uint8_t>> progs;
        std::vector<std::string> tps;
        std::vector<std::vector<int>> mi;
        std::string lic;
        return parseElfPrograms(path, progs, tps, mi, lic);
    }
#endif
#ifdef AATPE_HOST_TEST
    // EBPF-16 host seam: query the one-shot offset-filter WARN flags. kind:
    // 'f'=fork 'x'=exit 's'=switch 'e'=exec. Returns true if the first-drop
    // WARN has already fired in the current load epoch. unload() resets them.
    bool hostTestOffsetWarnFired(char kind) const;
    // EBPF-16 host seam: force one tracepoint kind into DEGRADED offset status
    // (so the deliver*Event offset gate drops events and flips its WARN flag).
    void hostTestSetOffsetDegraded(char kind);
    // EBPF-16 host seam: drive deliverForkEvent() directly (it is private) with
    // a synthetic event, so the flag lifecycle around unload() can be asserted.
    void hostTestDeliverFork(const ForkEvent& ev) { deliverForkEvent(ev); }
    // EBPF-20 host seam: parse an ELF .o and return the per-program mapIndices
    // vectors exactly as parseElfPrograms produced them (including the
    // no-.rel fallback), so the fallback sizing can be asserted on the host.
    bool hostTestParseElfMapIndices(const std::string& path,
                                     std::vector<std::vector<int>>& outMapIndices);
    // EBPF-13 host seam: expose the PERCPU_ARRAY buffer slot sizing.
    static int hostTestEmbeddedCpuBufferSlots(long possibleCpus);
#endif
    // P2-12: hardcoded ctx offsets are ONLY trusted when we actively verified
    // them against the live tracepoint format file (VERIFIED) or when the
    // tracepoint does not read ctx at all (NO_CTX). UNKNOWN (tracefs not
    // readable) and DEGRADED (format mismatch) both cause events from this
    // tracepoint to be filtered. This prevents us from blindly parsing kernel
    // structures at guessed offsets when we cannot verify them.
    bool isSwitchOffsetTrusted(void) const {
        const auto s = offsetStatus_switch_.load(std::memory_order_relaxed);
        return s == TpOffsetStatus::VERIFIED || s == TpOffsetStatus::NO_CTX;
    }
    bool isForkOffsetTrusted(void) const {
        const auto s = offsetStatus_fork_.load(std::memory_order_relaxed);
        return s == TpOffsetStatus::VERIFIED || s == TpOffsetStatus::NO_CTX;
    }
    bool isExecOffsetTrusted(void) const {
        const auto s = offsetStatus_exec_.load(std::memory_order_relaxed);
        return s == TpOffsetStatus::VERIFIED || s == TpOffsetStatus::NO_CTX;
    }
    bool isExitOffsetTrusted(void) const {
        const auto s = offsetStatus_exit_.load(std::memory_order_relaxed);
        return s == TpOffsetStatus::VERIFIED || s == TpOffsetStatus::NO_CTX;
    }
    bool isWakeupOffsetTrusted(void) const {
        const auto s = offsetStatus_wakeup_.load(std::memory_order_relaxed);
        return s == TpOffsetStatus::VERIFIED || s == TpOffsetStatus::NO_CTX;
    }
    // Cumulative events dropped by deliver*Event() because their tracepoint's
    // ctx offsets were known to be wrong (WebUI "ebpf_offset_filtered").
    uint64_t getOffsetFiltered(void) const {
        return offsetFilteredCount_.load(std::memory_order_relaxed);
    }

    // --- Configuration ---
    void setRingPages(int pages) { ringPages_ = pages; }
    void setPollFallbackEnabled(bool enabled) { pollFallbackEnabled_.store(enabled, std::memory_order_relaxed); }
    void setEventConsumer(IEventConsumer* consumer) { eventConsumer_ = consumer; }

    // ROUND-004: eBPF-relevant runtime knobs, decoupled from the global Config
    // struct so host unit tests can drive the convergence logic without linking
    // the whole daemon config. The production hook adapter converts Config -> this.
    struct EbpfRuntimeConfig {
        bool enabled = true;
        int  ringPages = 64;
        bool pollFallback = true;
    };
    // Converge runtime state to a freshly-loaded config. Idempotent: returns
    // true only if it actually changed runtime state (same values -> no action,
    // so a frequently-firing reload hook cannot thrash).
    //   enabled 1->0: safe unload (detach, stop ring/agg, reclaim) -> UNLOADED.
    //   enabled 0->1: load() (goes through the integrity gate + 3-phase loader +
    //                 ROUND-003 offset decision; gate refusal stays POLLING).
    //   ringPages changed while enabled: unload+reload (perf ring mmap size is
    //                 fixed at mmap time) to pick up the new size.
    //   pollFallback: applied live (no reload).
    bool onConfigReload(const EbpfRuntimeConfig& cfg);
    // Production: register onConfigReload with the global config reload registry.
    void registerConfigReloadHook();

    // --- Event polling (call from main loop) ---
    // Returns number of events processed. Non-blocking.
    //
    // pollEvents() is now RING-consumer only. It is called EXCLUSIVELY
    // by the dedicated poll thread (pollThreadFunc). The daemon main loop must
    // NOT call it (that was the dual-consumer race); call dispatchQueuedEvents()
    // instead to consume events the poll thread has already placed on the queue.
    int pollEvents(int maxEvents = 256);

    // --- P1-2: daemon main-loop event consumption ---
    // Drains the bounded event queues (fed by the poll thread) and dispatches
    // each event to eventConsumer_ on the CALLER (daemon main) thread. Returns
    // the number of dispatched events. Non-blocking. Replaces the old direct
    // pollEvents(256) call in the daemon main loop.
    //
    // dual-queue + per-cycle budget.
    //   1. criticalQueue_ (FORK/EXEC/EXIT) is drained COMPLETELY (unbounded)
    //      every cycle — lifecycle events must never wait behind SWITCH traffic.
    //   2. normalQueue_ (SWITCH) is drained up to kDispatchBudgetPerCycle
    //      events per cycle; the remainder stays for the next cycle so a
    //      backlog cannot produce a multi-millisecond decision-loop jitter.
    size_t dispatchQueuedEvents(void);
    // Cumulative events dropped by the queues due to overflow (WebUI "queue_dropped").
    uint64_t getQueueDropped(void) const;
    size_t getQueueSize(void) const;
    // wake hook. Installed by the daemon (aatped.cpp) to flip
    // g_skipNextSleep when a FORK/EXEC/EXIT is enqueued, so the main loop
    // shortens its DECISION_INTERVAL sleep and picks up the lifecycle event
    // within kMinSleepMs instead of up to one full interval. SWITCH events do
    // NOT invoke this hook (too frequent). Called on the poll thread; the
    // callee must be async-signal-safe-ish (it only stores an atomic bool).
    void setCriticalEventWakeHook(std::function<void()> fn) {
        criticalEventWakeHook_ = std::move(fn);
    }
    // events dispatched by the most recent dispatchQueuedEvents()
    // call (WebUI "events_processed_last_cycle").
    uint64_t getLastCycleEventsProcessed(void) const {
        return lastCycleEventsProcessed_.load(std::memory_order_relaxed);
    }
    // Per-cycle caps on event dispatch. BOTH queues are now bounded:
    // criticalQueue_ (FORK/EXEC/EXIT) gets 1024/cycle because each onExec
    // opens /proc/<pid>/cmdline (getPackageByPid) — an unbounded drain at
    // startup (when the poll thread has backlogged tens of thousands of
    // events during daemonInit) takes tens of seconds and exceeds the
    // watchdog heartbeat timeout, causing a kill-restart loop.
    // normalQueue_ (SWITCH) stays at 256/cycle (no /proc I/O in onSwitch).
    static constexpr size_t kDispatchBudgetPerCycle = 256;
    static constexpr size_t kCriticalDispatchBudgetPerCycle = 1024;

    // consolidated event statistics snapshot. Mirrors the
    // aatped-side g_ebpfEvents* counters plus the ring-level overflow counters.
    // Single call site for state JSON / WebUI.
    struct EventStats {
        uint64_t total = 0;          // events seen from the ring
        uint64_t processed = 0;      // fork+exit+switch+exec dispatched
        uint64_t dropped = 0;        // queue overflow drops (oldest evicted)
        uint64_t ringOverflow = 0;   // perf-ring lost (kernel overwrote head/tail)
        uint64_t fallbackCount = 0;  // times we degraded to polling / direct-delivery
        uint64_t consumerLag = 0;    // current queue backlog (size)
        uint64_t windowLost = 0;     // rolling-60s lost
        double   eventRatePerSec = 0.0;
        // per-queue drop breakdown + last-cycle throughput.
        uint64_t criticalDropped = 0;       // FORK/EXEC/EXIT ring overflow
        uint64_t normalDropped = 0;         // SWITCH ring overflow
        uint64_t criticalBacklog = 0;       // approximate critical ring depth
        uint64_t eventsProcessedLastCycle = 0;
    };
    EventStats getEventStats(void) const;

    // --- v83: Dedicated high-frequency poll thread ---
    // Drains ring buffers every ~2ms to prevent event loss under high load.
    bool startPollThread(void);
    void stopPollThread(void);
    bool isPollThreadRunning(void) const { return pollThreadRunning_.load(); }

    // --- Event counters ---
    uint64_t getTotalEvents(void) const { return eventCountTotal_.load(); }
    uint64_t getForkEvents(void) const { return eventCountFork_.load(); }
    uint64_t getExitEvents(void) const { return eventCountExit_.load(); }
    uint64_t getSwitchEvents(void) const { return eventCountSwitch_.load(); }
    uint64_t getExecEvents(void) const { return eventCountExec_.load(); }
    uint64_t getRingLost(void) const { return ringLost_.load(); }
    // windowed loss rate — returns lost/events in the current 60s window.
    // Cumulative ringLost_/eventCountTotal_ are retained for diagnostics only.
    uint64_t getWindowLost(void) const { return windowLost_.load(); }
    uint64_t getWindowEvents(void) const { return windowEvents_.load(); }
    uint64_t getWindowStartMs(void) const { return windowStartMs_.load(); }
    void rotateWindowIfNeeded(void);
    double getEventRatePerSec(void) const;

    // --- Legacy CPU stats (for backward compatibility) ---
    bool readCpuStats(std::vector<CpuSchedStats>& outStats);
    bool readEmbeddedCpuCounts(std::vector<uint64_t>& outCounts);
    static bool readFallbackSchedstat(std::vector<CpuSchedStats>& outStats);

    // --- Degradation ---
    void degrade(const char* reason);
    bool shouldDegrade(void) const;
    // throttled degrade-event summary. Call once per main-loop iteration;
    // internally prints at most one WARN per 30s if degrade events were seen.
    void logDegradeSummaryIfDue(void);
    // auto-recover from a ring-loss degrade (HYBRID) back to
    // EVENT_DRIVEN once the ring buffer has been loss-free for >=30s. A fatal
    // load failure (mode_ == POLLING) is NEVER auto-recovered.
    void recoverIfHealthy(void);
    // ROUND-003: post-load offset verdict decision (shared by load() lambda and
    // the host test seam). On UNKNOWN+knownFamily trust built-in offsets and stay
    // EVENT_DRIVEN; on DEGRADED (mismatch) or UNKNOWN+non-knownFamily latch POLLING.
    void postLoadOffsetDegradeDecision(void);

    // --- Dynamic ring buffer sizing ---
    // Monitors windowed loss rate every 30s and adjusts ringPages_ between
    // kRingPagesNormal (64) and kRingPagesHigh (128). On resize, unloads and
    // reloads the BPF programs with the new page count. Called once per main loop.
    void dynamicRingAdjustIfDue(void);
    int getCurrentRingPages(void) const { return ringPages_; }

    // --- Exponential backoff for load failure retry ---
    // After a fatal load failure (POLLING mode), retries loading with exponential
    // backoff: 1s -> 2s -> 4s -> 8s -> 15s -> 30s -> 60s -> 120s (capped at 5 min).
    // Distinguishes permanent failures (EPERM/EACCES - BPF unsupported) from
    // transient failures (ENOMEM, tracefs not yet mounted). Called from main loop.
    void retryLoadIfDue(void);
    // True if the last load failure was classified as permanent (no retry).
    bool isLoadPermanentlyFailed(void) const { return loadPermanentFailed_; }

    // D3: pure decision — parse newText (a /sys/.../cpu/online mask string such as
    // "0-7" or "0,1,4,5") and return true iff it differs from oldIds. No BPF, no
    // sysfs mutation; host tests drive the 0-7 -> 0,1,4,5 transition directly.
    static bool onlineMaskSetChanged(const std::vector<int>& oldIds, const std::string& newText);

    // --- Partial degradation state ---
    // Returns true if eBPF is ACTIVE but one or more tracepoints are DEGRADED
    // (known-wrong offsets). Used by WebUI to show "ACTIVE PARTIAL" instead of "ACTIVE".
    bool isPartiallyDegraded(void) const;
    // Extended state string: "ACTIVE" / "ACTIVE_PARTIAL" / "DEGRADED" etc.
    const char* getDetailedStateString(void) const;

    // --- Cleanup ---
    void unload(void);

    // --- Embedded bytecode accessors (for testing) ---
    static const uint8_t* getEmbeddedBytecode(size_t* outSize);
    static size_t getEmbeddedBytecodeSize(void);

private:
    // --- State ---
    // LED-016 (R2-001): active_/state_/mode_ are written by the main thread in
    // load()/degrade()/unload() and read by the dedicated poll thread in
    // pollThreadFunc() (line ~2008). Plain non-atomic members are a C++ data
    // race (UB). Promote them to atomics with relaxed/acquire-release ordering
    // (they are status flags, not guarding data).
    std::atomic<bool> active_{false};
    // P2-5: unloaded_ is read/written from the main thread (unload/load/dtor) and
    // must be safe to observe cross-thread. A plain bool can be reordered on
    // weakly-ordered arm64; make it an atomic.
    std::atomic<bool> unloaded_{false};  // idempotent guard for unload() (called from both main and dtor)
    EbpfLoaderType loaderType_ = EbpfLoaderType::NONE;
    std::atomic<EbpfState> state_{EbpfState::UNVERIFIED};   // ROUND-014 P3
    std::atomic<EbpfMode> mode_{EbpfMode::POLLING};
    // runtime tracepoint ctx-offset verification verdict
    // (populated by verifyTracepointOffsets() during load(); see getter above).
    // P2-6: protected by offsetVerifyMutex_ on both write (verifyTracepointOffsets)
    // and read (getOffsetVerifyResult) — the getter is called from the WebUI /
    // state-writer thread while load() runs.
    std::string offsetVerifyResult_ = "unknown";
    mutable std::mutex offsetVerifyMutex_;
    // ROUND-004 adversarial: serializes the unload()/load() lifecycle across the
    // config-hot-reload thread (onConfigReload) and the daemon main loop
    // (retryLoadIfDue). Without it, a reload-triggered unload() could close
    // tracepointFds_/mapFds_ while the main loop's load() is mid-attach into the
    // same vectors -> double-close / use-after-free. Plain (non-recursive):
    // load()/unload() never call back into onConfigReload/retryLoadIfDue.
    std::mutex lifecycleMutex_;
    // per-tracepoint offset verdicts (populated by
    // verifyTracepointOffsets(); used by deliver*Event() to filter events whose
    // ctx offsets are known to mismatch). exec/exit default to NO_CTX because
    // the BPF handlers do not dereference tracepoint ctx for those two.
    // G4-009: promote the per-tracepoint offset verdicts to atomics. They are
    // written by verifyTracepointOffsets() on the load/main thread and read by
    // deliver*Event() on the poll thread AND by getOffsetStatus*/isPartiallyDegraded
    // on the WebUI/state-writer thread. Plain enums were a C++ data race (UB);
    // single-byte enums do not tear on arm64 but the type now enforces the rule.
    std::atomic<TpOffsetStatus> offsetStatus_switch_{TpOffsetStatus::UNKNOWN};
    std::atomic<TpOffsetStatus> offsetStatus_fork_{TpOffsetStatus::UNKNOWN};
    std::atomic<TpOffsetStatus> offsetStatus_exec_{TpOffsetStatus::NO_CTX};
    std::atomic<TpOffsetStatus> offsetStatus_exit_{TpOffsetStatus::NO_CTX};
    std::atomic<TpOffsetStatus> offsetStatus_wakeup_{TpOffsetStatus::UNKNOWN};  /* P1-4 */
    // G4-001: distinct degradation cause for "sched_switch ctx offsets could not
    // be verified". Unlike ring-loss degradation (which auto-recovers after 30s
    // loss-free), offset degradation must NEVER auto-recover: the kernel layout
    // did not become correct on its own. It clears only when a fresh load()'s
    // verifyTracepointOffsets() re-validates the live tracefs format file and
    // returns VERIFIED for sched_switch. While set, recoverIfHealthy() refuses
    // to flip mode back to EVENT_DRIVEN and the state key reports the real cause.
    std::atomic<bool> offsetDegraded_{false};
    // ROUND-003 (F2误伤修复): when tracefs format files are unreadable but the
    // running kernel belongs to the known built-in offset family (4.14.x), we
    // cannot falsify the built-in offsets. We then TRUST them (v3.4.6 behavior)
    // and stay EVENT_DRIVEN, instead of latching POLLING. This is a soft
    // diagnostic: it does NOT count as DEGRADED, so recoverIfHealthy /
    // retryLoadIfDue treat it as healthy. Distinct from offsetDegraded_ which is
    // set only on evidence of a layout mismatch (format readable but != built-in).
    std::atomic<bool> offsetUnverifiedButTrusted_{false};
    // Set by verifyTracepointOffsets(): true when uname release matches the known
    // built-in offset family (currently Linux 4.14.x). On a known family with
    // tracefs unreadable we trust built-in offsets; on a non-known family with
    // tracefs unreadable we stay conservative (POLLING).
    std::atomic<bool> knownKernelFamily_{false};
    // Events dropped by deliver*Event() because the source tracepoint's offsets
    // were actively detected as mismatching. Relaxed atomic — diagnostics only.
    std::atomic<uint64_t> offsetFilteredCount_{0};
    // Convert a TpOffsetStatus to its stable string token. Static so it can be
    // used from the inline public getters above without a `this`.
    static const char* tpOffsetStatusToString(TpOffsetStatus s);
    // P2-7: non-owning raw pointer to the process-lifetime consumer (the daemon's
    // global g_eventConsumer). It is set once by setEventConsumer() at init and is
    // NEVER nulled on unload()/reload so dynamic ring-resize cycles keep working.
    // Lifecycle safety:
    //   * stopPollThread() is called FIRST in unload() and joins the poll thread,
    //     so by the time rings/fds are torn down no poll-thread deliver*Event()
    //     can be in flight.
    //   * Every deliver*Event() / dispatchQueuedEvents() null-checks eventConsumer_
    //     before dereferencing it.
    //   * g_eventConsumer outlives this object (both are process globals), so
    //     unload() (and the dtor) always run before the consumer is destroyed.
    IEventConsumer* eventConsumer_ = nullptr;
    // increased from 32 to 64 pages (256KB per CPU, 2MB total on 8-core)
    // to absorb sched_switch bursts during poll thread backoff windows.
    // increased from 64 to 128 pages (512KB per CPU, 4MB total
    // on 8-core). Under thermal throttling the poll thread (nice=-10) can be
    // starved for 50-100ms by the main loop's heavy I/O cycle; 256KB overflowed
    // at ~205 events/sec loss. 512KB doubles the buffer headroom so transient
    // scheduling delays don't drop events.
    //
    // Dynamic sizing. Start at kRingPagesNormal (64). If windowed
    // loss rate exceeds threshold, upgrade to kRingPagesHigh (128). If loss-free
    // for extended period, downgrade back to normal.
    int ringPages_ = 64;
    // Dynamic ring buffer tuning state
    static constexpr int kRingPagesNormal = 64;    // 256KB/CPU default
    static constexpr int kRingPagesHigh   = 128;   // 512KB/CPU under high load
    static constexpr int kRingPagesLow    = 32;    // 128KB/CPU low-idle (reserved)
    std::atomic<uint64_t> lastRingAdjustMs_{0};
    std::atomic<int> lowLossStreak_{0};  // consecutive 30s windows with <0.1% loss
    static constexpr uint64_t kRingAdjustIntervalMs = 30000;  // evaluate every 30s
    static constexpr double kRingHighLossThreshold = 0.05;    // >5% loss -> upgrade
    static constexpr double kRingLowLossThreshold  = 0.001;  // <0.1% loss -> consider downgrade
    // absolute lost-event burst trigger. When the ring overflows so
    // hard that the poll thread starves, consumed events (eventCountTotal_) stop
    // growing while ringLost_ climbs, so the windowed RATE never reaches 5% and
    // the old rate-only gate never upgraded. This absolute floor (>=200 lost
    // events in a 30s window) forces the 64->128 upgrade regardless of rate.
    static constexpr uint64_t kRingHighLostBurst = 200;
    // Track lost/events at last adjustment to compute delta-based loss rate
    std::atomic<uint64_t> lostAtLastAdjust_{0};
    std::atomic<uint64_t> eventsAtLastAdjust_{0};

    // D3 (v4.2): CPU hotplug online-mask tracking. onlineCpuIds_ was previously
    // only read at load(); after a core on/offlines at runtime the per-cpu rings
    // and the PERCPU_ARRAY aggregation indices were never rebuilt, so a newly
    // online core produced events we never mapped (漏核) and an offline core's
    // stale slot was still averaged (误统). hotplugRebuildIfDue() (called from the
    // existing main-loop dynamic-ring hook) re-reads /sys/devices/system/cpu/online,
    // and when the mask differs from the loaded onlineCpuIds_, performs a guarded
    // unload()+load() under lifecycleMutex_. Throttled; a rebuild failure leaves
    // the scheduler in POLLING (recovered by the existing retryLoadIfDue backoff)
    // and stamps a state key — we never leave a half-built ring live.
    std::atomic<uint64_t> lastHotplugRebuildMs_{0};
    static constexpr uint64_t kHotplugRebuildMinIntervalMs = 5000;
    // number of hotplug-triggered rebuilds attempted / succeeded (WebUI diagnostics).
    std::atomic<uint64_t> hotplugRebuildAttempts_{0};
    std::atomic<uint64_t> hotplugRebuildFailures_{0};

    // Load failure exponential backoff ---
    std::atomic<int> loadFailureCount_{0};
    std::atomic<uint64_t> lastLoadAttemptMs_{0};
    std::atomic<uint64_t> nextRetryDelayMs_{0};
    bool loadPermanentFailed_ = false;  // EPERM/EACCES = BPF unsupported, no retry
    // Backoff sequence: 1s, 2s, 4s, 8s, 15s, 30s, 60s, 120s (capped)
    static constexpr uint64_t kBackoffBaseMs = 1000;
    static constexpr uint64_t kBackoffMaxMs = 300000;  // 5 min cap
    static constexpr int kBackoffMaxRetries = 10;      // after this, stop retrying

    // --- Event storm protection (per-type rate limit) ---
    // Critical events (FORK/EXIT/EXEC) are rate-limited to kMaxCriticalPerSec
    // per second. Above this, excess events are dropped (the periodic /proc
    // scanThreads() is the backstop for missed lifecycle events). This prevents
    // app launch storms (hundreds of forks/sec) from flooding the main loop.
    std::atomic<uint64_t> criticalEventsInWindow_{0};
    std::atomic<uint64_t> criticalWindowStartMs_{0};
    std::atomic<uint64_t> criticalDroppedByRate_{0};
    static constexpr int kMaxCriticalPerSec = 200;  // 200 fork/exit/exec/sec max
    bool rateLimitCriticalEvent(void);  // returns true if event should be dropped
    std::atomic<bool> pollFallbackEnabled_{true};

    // --- v83: Dedicated poll thread ---
    std::atomic<bool> pollThreadRunning_{false};
    std::atomic<bool> pollThreadStop_{false};
    std::thread pollThread_;
    void pollThreadFunc(void);
    // wakeup pipe for poll() — write end signals thread to wake/exit
    int pollWakeupReadFd_ = -1;
    int pollWakeupWriteFd_ = -1;

    // --- P1-2 / v95 issues 6/7/12: dual SPSC event queues ---
    // When queueEnabled_ is true (set by startPollThread), processRingData()
    // pushes consumed events here instead of invoking eventConsumer_ directly,
    // and the daemon main loop runs dispatchQueuedEvents() to drain + dispatch.
    // This makes the poll thread the SOLE ring consumer, eliminating the
    // dual-consumer tail-pointer race. When the poll thread is unavailable the
    // queues are bypassed and events are delivered directly (legacy safe path).
    //
    //     //   criticalQueue_ : FORK/EXEC/EXIT lifecycle events, 256 slots, drained
    //                    unbounded every cycle, triggers the wake hook.
    //   normalQueue_   : SWITCH high-frequency events, 16384 slots, drained
    //                    up to kDispatchBudgetPerCycle per cycle, no wake.
    // raised 8192 -> 16384. Under a VMOS storm the ring saw 132k
    // SWITCH/s and 8192 slots overflowed, dropping 2.7M events. Doubling the
    // depth absorbs bursts; SWITCH events are informational (scoring) so some
    // loss is still acceptable, but this cuts it substantially.
    SpscBoundedQueue<AnyEvent> criticalQueue_{256};
    SpscBoundedQueue<AnyEvent> normalQueue_{16384};
    bool queueEnabled_ = false;
    void setEventQueueEnabled(bool en) { queueEnabled_ = en; }
    // Installed by the daemon; invoked on the poll thread when a critical
    // (FORK/EXEC/EXIT) event is enqueued (issue 8/9).
    std::function<void()> criticalEventWakeHook_;
    // events dispatched by the most recent dispatchQueuedEvents().
    std::atomic<uint64_t> lastCycleEventsProcessed_{0};
    // Routing helpers: push to queue if enabled, else deliver directly to the
    // consumer. Called from processRingData() on the poll thread.
    void deliverForkEvent(const ForkEvent& ev);
    void deliverExitEvent(const ExitEvent& ev);
    void deliverSwitchEvent(const SwitchEvent& ev);
    void deliverExecEvent(const ExecEvent& ev);
    void deliverRunqEvent(const RunqEvent& ev);  // P1-1

    // --- File descriptors ---
    // progFd_ / mapFd_ legacy single fds removed — new flow uses
    // progFds_ / mapFds_ vectors below.
    int eventsMapFd_ = -1;      // PERF_EVENT_ARRAY map fd
    int countersMapFd_ = -1;    // PERCPU_ARRAY counters map fd
    int threadCacheMapFd_ = -1; // HASH map fd
    std::vector<int> progFds_;  // per-program fds (4 programs)
    std::vector<int> mapFds_;   // per-map fds (3 maps)

    // --- Per-CPU perf ring buffers ---
    struct PerfRing {
        int perfFd = -1;
        void* base = nullptr;
        size_t mmapSize = 0;
        int cpu = -1;
    };
    std::vector<PerfRing> rings_;
    std::vector<int> tracepointFds_;  // tracepoint/kprobe perf_event fds
    std::vector<std::string> kprobeEventNames_;  // created kprobe event names for cleanup
    // #21: raw tracepoint link fds (BPF_RAW_TRACEPOINT_OPEN). Closing these fds
    // detaches the raw_tp programs. Separate from tracepointFds_ (which are
    // perf_event fds for kprobe/tracepoint SET_BPF attach).
    std::vector<int> rawTracepointLinks_;
    // #24: real online CPU IDs (parsed from /sys/devices/system/cpu/online).
    // PERCPU_ARRAY slots and perf_event_open are indexed by physical CPU id, NOT
    // by online ordinal. On sparse online sets (e.g. "0-1,4-5") iterating
    // 0..ncpus-1 opens rings for offline cores and misses online high-numbered
    // cores. Populated at load time; ring setup and aggregation use this list.
    std::vector<int> onlineCpuIds_;
    // P2-4: kprobe events are GLOBAL (one debugfs entry per probed function, one
    // kpId); per-CPU perf_event_open+SET_BPF reuses that same kpId. The old code
    // called createKprobeEvent() on every CPU, creating N duplicate kprobe
    // entries in kprobe_events. This cache keyed by "p:<func>"/"r:<func>" ensures
    // the debugfs entry + id are created once and shared across CPUs.
    std::unordered_map<std::string, std::pair<std::string,int>> kprobeIdCache_;
    int activeProgType_ = -1;  // BPF_PROG_TYPE_* of loaded programs
    std::string activeAttachMethod_;  // "kprobe" or "tracepoint"

    // --- Legacy fields (preserved) ---
    int perfFd_ = -1;
    int perfMmapSize_ = 0;
    void* perfRingBase_ = nullptr;
    // syscallPerfFds_ / embeddedPerfFds_ removed — legacy vectors
    // never filled in new load flow, only closed in unload().

    // --- Event counters ---
    std::atomic<uint64_t> eventCountTotal_{0};
    std::atomic<uint64_t> eventCountFork_{0};
    std::atomic<uint64_t> eventCountExit_{0};
    std::atomic<uint64_t> eventCountSwitch_{0};
    // P0-2: sched_switch userspace 1/10 sampling. BPF-side sampling (inserting
    // event_counters[3] lookup + mod-10 branch in the switch BPF program) was
    // deferred because manual bytecode injection would shift all jump offsets
    // and map-relocation pointers. Instead we drop 9/10 of switch events here
    // in the delivery path, before they hit normalQueue_ / onSwitch(). This
    // still pays the ring-buffer read cost but avoids queue push + main-loop
    // dispatch for 90% of events. switchSampleCounter_ counts EVERY switch
    // event; switchSampledOut_ counts how many were dropped by the 1/10 gate.
    std::atomic<uint64_t> switchSampleCounter_{0};
    std::atomic<uint64_t> switchSampledOut_{0};
    std::atomic<uint64_t> eventCountExec_{0};
    // P1-1: runqueue latency samples. runqCount_ = samples delivered to
    // consumers; runqMaxDelayNs_ = worst single wakeup->run latency observed.
    std::atomic<uint64_t> eventCountRunq_{0};
    std::atomic<uint64_t> runqMaxDelayNs_{0};
    std::atomic<uint64_t> ringLost_{0};
    // times the loader stepped down a notch
    // (embedded->syscall->bpftool->polling) or entered direct-delivery fallback.
    std::atomic<uint64_t> fallbackCount_{0};
    // log-throttle state for the degrade() path.
    // degradeEventCount_ counts how many times degrade() was called since the
    // last 30s summary window; degradeSummaryLastMs_ gates the summary log.
    std::atomic<uint64_t> degradeEventCount_{0};
    std::atomic<uint64_t> degradeSummaryLastMs_{0};
#ifdef AATPE_HOST_TEST
    // v4.4.9 R3-F01: counts how many times degrade() took the one-time WARN
    // transition branch. Host-only (release compiles it out); the host test uses
    // it to prove the dedup early-return keeps this at 1 across repeated calls.
    std::atomic<uint32_t> degradeWarnCount_{0};
#endif
    // ring-loss recovery state.
    // lastRingLossMs_ is stamped every time ringLost_ grows; recoverIfHealthy()
    // requires 30s of zero growth before flapping HYBRID->EVENT_DRIVEN.
    // lastRecoverMs_ starts a 60s grace so shouldDegrade() cannot immediately
    // re-degrade on the still-elevated cumulative lost/total ratio (which only
    // proves a PAST burst, not a current one).
    std::atomic<uint64_t> lastRingLossMs_{0};
    std::atomic<uint64_t> lastRecoverMs_{0};
    // windowed loss counters — reset every 60s to prevent cumulative
    // loss rate from permanently elevating after a transient burst.
    std::atomic<uint64_t> windowLost_{0};
    std::atomic<uint64_t> windowEvents_{0};
    std::atomic<uint64_t> windowStartMs_{0};
    static constexpr uint64_t kWindowRotateMs = 60000;  // 60 seconds
    // total bytes consumed from ring buffers (perf record sizes).
    // Used to compute average event size so overflow-byte-count can be
    // converted to an estimated lost-event count (fixes 190% false loss rate).
    std::atomic<uint64_t> ringConsumedBytes_{0};
    std::atomic<uint64_t> lastRateCheckNs_{0};
    std::atomic<uint64_t> eventsAtLastCheck_{0};
    std::atomic<double> eventRate_{0.0};
    // LED-020 (R2-005): per-CPU ring coverage. When setupPerCpuRingBuffers()
    // fails to open/mmap a ring for some CPUs (offlined/restricted CPUs on
    // locked ROMs), events produced on those CPUs are silently dropped by the
    // kernel. Record X/Y so logDegradeSummary and the EventStats snapshot can
    // surface the gap instead of leaving it as a one-line WARN.
    std::atomic<int> ringsMappedCount_{0};
    std::atomic<int> ringsTotalCount_{0};
    int getRingsMapped(void) const { return ringsMappedCount_.load(std::memory_order_relaxed); }
    int getRingsTotal(void) const { return ringsTotalCount_.load(std::memory_order_relaxed); }

    // --- Event aggregation for high-frequency switch events ---
    struct SwitchAggregate {
        uint32_t nextPid = 0;
        uint64_t count = 0;
        uint64_t lastTs = 0;
        char nextComm[16] = {0};
    };
    std::unordered_map<uint32_t, SwitchAggregate> switchAgg_;
    // mutex protecting switchAgg_ against concurrent access.
    // EBPF-17 verification (2026-10): the review note suggested this mutex was
    // redundant because "pollEvents only runs on the poll thread". That is
    // factually wrong — aatped_main.cpp loopEbpfDrain() calls
    // g_ebpfScheduler.pollEvents(256) on the DAEMON MAIN LOOP whenever
    // isPollThreadRunning() is false (legacy/POLLING fallback), and pollEvents
    // both inserts via processRingData()->localSwitchBatch merge AND erases via
    // cleanupSwitchAgg(). The start/stop transitions (startPollThread /
    // stopPollThread join / degrade) create windows where both threads can be
    // inside pollEvents(), and unload() clears switchAgg_ after joining. A
    // std::unordered_map is not safe across that — the mutex STAYS (cheap:
    // one lock per switch batch / 5-min sweep). Removing it would be a real
    // use-after-insert race.
    // unordered_map is not thread-safe; find/insert/erase must all hold the lock.
    mutable std::mutex switchAggMutex_;  // mutable so const test accessors can lock
    static constexpr int kSwitchAggregateThreshold = 10;
    // periodic cleanup of switchAgg_ to prevent unbounded growth.
    // Entries for processes that haven't switched in >5min are evicted.
    // Also hard-caps at kMaxSwitchAggEntries to protect hash lookup latency.
    //
    // the cleanup interval was 1 HOUR while the entry TTL is
    // only 5 MINUTES, so stale entries lingered for up to ~65 minutes resident.
    // The interval is now matched to the TTL (5 min) and a lazy eviction runs
    // every kSwitchAggLazyEvictInserts inserts so expired entries are purged
    // promptly even between periodic sweeps.
    std::atomic<uint64_t> lastSwitchAggCleanupMs_{0};
    static constexpr uint64_t kSwitchAggCleanupIntervalMs = 300000;  // 5 min (= TTL)
    static constexpr uint64_t kSwitchAggEntryTtlMs = 300000;  // 5 minutes
    static constexpr size_t kMaxSwitchAggEntries = 10000;
    // lazy eviction trigger — every N map inserts, sweep
    // expired entries inline under switchAggMutex_ (which the SWITCH aggregation
    // path already holds). Cheap when the map is small (the common case).
    static constexpr uint64_t kSwitchAggLazyEvictInserts = 512;
    uint64_t switchAggInsertCount_ = 0;  // only touched under switchAggMutex_
    void cleanupSwitchAgg(uint64_t nowMs);
    // Body of cleanupSwitchAgg(); assumes switchAggMutex_ is ALREADY held.
    // Split out so the lazy insert-path can call it without re-locking.
    void evictStaleSwitchAggLocked(uint64_t nowMs);

    // --- Loading methods ---
    bool tryLoadEmbeddedSyscall(void);
    bool tryLoadSyscallPrecompiled(void);
    bool tryLoadBpftoolPrecompiled(void);

    // --- Low-level BPF helpers ---
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
    // Load with explicit (byte_offset, map_index) relocation pairs.
    // Patches the 32-bit immediate at each offset+4 with the corresponding
    // map FD. Required for legacy bpf_map_def bytecode where LD_IMM uses
    // src_reg=0 (not PSEUDO_MAP_FD src_reg=1), so the sequential scanner
    // cannot identify map references automatically.
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

    // 4.14 kernel fix: open a ring-buffer-only perf event (no SET_BPF)
    // to avoid EEXIST when the same BPF prog is already attached to the tp.
    bool openRingBufferOnlyEvent(int tpId, int cpu, int& outPerfFd);
    // Set up per-CPU ring buffers after a single tracepoint attach.
    // attachFd/attachCpu is the already-attached perf event (reused as that CPU's ring).
    // Returns number of ring buffers successfully mapped.
    int setupPerCpuRingBuffers(int tpId, int attachFd, int attachCpu, int ncpus, int progIndex);

    // D3: re-read the online CPU mask and, if it diverged from the loaded
    // onlineCpuIds_, perform a guarded unload()+load() to rebuild per-cpu rings and
    // PERCPU_ARRAY aggregation indices. Called from dynamicRingAdjustIfDue() (the
    // existing every-main-loop hook) so no aatped_main.cpp change is needed.
    // Host-testable decision core: returns the NEW mask when a rebuild was
    // triggered (throttled, active-only), empty vector when no action taken.
    std::vector<int> hotplugRebuildIfDue(void);

    // --- KPROBE attach support ---
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
    // P2-4: create the kprobe debugfs entry + resolve its id ONCE per probed
    // function (deduped in kprobeIdCache_). Per-CPU callers then reuse the kpId
    // via attachTracepoint() instead of re-creating the event. Returns kpId (>0)
    // or -1 on failure; outEventName receives the debugfs event name (also tracked
    // in kprobeEventNames_ for unload cleanup).
    int ensureKprobeEvent(const char* funcName, bool isKretprobe,
                          std::string& outEventName);
    int createKprobeEvent(const char* funcName, bool isKretprobe,
                           std::string& outEventName);
    int readKprobeEventId(const std::string& eventName);
    void cleanupKprobeEvents(void);

    // --- ELF parsing for precompiled .o ---
    bool extractBpfTextFromElf(const std::string& path,
                                 std::vector<uint8_t>& outText,
                                 std::string& outLicense);
    bool parseElfPrograms(const std::string& path,
                           std::vector<std::vector<uint8_t>>& outPrograms,
                           std::vector<std::string>& outTpPaths,
                           std::vector<std::vector<int>>& outMapIndices,
                           std::string& outLicense);

    // --- Perf ring buffer ---
    bool mmapPerfRing(int perfFd, size_t pageCount);
    bool mmapPerfRingForCpu(int cpu, int perfFd, size_t pageCount);
    // Update events_map (PERF_EVENT_ARRAY) with retry-on-update-failure.
    // NOTE: We do NOT verify via BPF_MAP_LOOKUP_ELEM because some Android 4.14
    // kernels do not implement user-space lookup for PERF_EVENT_ARRAY (returns
    // ENOTSUPP/errno=524), and even when supported the returned value is a
    // kernel virtual address. Trust the update syscall's return value instead.
    bool updateEventsMapWithVerify(int cpu, int perfFd);
    void unmapPerfRing(void);
    void unmapAllRings(void);
    int processRingData(PerfRing& ring, int maxEvents);

    // --- Environment detection ---
    void detectEnvironment(void);
    // #48/#49: probe the actually-working BPF capability set and write the
    // minimal-permission state file (only attempt operations that probe OK).
    void runCapabilityDetection(void);
    bool checkDebugfsTracefs(void);
    int readKernelVersion(void);
    // read live tracepoint `format` files and compare the
    // hardcoded Linux-4.14 ctx offsets used by sched_events.bpf.c. Populates
    // offsetVerifyResult_. Called once from load(); graceful no-op if unreadable.
    void verifyTracepointOffsets(void);
    // EBPF-15: the legacy tryLoadPrecompiled / loadEmbeddedBytecode /
    // attachPerfEvent declarations were REMOVED — verified zero callers in the
    // whole tree (grep over *.cpp/*.h incl. tests/). See cpp file note.
};

} // namespace aatpe_ebpf

#endif // AATPE_EBPF_H
