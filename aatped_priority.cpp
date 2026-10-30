
#include "aatped.h"
#include "aatped_priority.h"
#include "aatped_logging.h"
#include "aatpe_recovery.h"
#include "aatpe_subsystem.h"
#include "aatpe_sched_abi.h"
#include "aatpe_placement_mech.h"

#include <cerrno>
#include <cstring>
#include <cstdint>

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sched.h>

#include <fstream>
#include <sstream>
#include <set>
#include <chrono>
#include <mutex>

using aatpe_sched_attr = struct sched_attr;

extern std::atomic<float> g_thermalPlacementScale;
extern std::atomic<bool> thermalSmoothEnabled;

#ifndef __NR_sched_setattr
#if defined(__x86_64__)
#define __NR_sched_setattr 314
#elif defined(__aarch64__)
#define __NR_sched_setattr 274
#else
#define __NR_sched_setattr 314
#endif
#endif

#ifndef SCHED_FIFO
#define SCHED_FIFO 1
#endif
#ifndef SCHED_RR
#define SCHED_RR 2
#endif
#ifndef SCHED_OTHER
#define SCHED_OTHER 0
#endif

namespace {

std::atomic<int> s_uclampSupport{0};
std::atomic<long long> s_lastUnsupportedMs{0};
constexpr long long kUclampReprobeMs = 60000LL;
static long long uclampNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static bool dueForUclampReprobe() {
    int v = s_uclampSupport.load(std::memory_order_relaxed);
    if (v >= 0) return false;
    long long last = s_lastUnsupportedMs.load(std::memory_order_relaxed);
    if (last == 0) { s_lastUnsupportedMs.store(uclampNowMs(), std::memory_order_relaxed); return false; }
    return (uclampNowMs() - last) >= kUclampReprobeMs;
}

}

namespace aatpe_priority {

bool uclampAvailable() {
    return s_uclampSupport.load(std::memory_order_relaxed) == 1;
}

int uclampSupportState() {
    return s_uclampSupport.load(std::memory_order_relaxed);
}

void persistUclampSupport(bool supported) {
    std::string path = std::string(STATE_DIR) + "/uclamp_supported";
    writeFile(path.c_str(), supported ? 1 : 0);
}

}

