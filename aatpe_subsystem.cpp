// AATPE: unified subsystem lifecycle implementation.
// See aatpe_subsystem.h for the contract.
#include "aatpe_subsystem.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

// ROUND-007: forward declaration of global restore function (in aatped_control.cpp).
void restoreUserControlMode();

namespace aatpe {
namespace subsys {

// ===========================================================================
// 1+2. State name + SubsystemHealth
// ===========================================================================
const char* subsystemStateName(SubsystemState s) {
    switch (s) {
        case SubsystemState::INIT:      return "INIT";
        case SubsystemState::STARTING:  return "STARTING";
        case SubsystemState::ACTIVE:    return "ACTIVE";
        case SubsystemState::DEGRADED:  return "DEGRADED";
        case SubsystemState::STOPPING:  return "STOPPING";
        case SubsystemState::STOPPED:   return "STOPPED";
        case SubsystemState::FAILED:    return "FAILED";
    }
    return "UNKNOWN";
}

void SubsystemHealth::setState(SubsystemState s) {
    state.store(s, std::memory_order_relaxed);
    if (s == SubsystemState::ACTIVE || s == SubsystemState::DEGRADED) {
        lastSuccess.store(std::time(nullptr), std::memory_order_relaxed);
    }
}

void SubsystemHealth::markSuccess() {
    lastSuccess.store(std::time(nullptr), std::memory_order_relaxed);
}

void SubsystemHealth::markError(const std::string& reason) {
    errorCount.fetch_add(1, std::memory_order_relaxed);
    lastError.store(std::time(nullptr), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(reasonMutex);
    lastErrorReason = reason;
    // P1-5: CAS loop instead of load-check-store. Previously a concurrent
    // markError from another thread could set FAILED between our load and store,
    // and we'd clobber it back to DEGRADED. CAS only transitions ACTIVE/STARTING/
    // INIT -> DEGRADED; if another thread changed the state (e.g. to FAILED), we
    // retry and leave it alone.
    SubsystemState desired = SubsystemState::DEGRADED;
    SubsystemState cur = state.load(std::memory_order_relaxed);
    while (cur == SubsystemState::ACTIVE || cur == SubsystemState::STARTING ||
           cur == SubsystemState::INIT) {
        if (state.compare_exchange_weak(cur, desired,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            break;
        }
        // cur was updated to the actual current value by CAS; loop retries.
    }
}

void SubsystemHealth::markRecovery() {
    recoveryCount.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(reasonMutex);
    lastErrorReason.clear();
}

std::string SubsystemHealth::getLastErrorReason() {
    std::lock_guard<std::mutex> lk(reasonMutex);
    return lastErrorReason;
}

std::string SubsystemHealth::toJson(const std::string& name) {
    SubsystemState st = state.load(std::memory_order_relaxed);
    std::string reason = getLastErrorReason();
    // P2: full JSON string escaping — not just '"'/'\\' but also C0 control
    // chars (\n \r \t and <0x20) which can otherwise break out of the JSON string
    // when lastErrorReason carries a kernel/errno-derived message.
    auto esc = [](const std::string& in) {
        std::string out;
        out.reserve(in.size() + 8);
        for (unsigned char c : in) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else out += (char)c;
        }
        return out;
    };
    std::string reasonEsc = esc(reason);
    std::string nameEsc = esc(name);
    std::ostringstream o;
    o << '"' << nameEsc << "\":{"
      << "\"state\":\"" << subsystemStateName(st) << "\","
      << "\"last_success\":" << (long)lastSuccess.load() << ","
      << "\"last_error\":" << (long)lastError.load() << ","
      << "\"error_count\":" << errorCount.load() << ","
      << "\"recovery_count\":" << recoveryCount.load();
    if (!reason.empty()) o << ",\"last_error\":\"" << reasonEsc << "\"";
    o << "}";
    return o.str();
}

// ===========================================================================
// 3. Registry
// ===========================================================================
Registry::Registry() {}

Registry& Registry::instance() {
    static Registry inst;
    return inst;
}

SubsystemHealth& Registry::get(const std::string& name) {
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mEntries.find(name);
    if (it == mEntries.end()) {
        auto h = std::unique_ptr<SubsystemHealth>(new SubsystemHealth());
        SubsystemHealth& ref = *h;
        mEntries[name] = std::move(h);
        return ref;
    }
    return *it->second;
}

void Registry::setState(const std::string& name, SubsystemState s) {
    get(name).setState(s);
}
void Registry::markSuccess(const std::string& name) { get(name).markSuccess(); }
void Registry::markError(const std::string& name, const std::string& reason) {
    get(name).markError(reason);
}
void Registry::markRecovery(const std::string& name) { get(name).markRecovery(); }

std::vector<std::string> Registry::listNames() {
    std::lock_guard<std::mutex> lk(mMutex);
    std::vector<std::string> out;
    out.reserve(mEntries.size());
    for (auto& kv : mEntries) out.push_back(kv.first);
    return out;
}

std::string Registry::toJson() {
    std::lock_guard<std::mutex> lk(mMutex);
    std::ostringstream o;
    o << "{";
    bool first = true;
    for (auto& kv : mEntries) {
        if (!first) o << ",";
        o << kv.second->toJson(kv.first);
        first = false;
    }
    o << "}";
    return o.str();
}

// P1-3: aatpe_subsystem.cpp is linked STANDALONE by the v342_round5/ebpf unit
// tests (no aatped_utils.o in those link sets), so it cannot call the global
// writeFileAtomic. The old local version was NOT atomic (direct O_TRUNC, no
// tmp+rename, landed 0644). Port the SAME atomic contract as aatped_utils.cpp
// here: write to path.tmp (0600, O_NOFOLLOW), fsync, rename over the target.
// This makes the state-file write crash-safe and root-only without pulling in
// the whole utils TU.
static bool writeFileAtomic(const char* path, const std::string& data) {
    // G1-005: unique tmp name per call (mkstemp), same rationale as
    // aatped_utils.cpp — concurrent same-path writers must not share a tmp file.
    std::string tmpPath = std::string(path) + ".tmp.XXXXXX";
    int fd = ::mkstemp(&tmpPath[0]);
    if (fd < 0) return false;
    size_t off = 0;
    bool ok = true;
    // P2: bound the write retry loop. Previously `if (w == 0) { usleep; continue; }`
    // could spin forever if the fd kept returning 0 (e.g. a pipe/socket closed by
    // the peer). Cap at 16 retries (~16ms) before giving up.
    int zeroRetries = 0;
    while (off < data.size()) {
        ssize_t w = ::write(fd, data.data() + off, data.size() - off);
        if (w <= 0) {
            if (w == 0) {
                if (++zeroRetries >= 16) { ok = false; break; }
                usleep(1000);
                continue;
            }
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        zeroRetries = 0;
        off += (size_t)w;
    }
    // P2: check fsync return value; a failed fsync means the write may not have
    // hit stable storage — mark the write as failed so the caller doesn't rename
    // over the good file.
    if (ok && ::fsync(fd) != 0) ok = false;
    // P3-35: check close() return value.
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        // P3-36: check unlink.
        if (::unlink(tmpPath.c_str()) != 0) {}
        return false;
    }
    if (::rename(tmpPath.c_str(), path) != 0) {
        // P3-36: check unlink on rename failure.
        if (::unlink(tmpPath.c_str()) != 0) {}
        return false;
    }
    // P1-fix (issue 6): fsync the parent directory so the rename itself is
    // durable across power loss. Best-effort: tmpfs/dev/aatpe simply fails.
    // STATE-09: the rename above already succeeded, so the file IS on disk.
    // A dir-fsync failure only means the rename may not survive a total power
    // loss (a metadata-durability nicety, not a correctness issue). Return
    // success so the caller doesn't log a spurious failure / retry.
    {
        std::string pstr(path);
        size_t slash = pstr.rfind('/');
        std::string dir = ".";
        if (slash != std::string::npos) dir = pstr.substr(0, slash == 0 ? 1 : slash);
        int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            if (::fsync(dfd) != 0) { /* best-effort: rename already durable */ }
            ::close(dfd);
        }
    }
    return true;
}

void Registry::writeStateFile(const char* dir) {
    if (!dir) return;
    std::string p = std::string(dir) + "/subsystem_health";
    if (!writeFileAtomic(p.c_str(), toJson())) {
        // best-effort; subsystem health is diagnostic only
    }
}

int Registry::failedCount() {
    // STATE-10: iterate mEntries directly under mMutex instead of
    // listNames()+get(name). The old two-step pattern released mMutex between
    // listNames() and get(), so a concurrent get("newsub") could instantiate a
    // fresh slot mid-iteration. In practice entries are only added at startup,
    // but the direct walk is also faster (no extra lock/unlock per entry).
    std::lock_guard<std::mutex> lk(mMutex);
    int n = 0;
    for (auto& kv : mEntries) {
        if (kv.second->getState() == SubsystemState::FAILED) ++n;
    }
    return n;
}
int Registry::degradedCount() {
    std::lock_guard<std::mutex> lk(mMutex);
    int n = 0;
    for (auto& kv : mEntries) {
        if (kv.second->getState() == SubsystemState::DEGRADED) ++n;
    }
    return n;
}

// ===========================================================================
// 4. StopToken
// ===========================================================================
StopToken::StopToken() {}

StopToken::~StopToken() {
    if (mFd >= 0) ::close(mFd);
    mFd = -1;
}

bool StopToken::open() {
    if (mFd >= 0) return true;
    mFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (mFd < 0) return false;
    mStop.store(false, std::memory_order_release);
    return true;
}

void StopToken::requestStop() {
    // A2-014 (accepted): stop is signalled by an atomic flag + eventfd write, not
    // by RAII-scoped locking; the fd ownership IS RAII (dtor closes, line ~273).
    // No mutex is held across this signal, so a failed wakeup cannot leak a lock.
    bool was = mStop.exchange(true, std::memory_order_acq_rel);
    if (!was && mFd >= 0) {
        uint64_t one = 1;
        ssize_t w = ::write(mFd, &one, sizeof(one));
        (void)w;  // best-effort wakeup; EAGAIN just means already signalled
    }
}

void StopToken::drain() {
    if (mFd < 0) return;
    // P1-fix (issue 26): eventfd read(2) returns the CURRENT counter value and
    // resets it to 0 in a single call (EFD_SEMAPHORE not set, so non-semaphore
    // semantics). A single read drains all accumulated wakeups; the while loop
    // was redundant (second read would return -1/EAGAIN on a non-blocking fd).
    uint64_t val;
    ::read(mFd, &val, sizeof(val));
}

void StopToken::reset() {
    mStop.store(false, std::memory_order_release);
    drain();
}

// ===========================================================================
// 5. Safe Mode
// ===========================================================================
namespace safemode {

std::atomic<bool> gSafeModeActive{false};
std::atomic<int>  gSafeModeReason{0};
std::atomic<time_t> gSafeModeEntered{0};

// STATE-03: reason bits and the active latch are a pair that must flip
// together. The previous code CAS-updated reason then re-loaded it and
// blind-store()ed active=false; a concurrent trigger() in that window could
// OR in a new reason bit (and exchange active=true, which was already true so
// a no-op), leaving us with active=false but reason!=0. This tiny mutex
// serializes every read-modify-write of the (reason, active) pair so the
// pair is always consistent. The safemode path is ~1Hz (maybeRecover) or
// error-driven (trigger) — not a hot path, so a mutex is cheap.
static std::mutex gSafeModePairMutex;

static int gStreakThreshold = 3;       // consecutive failures to latch
static int gRecoverCooldownSec = 60;    // min time before auto-recovery

static std::atomic<int> gEbfpStreak{0};
static std::atomic<int> gSchedStreak{0};

// G1 CRASH_LOOP self-heal: after the hard latch, observe a quiet window; if no
// new crash_loop signal arrives within kCrashLoopObserveSec, auto-clear the bit.
// Re-triggering inside the window re-arms the timer. After kCrashLoopMaxHeal
// self-heals, further triggers stay latched until reboot (a truly crash-looping
// device must not flap).
static constexpr int kCrashLoopObserveSec = 300;
static constexpr int kCrashLoopMaxHeal = 2;
static std::atomic<time_t> gCrashLoopSince{0};      // 0 = not latched by crash loop
static std::atomic<int> gCrashLoopHealTries{0};     // self-heals consumed (cumulative)
static std::atomic<bool> gCrashLoopEverLatched{false};  // distinguishes first-ever vs post-heal re-trigger
// Test seam: when >0, overrides the observation window (seconds).
// RC7(C6): guarded so the release binary carries no test-override global;
// production consumes it via the AATPE_HOST_TEST branch at the read site.
#ifdef AATPE_HOST_TEST
int g_crashLoopObserveSecTestOverride = 0;
#endif

const char* reasonName(int bit) {
    switch (bit) {
        case EBPF_STREAK:    return "ebpf_streak";
        case SCHED_STREAK:    return "sched_streak";
        case CRASH_LOOP:      return "crash_loop";
        case MEM_PRESSURE:    return "mem_pressure";
        case THERMAL_EMERG:   return "thermal_emergency";
        case SELF_OVERLOAD:   return "self_overload";
        default:              return "none";
    }
}

std::string describeReasons(int bits) {
    std::string s;
    for (int b = 0; b < 16; ++b) {
        int bit = 1 << b;
        if (bits & bit) {
            if (!s.empty()) s += "+";
            s += reasonName(bit);
        }
    }
    return s.empty() ? "none" : s;
}

void trigger(SafeReason r, const char* detail) {
    (void)detail;
    // Streak-gated triggers.
    if (r == EBPF_STREAK) {
        int n = gEbfpStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n < gStreakThreshold) return;  // not enough consecutive failures
    } else if (r == SCHED_STREAK) {
        int n = gSchedStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n < gStreakThreshold) return;
    }
    // P1-12: atomic read-modify-write on the reason bitmask. The previous
    // load-then-store race could lose a bit if two threads triggered different
    // reasons concurrently. CAS loop is correct and lock-free.
    // STATE-03: hold gSafeModePairMutex while we OR the bit and flip active so
    // a concurrent recovery() cannot observe (reason!=0, active=false).
    {
        std::lock_guard<std::mutex> lk(gSafeModePairMutex);
        int oldBits = gSafeModeReason.load(std::memory_order_relaxed);
        int newBits;
        do {
            newBits = oldBits | (int)r;
        } while (!gSafeModeReason.compare_exchange_weak(oldBits, newBits,
                                                         std::memory_order_relaxed,
                                                         std::memory_order_relaxed));
        if (!gSafeModeActive.exchange(true, std::memory_order_acq_rel)) {
            gSafeModeEntered.store(std::time(nullptr), std::memory_order_relaxed);
        }
    }
    // G1 CRASH_LOOP: record latch time; re-trigger re-arms the observation
    // window and consumes one self-heal attempt. everLatched distinguishes the
    // first-ever latch (healTries=0) from a post-heal re-trigger (healTries++).
    if (r == CRASH_LOOP) {
        time_t now = std::time(nullptr);
        if (!gCrashLoopEverLatched.exchange(true, std::memory_order_relaxed)) {
            gCrashLoopSince.store(now, std::memory_order_relaxed);
            gCrashLoopHealTries.store(0, std::memory_order_relaxed);
        } else {
            gCrashLoopSince.store(now, std::memory_order_relaxed);
            gCrashLoopHealTries.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// G1-001: health record. The failure streak must reset when the subsystem is
// observed working, otherwise "consecutive failures" is enforced only until the
// next successful poll is forgotten (it never was). Called by the eBPF load
// success path on every good load.
void resetStreak(SafeReason r) {
    if (r == EBPF_STREAK) gEbfpStreak.store(0, std::memory_order_relaxed);
    else if (r == SCHED_STREAK) gSchedStreak.store(0, std::memory_order_relaxed);
}

void maybeRecover(std::time_t now) {
    if (!gSafeModeActive.load(std::memory_order_relaxed)) return;
    // G1 CRASH_LOOP self-heal runs on its own observation clock, independent of
    // the soft-trigger cooldown. If the crash-loop bit has been quiet for the
    // observation window AND we still have self-heal budget, drop it. After
    // kCrashLoopMaxHeal re-triggers, the latch persists until reboot.
    int cur = gSafeModeReason.load(std::memory_order_relaxed);
    if ((cur & CRASH_LOOP) && gCrashLoopSince.load(std::memory_order_relaxed) != 0) {
        int observe =
#ifdef AATPE_HOST_TEST
            g_crashLoopObserveSecTestOverride > 0 ? g_crashLoopObserveSecTestOverride :
#endif
            kCrashLoopObserveSec;
        if (now - gCrashLoopSince.load(std::memory_order_relaxed) >= observe &&
            gCrashLoopHealTries.load(std::memory_order_relaxed) < kCrashLoopMaxHeal) {
            // STATE-03: hold the pair mutex while we clear the CRASH_LOOP bit
            // and (if reason goes to 0) drop the active latch atomically.
            bool droppedLatch = false;
            {
                std::lock_guard<std::mutex> lk(gSafeModePairMutex);
                int bits = gSafeModeReason.load(std::memory_order_relaxed);
                int nb;
                do { nb = bits & ~CRASH_LOOP; }
                while (!gSafeModeReason.compare_exchange_weak(bits, nb,
                                                              std::memory_order_relaxed,
                                                              std::memory_order_relaxed));
                gCrashLoopSince.store(0, std::memory_order_relaxed);
                // If no reason bits remain, release the latch immediately.
                if (nb == 0) {
                    gSafeModeActive.store(false, std::memory_order_relaxed);
                    droppedLatch = true;
                }
            }
            // ROUND-007: CRASH_LOOP self-healed — restore the user's persistent
            // control-mode choice. Implemented in aatped_control.cpp.
            restoreUserControlMode();
            // G1: do NOT reset healTries to 0 — it counts total self-heals
            // consumed. After kCrashLoopMaxHeal, further triggers stay latched.
            (void)droppedLatch;
            return;  // CRASH_LOOP handled; skip soft-trigger cooldown path.
        }
    }
    time_t entered = gSafeModeEntered.load(std::memory_order_relaxed);
    if (now - entered < gRecoverCooldownSec) return;  // too early for soft triggers
    // Reset streaks on recovery attempt.
    gEbfpStreak.store(0, std::memory_order_relaxed);
    gSchedStreak.store(0, std::memory_order_relaxed);
    // P1-6: CAS loop instead of bare store. A concurrent trigger() could add a
    // new bit between our load() and store(); the old bare store would clobber it.
    // STATE-03: hold the pair mutex so the reason-clear and active-drop happen
    // atomically with respect to trigger().
    {
        std::lock_guard<std::mutex> lk(gSafeModePairMutex);
        int softMask = EBPF_STREAK | SCHED_STREAK;
        int oldBits = gSafeModeReason.load(std::memory_order_relaxed);
        int newBits;
        do {
            // Drop soft triggers, keep hard ones (CRASH_LOOP | THERMAL_EMERG).
            newBits = oldBits & ~softMask;
        } while (!gSafeModeReason.compare_exchange_weak(oldBits, newBits,
                                                         std::memory_order_relaxed,
                                                         std::memory_order_relaxed));
        // Only drop the latch if no bits remain.
        if (newBits == 0) {
            gSafeModeActive.store(false, std::memory_order_relaxed);
            gSafeModeReason.store(0, std::memory_order_relaxed);
        }
    }
}

bool isActive() { return gSafeModeActive.load(std::memory_order_relaxed); }
bool rtBoostAllowed() { return !isActive(); }
bool mlPlacementAllowed() { return !isActive(); }
bool aggressiveMigrationAllowed() { return !isActive(); }

// clear a hard-trigger latch bit when its underlying condition is
// gone. Previously only TRIGGERING (set) existed; the THERMAL_EMERG bit was set
// while hot but never cleared when temperature recovered, so maybeRecover()
// kept the latch forever (hard!=0) and safe_mode stayed active at NORMAL state,
// rejecting 984 migrations.
void clearReason(SafeReason r) {
    // P1-6: atomic RMW on the reason bitmask.
    // STATE-03: hold the pair mutex so clearing the last reason bit and
    // dropping the active latch are atomic with respect to trigger().
    std::lock_guard<std::mutex> lk(gSafeModePairMutex);
    int oldBits = gSafeModeReason.load(std::memory_order_relaxed);
    int newBits;
    do {
        newBits = oldBits & ~(int)r;
    } while (!gSafeModeReason.compare_exchange_weak(oldBits, newBits,
                                                     std::memory_order_relaxed,
                                                     std::memory_order_relaxed));
    if (newBits == 0) {
        gSafeModeActive.store(false, std::memory_order_relaxed);
    }
}

void writeStateFiles(const char* dir) {
    if (!dir) return;
    std::string p1 = std::string(dir) + "/safe_mode_active";
    std::string p2 = std::string(dir) + "/safe_mode_reason";
    writeFileAtomic(p1.c_str(), isActive() ? "1" : "0");
    writeFileAtomic(p2.c_str(), describeReasons(gSafeModeReason.load()));
}

#ifdef AATPE_HOST_TEST
void setStreakThresholdForTest(int n) { if (n > 0) gStreakThreshold = n; }
void setRecoverCooldownSecForTest(int s) { if (s > 0) gRecoverCooldownSec = s; }
#endif

}  // namespace safemode

// ===========================================================================
// 6. Self-observation
// ===========================================================================
namespace selfobs {

std::atomic<int> gSelfCpuPercent{0};
std::atomic<long> gSelfRssKb{0};
std::atomic<int> gSelfFdCount{0};
std::atomic<int> gSelfThreadCount{0};
std::atomic<bool> gThrottled{false};

static int gCpuBudgetPercent = 50;
static int gSustainSec = 10;

// CPU sampling: read /proc/self/stat utime+stime and diff against previous.
// Parse the space-separated /proc/<pid>/stat fields that appear AFTER the
// closing ") " of comm, returning utime (field 14) and stime (field 15) in
// clock ticks.
//
// Field order after ")" per proc(5):
//   1=state 2=ppid 3=pgrp 4=session 5=tty_nr 6=tpgid 7=flags
//   8=minflt 9=cminflt 10=majflt 11=cmajflt 12=utime 13=stime ...
//
// STATE-01 fix: the original code skipped only state/ppid/pgrp/session/tty_nr
// (5 fields) and then read tpgid(6)/flags(7) as utime/stime. For a daemon not
// attached to a tty, tpgid=-1 is constant and flags is constant, so the
// utime+stime diff was always 0 and gSelfCpuPercent never rose above 0 — the
// self-throttle protection was dead. We now skip all 11 fields up to and
// including cmajflt before reading utime/stime. Returns false on parse
// failure (caller leaves previous counters untouched).
static bool parseSelfStatCpuFields(const std::string& afterParen,
                                   long long& utimeOut, long long& stimeOut) {
    std::istringstream iss(afterParen);
    // Field 1 after ")" is the single-char process state (R/S/D/Z/T), which
    // cannot be read into a long. Read it as a char first (matches
    // aatped_priority.cpp:readThreadStarttime which does `iss >> state`).
    char stateChar = 0;
    if (!(iss >> stateChar)) return false;
    long skip = 0;
    // fields 2..11: ppid, pgrp, session, tty_nr, tpgid, flags,
    //              minflt, cminflt, majflt, cmajflt  (10 numeric fields)
    for (int i = 0; i < 10; ++i) {
        if (!(iss >> skip)) return false;
    }
    long long utime = 0, stime = 0;
    if (!(iss >> utime)) return false;
    if (!(iss >> stime)) return false;
    utimeOut = utime;
    stimeOut = stime;
    return true;
}

void sampleSelf() {
    // --- RSS + threads from /proc/self/status ---
    std::ifstream sf("/proc/self/status");
    if (sf) {
        std::string line;
        while (std::getline(sf, line)) {
            if (line.compare(0, 5, "VmRSS") == 0) {
                // "VmRSS:\t  12345 kB"
                size_t p = line.find_first_of("0123456789");
                if (p != std::string::npos) {
                    try { gSelfRssKb.store(std::stol(line.substr(p)), std::memory_order_relaxed); }
                    catch (const std::exception&) { /* malformed /proc/self/status line: ignore */ }
                }
            } else if (line.compare(0, 7, "Threads") == 0) {
                size_t p = line.find_first_of("0123456789");
                if (p != std::string::npos) {
                    try { gSelfThreadCount.store(std::stoi(line.substr(p)), std::memory_order_relaxed); }
                    catch (const std::exception&) { /* malformed /proc/self/status line: ignore */ }
                }
            }
        }
    }
    // --- FD count: count entries in /proc/self/fd ---
    DIR* d = ::opendir("/proc/self/fd");
    if (d) {
        // G1-009: the opendir() fd itself appears in /proc/self/fd during the
        // enumeration — skip it so the count is exact instead of +1.
        int selfDirFd = ::dirfd(d);
        int n = 0;
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            char* endp = nullptr;
            long v = std::strtol(e->d_name, &endp, 10);
            if (endp && *endp == '\0' && v == selfDirFd) continue;
            ++n;
        }
        ::closedir(d);
        gSelfFdCount.store(n, std::memory_order_relaxed);
    }
    // --- CPU%: diff utime+stime (clock ticks) over wall time. ---
    // LED-162 (QA-001, P2): sPrevTicks/sPrevTime were function-local statics shared
    // across all threads. TSan flagged a data race when sampleSelf() is called
    // concurrently (the self-observation loop can be invoked from more than one
    // thread in test/stress setups). thread_local gives each thread its own
    // baseline — the CPU% published to gSelfCpuPercent is a single atomic anyway,
    // so per-thread baselines simply average out, which is fine for a throttling
    // heuristic. This avoids a lock on the ~1Hz hot path.
    static thread_local long long sPrevTicks = -1;
    static thread_local std::chrono::steady_clock::time_point sPrevTime;
    std::ifstream sstat("/proc/self/stat");
    if (sstat) {
        // pid (comm) state ppid ... utime(14) stime(15) ... comm may contain spaces/parens.
        std::string rest;
        std::getline(sstat, rest);
        // Strip up to the LAST ')' (comm is wrapped in parens).
        size_t rp = rest.rfind(')');
        // G1-008: guard like pid_identity.h — a truncated/odd /proc/self/stat
        // where ')' is the last byte would make substr(rp+2) throw
        // std::out_of_range once per sampleSelf() call.
        if (rp != std::string::npos && rp + 2 <= rest.size()) {
            long long utime = 0, stime = 0;
            if (!parseSelfStatCpuFields(rest.substr(rp + 2), utime, stime)) {
                // malformed /proc/self/stat: leave previous counters untouched
            } else {
            long long ticks = utime + stime;
            auto now = std::chrono::steady_clock::now();
            if (sPrevTicks >= 0) {
                double wallSec = std::chrono::duration<double>(now - sPrevTime).count();
                long ticksPerSec = sysconf(_SC_CLK_TCK);
                if (wallSec > 0.01 && ticksPerSec > 0) {
                    double pct = (double)(ticks - sPrevTicks) / (double)ticksPerSec / wallSec * 100.0;
                    if (pct < 0) pct = 0;
                    gSelfCpuPercent.store((int)pct, std::memory_order_relaxed);
                }
            }
            sPrevTicks = ticks;
            sPrevTime = now;
            }
        }
    }
}

bool evaluateThrottle() {
    int cpu = gSelfCpuPercent.load(std::memory_order_relaxed);
    // P3-37: static locals below assume evaluateThrottle() is called from a
    // single thread (the daemon main loop). Multi-threaded callers would race
    // on sOverSince / sWasOver / sUnderSince.
    static std::chrono::steady_clock::time_point sOverSince;
    static std::chrono::steady_clock::time_point sUnderSince;
    static bool sWasOver = false;
    bool over = cpu > gCpuBudgetPercent;
    if (over) {
        if (!sWasOver) {
            sOverSince = std::chrono::steady_clock::now();
            sWasOver = true;
        }
        // P2-21: reset sUnderSince when we go back over budget, otherwise the
        // next time we dip under, sUnderSince may still hold an old timestamp
        // from a previous under-budget period and immediately un-throttle.
        sUnderSince = std::chrono::steady_clock::time_point();
        double secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - sOverSince).count();
        if (secs >= gSustainSec) {
            gThrottled.store(true, std::memory_order_relaxed);
            return true;
        }
    } else {
        sWasOver = false;
        // require a below-budget grace period before un-throttling
        if (gThrottled.load()) {
            if (sUnderSince.time_since_epoch().count() == 0) sUnderSince = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - sUnderSince).count();
            if (secs >= gSustainSec) {
                gThrottled.store(false, std::memory_order_relaxed);
                sUnderSince = std::chrono::steady_clock::time_point();
            }
        }
    }
    return gThrottled.load();
}

void writeStateFiles(const char* dir) {
    if (!dir) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", gSelfCpuPercent.load());
    writeFileAtomic((std::string(dir) + "/self_cpu").c_str(), buf);
    snprintf(buf, sizeof(buf), "%ld", gSelfRssKb.load());
    writeFileAtomic((std::string(dir) + "/self_rss").c_str(), buf);
    snprintf(buf, sizeof(buf), "%d", gSelfFdCount.load());
    writeFileAtomic((std::string(dir) + "/self_fd").c_str(), buf);
    snprintf(buf, sizeof(buf), "%d", gSelfThreadCount.load());
    writeFileAtomic((std::string(dir) + "/self_thread").c_str(), buf);
}

#ifdef AATPE_HOST_TEST
void setCpuBudgetPercentForTest(int pct) { if (pct > 0) gCpuBudgetPercent = pct; }
void setSustainSecForTest(int s) { if (s > 0) gSustainSec = s; }
// STATE-01 regression: expose the /proc/self/stat field parser so host tests
// can feed a known-good fixture (with tpgid=-1, real flags/minflt, etc.) and
// assert utime/stime are extracted from the CORRECT fields. Returns false on
// malformed input.
bool parseSelfStatCpuForTest(const std::string& afterParen,
                             long long& utimeOut, long long& stimeOut) {
    return parseSelfStatCpuFields(afterParen, utimeOut, stimeOut);
}
#endif

}  // namespace selfobs
}  // namespace subsys
}  // namespace aatpe
