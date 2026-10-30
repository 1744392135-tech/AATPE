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

void restoreUserControlMode();

namespace aatpe {
namespace subsys {

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
    SubsystemState desired = SubsystemState::DEGRADED;
    SubsystemState cur = state.load(std::memory_order_relaxed);
    while (cur == SubsystemState::ACTIVE || cur == SubsystemState::STARTING ||
           cur == SubsystemState::INIT) {
        if (state.compare_exchange_weak(cur, desired,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            break;
        }
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

static bool writeFileAtomic(const char* path, const std::string& data) {
    std::string tmpPath = std::string(path) + ".tmp.XXXXXX";
    int fd = ::mkstemp(&tmpPath[0]);
    if (fd < 0) return false;
    size_t off = 0;
    bool ok = true;
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
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        if (::unlink(tmpPath.c_str()) != 0) {}
        return false;
    }
    if (::rename(tmpPath.c_str(), path) != 0) {
        if (::unlink(tmpPath.c_str()) != 0) {}
        return false;
    }
    {
        std::string pstr(path);
        size_t slash = pstr.rfind('/');
        std::string dir = ".";
        if (slash != std::string::npos) dir = pstr.substr(0, slash == 0 ? 1 : slash);
        int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            if (::fsync(dfd) != 0) {  }
            ::close(dfd);
        }
    }
    return true;
}

void Registry::writeStateFile(const char* dir) {
    if (!dir) return;
    std::string p = std::string(dir) + "/subsystem_health";
    if (!writeFileAtomic(p.c_str(), toJson())) {
    }
}

int Registry::failedCount() {
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
    bool was = mStop.exchange(true, std::memory_order_acq_rel);
    if (!was && mFd >= 0) {
        uint64_t one = 1;
        ssize_t w = ::write(mFd, &one, sizeof(one));
        (void)w;
    }
}

void StopToken::drain() {
    if (mFd < 0) return;
    uint64_t val;
    ::read(mFd, &val, sizeof(val));
}

void StopToken::reset() {
    mStop.store(false, std::memory_order_release);
    drain();
}

namespace safemode {

std::atomic<bool> gSafeModeActive{false};
std::atomic<int>  gSafeModeReason{0};
std::atomic<time_t> gSafeModeEntered{0};

static std::mutex gSafeModePairMutex;

static int gStreakThreshold = 3;
static int gRecoverCooldownSec = 60;

static std::atomic<int> gEbfpStreak{0};
static std::atomic<int> gSchedStreak{0};

static constexpr int kCrashLoopObserveSec = 300;
static constexpr int kCrashLoopMaxHeal = 2;
static std::atomic<time_t> gCrashLoopSince{0};
static std::atomic<int> gCrashLoopHealTries{0};
static std::atomic<bool> gCrashLoopEverLatched{false};
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
    if (r == EBPF_STREAK) {
        int n = gEbfpStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n < gStreakThreshold) return;
    } else if (r == SCHED_STREAK) {
        int n = gSchedStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n < gStreakThreshold) return;
    }
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

void resetStreak(SafeReason r) {
    if (r == EBPF_STREAK) gEbfpStreak.store(0, std::memory_order_relaxed);
    else if (r == SCHED_STREAK) gSchedStreak.store(0, std::memory_order_relaxed);
}

void maybeRecover(std::time_t now) {
    if (!gSafeModeActive.load(std::memory_order_relaxed)) return;
    int cur = gSafeModeReason.load(std::memory_order_relaxed);
    if ((cur & CRASH_LOOP) && gCrashLoopSince.load(std::memory_order_relaxed) != 0) {
        int observe =
#ifdef AATPE_HOST_TEST
            g_crashLoopObserveSecTestOverride > 0 ? g_crashLoopObserveSecTestOverride :
#endif
            kCrashLoopObserveSec;
        if (now - gCrashLoopSince.load(std::memory_order_relaxed) >= observe &&
            gCrashLoopHealTries.load(std::memory_order_relaxed) < kCrashLoopMaxHeal) {
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
                if (nb == 0) {
                    gSafeModeActive.store(false, std::memory_order_relaxed);
                    droppedLatch = true;
                }
            }
            restoreUserControlMode();
            (void)droppedLatch;
            return;
        }
    }
    time_t entered = gSafeModeEntered.load(std::memory_order_relaxed);
    if (now - entered < gRecoverCooldownSec) return;
    gEbfpStreak.store(0, std::memory_order_relaxed);
    gSchedStreak.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(gSafeModePairMutex);
        int softMask = EBPF_STREAK | SCHED_STREAK;
        int oldBits = gSafeModeReason.load(std::memory_order_relaxed);
        int newBits;
        do {
            newBits = oldBits & ~softMask;
        } while (!gSafeModeReason.compare_exchange_weak(oldBits, newBits,
                                                         std::memory_order_relaxed,
                                                         std::memory_order_relaxed));
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

void clearReason(SafeReason r) {
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

}

namespace selfobs {

std::atomic<int> gSelfCpuPercent{0};
std::atomic<long> gSelfRssKb{0};
std::atomic<int> gSelfFdCount{0};
std::atomic<int> gSelfThreadCount{0};
std::atomic<bool> gThrottled{false};

static int gCpuBudgetPercent = 50;
static int gSustainSec = 10;

static bool parseSelfStatCpuFields(const std::string& afterParen,
                                   long long& utimeOut, long long& stimeOut) {
    std::istringstream iss(afterParen);
    char stateChar = 0;
    if (!(iss >> stateChar)) return false;
    long skip = 0;
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
    std::ifstream sf("/proc/self/status");
    if (sf) {
        std::string line;
        while (std::getline(sf, line)) {
            if (line.compare(0, 5, "VmRSS") == 0) {
                size_t p = line.find_first_of("0123456789");
                if (p != std::string::npos) {
                    try { gSelfRssKb.store(std::stol(line.substr(p)), std::memory_order_relaxed); }
                    catch (const std::exception&) {  }
                }
            } else if (line.compare(0, 7, "Threads") == 0) {
                size_t p = line.find_first_of("0123456789");
                if (p != std::string::npos) {
                    try { gSelfThreadCount.store(std::stoi(line.substr(p)), std::memory_order_relaxed); }
                    catch (const std::exception&) {  }
                }
            }
        }
    }
    DIR* d = ::opendir("/proc/self/fd");
    if (d) {
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
    static thread_local long long sPrevTicks = -1;
    static thread_local std::chrono::steady_clock::time_point sPrevTime;
    std::ifstream sstat("/proc/self/stat");
    if (sstat) {
        std::string rest;
        std::getline(sstat, rest);
        size_t rp = rest.rfind(')');
        if (rp != std::string::npos && rp + 2 <= rest.size()) {
            long long utime = 0, stime = 0;
            if (!parseSelfStatCpuFields(rest.substr(rp + 2), utime, stime)) {
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
    static std::chrono::steady_clock::time_point sOverSince;
    static std::chrono::steady_clock::time_point sUnderSince;
    static bool sWasOver = false;
    bool over = cpu > gCpuBudgetPercent;
    if (over) {
        if (!sWasOver) {
            sOverSince = std::chrono::steady_clock::now();
            sWasOver = true;
        }
        sUnderSince = std::chrono::steady_clock::time_point();
        double secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - sOverSince).count();
        if (secs >= gSustainSec) {
            gThrottled.store(true, std::memory_order_relaxed);
            return true;
        }
    } else {
        sWasOver = false;
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
bool parseSelfStatCpuForTest(const std::string& afterParen,
                             long long& utimeOut, long long& stimeOut) {
    return parseSelfStatCpuFields(afterParen, utimeOut, stimeOut);
}
#endif

}
}
}