namespace aatpe_priority {

namespace {

int roleToNice(ThreadRole role) {
    switch (role) {
        case ROLE_S1_UI:     return -8;
        case ROLE_S2_RENDER: return -10;
        case ROLE_S3_GAME:   return -12;
        default:             return 0;
    }
}

struct RtParams {
    int policy;
    int priority;
};
RtParams levelToRt(ThreadRole role, int level) {
    int basePrio = 1;
    if (role == ROLE_S2_RENDER || role == ROLE_S3_GAME) basePrio = 2;
    if (level >= 3) {
        return {SCHED_FIFO, basePrio + 1};
    } else if (level == 2) {
        return {SCHED_FIFO, basePrio};
    }
    return {SCHED_RR, basePrio};
}

uint32_t roleToUclampMin(ThreadRole role) {
    uint32_t base = 0u;
    if (role == ROLE_S2_RENDER || role == ROLE_S3_GAME) base = 1024u;
    else if (role == ROLE_S1_UI) base = 512u;
    if (base == 0u) return 0u;
    if (thermalSmoothEnabled.load(std::memory_order_relaxed)) {
        float scale = g_thermalPlacementScale.load(std::memory_order_relaxed);
        if (scale < 1.0f) {
            base = (uint32_t)((float)base * scale);
        }
    }
    return base;
}

static bool uclampRewriteSkippable(uint32_t curMin, uint32_t targetMin,
                                   uint32_t lastPriorityMin) {
    if (targetMin == 0u) return true;
    bool kernelHighEnough = (curMin >= targetMin);
    bool iMustLower = (targetMin < lastPriorityMin);
    if (lastPriorityMin == 0u && curMin > targetMin) return false;
    return kernelHighEnough && !iMustLower;
}

#ifdef AATPE_HOST_TEST
extern "C" int aatpeRtUclampRewriteSkippableForTest(unsigned int curMin,
                                                    unsigned int targetMin,
                                                    unsigned int lastPriorityMin) {
    return uclampRewriteSkippable(curMin, targetMin, lastPriorityMin) ? 1 : 0;
}
#endif

int schedSetAttr(int tid, aatpe_sched_attr* attr, unsigned long flags) {
    return static_cast<int>(syscall(__NR_sched_setattr, tid, attr, flags));
}

#ifndef __NR_sched_getattr
#if defined(__aarch64__)
#define __NR_sched_getattr 276
#else
#define __NR_sched_getattr 315
#endif
#endif

bool schedGetAttr(int tid, aatpe_sched_attr* out) {
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    return syscall(__NR_sched_getattr, tid, out, sizeof(*out), 0) == 0;
}

static constexpr int kRtMaxBoosted = 64;
std::mutex           g_rtMutex;
std::set<int>        g_rtBoostedTids;
static std::unordered_map<int, unsigned long long> g_rtStarttimes;

#ifdef AATPE_HOST_TEST
void aatpeRtDebugSeedImpl(int tid, unsigned long long starttime) {
    g_rtBoostedTids.insert(tid);
    g_rtStarttimes[tid] = starttime;
}
#endif
static bool readThreadStarttime(int tid, unsigned long long* out);

#ifdef AATPE_HOST_TEST
using RtAttrOkFn = bool(*)(int tid, void* attrOut);
using RtStartFn  = bool(*)(int tid, unsigned long long* out);
static RtAttrOkFn g_testAttrFn = nullptr;
static RtStartFn  g_testStartFn = nullptr;
extern "C" void aatpeRtSetHooksForTest(RtAttrOkFn a, RtStartFn s) { g_testAttrFn=a; g_testStartFn=s; }
extern "C" int  aatpeRtCountForTest() { return (int)g_rtBoostedTids.size(); }
extern "C" int  aatpeRtContainsForTest(int tid) { return g_rtBoostedTids.count(tid); }
#endif

static void pruneDeadRtTids() {
    std::set<int> snapTids;
    std::unordered_map<int, unsigned long long> snapStart;
    {
        std::lock_guard<std::mutex> lk(g_rtMutex);
        snapTids = g_rtBoostedTids;
        snapStart = g_rtStarttimes;
    }

    std::set<int> deadTids;
    std::unordered_map<int, unsigned long long> liveStart;
    for (int tid : snapTids) {
        aatpe_sched_attr a{};
#ifdef AATPE_HOST_TEST
        bool alive = g_testAttrFn ? g_testAttrFn(tid, &a) : schedGetAttr(tid, &a);
#else
        bool alive = schedGetAttr(tid, &a);
#endif
        if (!alive) { deadTids.insert(tid); continue; }
        unsigned long long st = 0;
#ifdef AATPE_HOST_TEST
        bool gotSt = g_testStartFn ? g_testStartFn(tid, &st) : readThreadStarttime(tid, &st);
#else
        bool gotSt = readThreadStarttime(tid, &st);
#endif
        if (!gotSt) { deadTids.insert(tid); continue; }
        auto it = snapStart.find(tid);
        if (it != snapStart.end() && it->second != st) { deadTids.insert(tid); continue; }
        liveStart[tid] = st;
    }

    {
        std::lock_guard<std::mutex> lk(g_rtMutex);
        for (int tid : deadTids) {
            g_rtBoostedTids.erase(tid);
            g_rtStarttimes.erase(tid);
        }
        for (auto& kv : liveStart) {
            g_rtStarttimes[kv.first] = kv.second;
        }
    }
}
extern "C" void aatpePruneDeadRtTids() {
    pruneDeadRtTids();
}

bool readThreadStarttime(int tid, unsigned long long* out) {
    std::string path = "/proc/" + std::to_string(tid) + "/stat";
    std::ifstream f(path.c_str());
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    size_t rp = line.rfind(") ");
    if (rp == std::string::npos || rp + 1 >= line.size()) return false;
    std::istringstream ss(line.substr(rp + 2));
    char state;
    if (!(ss >> state)) return false;
    unsigned long long v;
    for (int i = 1; i <= 19; ++i) {
        if (!(ss >> v)) return false;
        if (i == 19) { *out = v; return true; }
    }
    return false;
}

}

bool aatpeSetThreadPriority(int tid, ThreadRole role, int level) {
    if (!aatpe::subsys::safemode::rtBoostAllowed()) {
        return false;
    }
    if (role != ROLE_S1_UI && role != ROLE_S2_RENDER && role != ROLE_S3_GAME) {
        return false;
    }
    if (tid <= 0) return false;
    if (level < 1) level = 1;
    if (level > 3) level = 3;

    RtParams rt = levelToRt(role, level);

    aatpe_recovery::RecoveryRecord existingRec;
    bool schedAlreadyRecorded =
        aatpe_recovery::recoveryGet(tid, existingRec) && existingRec.haveOrigSched;

    aatpe_sched_attr origAttr{};
    bool origValid = false;
    unsigned long long starttime = 0;
    bool starttimeValid = false;
    if (!schedAlreadyRecorded) {
        origValid = schedGetAttr(tid, &origAttr);
        starttimeValid = readThreadStarttime(tid, &starttime);
    }

    {
        aatpe_sched_attr cur{};
        if (schedGetAttr(tid, &cur)) {
            uint32_t targetUclampMin = roleToUclampMin(role);
            uint32_t lastPriorityMin =
                aatpe_mech::aatpeUclampFloorGet(tid, aatpe_mech::AATPE_UCLAMP_PRIORITY);
            bool uclampSatisfied =
                uclampRewriteSkippable(cur.sched_util_min, targetUclampMin, lastPriorityMin);
            if (cur.sched_policy == (uint32_t)rt.policy &&
                cur.sched_priority == (uint32_t)rt.priority &&
                uclampSatisfied) {
                pruneDeadRtTids();
                std::lock_guard<std::mutex> rtk(g_rtMutex);
                g_rtBoostedTids.insert(tid);
                { unsigned long long st=0; if (readThreadStarttime(tid,&st)) g_rtStarttimes[tid]=st; }
                return true;
            }
        }
    }

    bool rtBudgetFree = true;
    bool weTookBudget = false;
    {
        std::unique_lock<std::mutex> rtk(g_rtMutex);
        if (g_rtBoostedTids.count(tid) == 0 &&
            (int)g_rtBoostedTids.size() >= kRtMaxBoosted) {
            rtk.unlock();
            pruneDeadRtTids();
            rtk.lock();
        }
        if (g_rtBoostedTids.count(tid) == 0) {
            if ((int)g_rtBoostedTids.size() >= kRtMaxBoosted) {
                rtBudgetFree = false;
                LOG_WARN("aatpeSetThreadPriority: tid=%d RT budget exhausted (%d cap), degrading to nice",
                         tid, kRtMaxBoosted);
            } else {
                g_rtBoostedTids.insert(tid);
                { unsigned long long st=0; if (readThreadStarttime(tid,&st)) g_rtStarttimes[tid]=st; }
                weTookBudget = true;
            }
        }
    }

    auto releaseBudgetIfWeTook = [&]() {
        if (weTookBudget) {
            std::lock_guard<std::mutex> rtk(g_rtMutex);
            g_rtBoostedTids.erase(tid);
            g_rtStarttimes.erase(tid);
            weTookBudget = false;
        }
    };

    aatpe_sched_attr attr{};
    attr.size          = sizeof(attr);
    attr.sched_policy  = static_cast<uint32_t>(rt.policy);
    attr.sched_flags   = 0;
    attr.sched_priority = static_cast<uint32_t>(rt.priority);

    int rc;
    if (!rtBudgetFree) {
        rc = -1; errno = EBUSY;
    } else {
        rc = schedSetAttr(tid, &attr, 0);
    }
    bool rtOk = (rc == 0);
    if (!rtOk) {
        int e = errno;
        if (e == EPERM || e == ENOSYS || e == EBUSY) {
            int niceVal = roleToNice(role);
            if (niceVal != 0 && setpriority(PRIO_PROCESS, tid, niceVal) == 0) {
                if (!schedAlreadyRecorded && origValid && starttimeValid) {
                    aatpe_recovery::recoveryRecordSched(
                        0, tid, starttime,
                        origAttr.sched_policy, origAttr.sched_priority, (int)origAttr.sched_nice,
                        origAttr.sched_util_min, origAttr.sched_util_max,
                        origAttr.sched_policy, origAttr.sched_priority, niceVal,
                        origAttr.sched_util_min, origAttr.sched_util_max);
                } else if (schedAlreadyRecorded) {
                    aatpe_recovery::recoveryRefreshAppliedSched(
                        tid, origAttr.sched_policy, origAttr.sched_priority,
                        niceVal,
                        origAttr.sched_util_min, origAttr.sched_util_max);
                }
                LOG_WARN("aatpeSetThreadPriority: tid=%d role=%d RT denied (errno=%d=%s), "
                         "degraded to nice=%d",
                         tid, (int)role, e, aatpeErrStr(e), niceVal);
                releaseBudgetIfWeTook();
                return true;
            }
            LOG_WARN("aatpeSetThreadPriority: tid=%d role=%d RT setattr failed errno=%d=%s "
                     "and nice degrade failed errno=%d",
                     tid, (int)role, e, aatpeErrStr(e), errno);
            releaseBudgetIfWeTook();
            return false;
        }
        LOG_WARN("aatpeSetThreadPriority: tid=%d role=%d sched_setattr(policy) failed errno=%d=%s",
                 tid, (int)role, e, aatpeErrStr(e));
        releaseBudgetIfWeTook();
        return false;
    }

    bool uclampApplied = false;
    uint32_t uclampMin = roleToUclampMin(role);
    if (s_uclampSupport.load(std::memory_order_relaxed) < 0 && dueForUclampReprobe()) {
        LOG_INFO("aatpeSetThreadPriority: uclamp re-probe after %lldms (was latched unsupported)",
                 (long long)kUclampReprobeMs);
        s_uclampSupport.store(0, std::memory_order_relaxed);
    }
    if (s_uclampSupport.load(std::memory_order_relaxed) >= 0 && uclampMin > 0) {
        aatpe_sched_attr clamp{};
        clamp.size        = sizeof(clamp);
        clamp.sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS |
                            SCHED_FLAG_UTIL_CLAMP_MIN | SCHED_FLAG_UTIL_CLAMP_MAX;
        clamp.sched_util_min = uclampMin;
        clamp.sched_util_max = 1024;
        int rc2 = schedSetAttr(tid, &clamp, 0);
        if (rc2 != 0) {
            int e2 = errno;
            bool firstProbe = (s_uclampSupport.load(std::memory_order_relaxed) == 0);
            if (e2 == E2BIG || e2 == ENOSYS || (e2 == EINVAL && firstProbe)) {
                s_uclampSupport.store(-1, std::memory_order_relaxed);
                s_lastUnsupportedMs.store(uclampNowMs(), std::memory_order_relaxed);
                persistUclampSupport(false);
                LOG_INFO("aatpeSetThreadPriority: uclamp not supported by kernel "
                         "(errno=%d=%s), skipping uclamp for all threads (RT policy kept)",
                         e2, aatpeErrStr(e2));
            } else {
                LOG_WARN("aatpeSetThreadPriority: tid=%d role=%d uclamp setattr failed errno=%d=%s "
                         "(RT policy kept)",
                         tid, (int)role, e2, aatpeErrStr(e2));
            }
        } else {
            aatpe_sched_attr verifyAttr{};
            bool policySurvived = false;
            bool readbackOk = schedGetAttr(tid, &verifyAttr);
            if (readbackOk) {
                policySurvived = (verifyAttr.sched_policy == (uint32_t)rt.policy);
            } else {
                LOG_INFO("aatpeSetThreadPriority: tid=%d sched_getattr readback "
                         "failed errno=%d (thread may have exited, skipping)",
                         tid, errno);
                return false;
            }
            if (!policySurvived) {
                aatpe_sched_attr repair{};
                repair.size        = sizeof(repair);
                repair.sched_policy = static_cast<uint32_t>(rt.policy);
                repair.sched_flags  = 0;
                repair.sched_priority = static_cast<uint32_t>(rt.priority);
                int rcRepair = schedSetAttr(tid, &repair, 0);
                if (rcRepair != 0) {
                    LOG_WARN("aatpeSetThreadPriority: tid=%d RT repair sched_setattr "
                             "failed errno=%d=%s (RT may be lost)",
                             tid, errno, aatpeErrStr(errno));
                }
                s_uclampSupport.store(-2, std::memory_order_relaxed);
                s_lastUnsupportedMs.store(uclampNowMs(), std::memory_order_relaxed);
                persistUclampSupport(false);
                LOG_WARN("aatpeSetThreadPriority: tid=%d uclamp step2 returned 0 but "
                         "policy reverted to %u (expected %d) — KEEP_POLICY not "
                         "backported, latching partial=-2, RT repaired",
                         tid, verifyAttr.sched_policy, rt.policy);
            } else {
                bool utilClampSticky =
                    (verifyAttr.sched_util_min >= (uclampMin / 2)) &&
                    (verifyAttr.sched_util_max > 0);
                if (!utilClampSticky) {
                    s_uclampSupport.store(-3, std::memory_order_relaxed);
                    s_lastUnsupportedMs.store(uclampNowMs(), std::memory_order_relaxed);
                    persistUclampSupport(false);
                    LOG_WARN("aatpeSetThreadPriority: tid=%d uclamp step2 returned 0, "
                             "policy kept but util_min=%u (requested %u) ignored — "
                             "latching policy-only=-3, fall back to stune/affinity",
                             tid, verifyAttr.sched_util_min, uclampMin);
                } else {
                    uclampApplied = true;
                    if (s_uclampSupport.load(std::memory_order_relaxed) == 0) {
                        s_uclampSupport.store(1, std::memory_order_relaxed);
                        persistUclampSupport(true);
                    }
                    aatpe_mech::aatpeUclampFloorSet(tid, aatpe_mech::AATPE_UCLAMP_PRIORITY,
                                                    uclampMin);
                }
            }
        }
    }

    if (!schedAlreadyRecorded && origValid && starttimeValid) {
        aatpe_recovery::recoveryRecordSched(
            0, tid, starttime,
            origAttr.sched_policy, origAttr.sched_priority, (int)origAttr.sched_nice,
            origAttr.sched_util_min, origAttr.sched_util_max,
            static_cast<uint32_t>(rt.policy), static_cast<uint32_t>(rt.priority),
            (int)origAttr.sched_nice,
            uclampApplied ? uclampMin : origAttr.sched_util_min,
            uclampApplied ? 1024u      : origAttr.sched_util_max);
    } else if (schedAlreadyRecorded) {
        aatpe_recovery::recoveryRefreshAppliedSched(
            tid,
            static_cast<uint32_t>(rt.policy), static_cast<uint32_t>(rt.priority),
            (int)origAttr.sched_nice,
            uclampApplied ? uclampMin : origAttr.sched_util_min,
            uclampApplied ? 1024u      : origAttr.sched_util_max);
    }
    return true;
}

#ifdef AATPE_HOST_TEST
struct RestoreCapture {
    int tid = -1;
    int policy = -999;
    int prio = -999;
    int nice = 999;
    uint32_t uclampMin = 0xFFFFFFFFu;
    uint32_t uclampMax = 0xFFFFFFFFu;
    bool invoked = false;
};
static RestoreCapture s_restoreCapture;
static bool g_restoreCaptureOn = false;
extern "C" void aatpeRtRestoreCaptureResetForTest() { s_restoreCapture = RestoreCapture{}; }
extern "C" RestoreCapture aatpeRtRestoreCaptureForTest() { return s_restoreCapture; }
extern "C" void aatpeRtRestoreCaptureEnableForTest(bool on) { g_restoreCaptureOn = on; }
#endif

namespace {
void applyRestoreSched(int tid, uint32_t policy, uint32_t prio, int nice) {
#ifdef AATPE_HOST_TEST
    if (g_restoreCaptureOn) {
        s_restoreCapture.invoked = true;
        s_restoreCapture.tid    = tid;
        s_restoreCapture.policy  = (int)policy;
        s_restoreCapture.prio    = (int)prio;
        s_restoreCapture.nice    = nice;
        return;
    }
#endif
    struct sched_param sp{};
    sp.sched_priority = (int)prio;
    if (sched_setscheduler(tid, (int)policy, &sp) != 0) {
        if (errno != ESRCH)
            LOG_WARN("aatpeRestoreThreadPriority: tid=%d sched_setscheduler(%u) failed errno=%d=%s",
                     tid, policy, errno, aatpeErrStr(errno));
    }
    if (setpriority(PRIO_PROCESS, tid, nice) != 0) {
    }
}
void applyRestoreUclamp(int tid, uint32_t umin, uint32_t umax) {
#ifdef AATPE_HOST_TEST
    if (g_restoreCaptureOn) {
        s_restoreCapture.uclampMin = umin;
        s_restoreCapture.uclampMax = umax;
        return;
    }
#endif
    aatpe_sched_attr attr{};
    attr.size        = sizeof(attr);
    attr.sched_flags  = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS |
                        SCHED_FLAG_UTIL_CLAMP_MIN | SCHED_FLAG_UTIL_CLAMP_MAX;
    attr.sched_util_min = umin;
    attr.sched_util_max = umax;
    if (schedSetAttr(tid, &attr, 0) != 0) {  }
}
}

bool aatpeRestoreThreadPriority(int tid) {
    if (tid <= 0) return false;

    aatpe_recovery::RecoveryRecord rec;
    bool haveRec = aatpe_recovery::recoveryGet(tid, rec);

    uint32_t targetPolicy = (haveRec && rec.haveOrigSched) ? rec.origPolicy
                                                           : (uint32_t)SCHED_OTHER;
    uint32_t targetPrio   = (haveRec && rec.haveOrigSched) ? rec.origPriority : 0u;
    int      targetNice    = haveRec ? rec.origNice : 0;
    uint32_t targetUMin   = (haveRec && rec.haveOrigUclamp) ? rec.origUclampMin : 0u;
    uint32_t targetUMax   = (haveRec && rec.haveOrigUclamp) ? rec.origUclampMax : 1024u;

    applyRestoreSched(tid, targetPolicy, targetPrio, targetNice);
    applyRestoreUclamp(tid, targetUMin, targetUMax);
    aatpe_mech::aatpeUclampFloorClear(tid, aatpe_mech::AATPE_UCLAMP_PRIORITY);

    {
        std::lock_guard<std::mutex> lk(g_rtMutex);
        g_rtBoostedTids.erase(tid);
        g_rtStarttimes.erase(tid);
    }
    if (haveRec) aatpe_recovery::recoveryErase(tid);

    LOG_INFO("aatpeRestoreThreadPriority: tid=%d restored to policy=%u nice=%d "
             "(record=%d)", tid, targetPolicy, targetNice, (int)haveRec);
    return true;
}

}

#ifdef AATPE_HOST_TEST
extern "C" void aatpeRtDebugSeedForTest(int tid, unsigned long long starttime) {
    aatpe_priority::aatpeRtDebugSeedImpl(tid, starttime);
}
#endif
