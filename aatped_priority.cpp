// aatped_priority.cpp — 
//
// 通过 sched_setattr(2) 给关键线程设置实时调度类 + uclamp 算力墙。
// 设计为 best-effort：任何系统调用失败只记 LOG_WARN，不崩溃。
//
// 两步调用模型（避免 uclamp 设置把 RT policy 冲掉）：
//   step1: sched_setattr(tid, {.sched_policy=SCHED_FIFO, .sched_priority=N}, 0)
//          —— 切换调度类到实时
//   step2: sched_setattr(tid, {.sched_util_min=50, .sched_util_max=1024},
//                              SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS |
//                              SCHED_FLAG_UTIL_CLAMP_MIN | SCHED_FLAG_UTIL_CLAMP_MAX)
//          —— KEEP flags 告诉内核：本次调用不要改动 policy / params，只更新 uclamp
//
// 权限不足（EPERM）时降级：sched_setaffinity 已在 placement.cpp 成功，
// 说明我们至少有 cap_setuid 级别的权限；RT 需要 CAP_SYS_NICE，没拿到就
// 退回到 setpriority(PRIO_PROCESS, tid, -nice) 做静态优先级提升。

#include "aatped.h"   // 先引入完整 daemon 头（aatpe_ml::ThreadFeatures 等前向声明）
#include "aatped_priority.h"
#include "aatped_logging.h"
#include "aatpe_recovery.h"   // unified recovery record
#include "aatpe_subsystem.h"  // safe-mode gate on RT boost
#include "aatpe_sched_abi.h"  // ROUND-W3: single source of truth for SCHED_FLAG_* + sched_attr layout
#include "aatpe_placement_mech.h"  // audit4 F4: register RT floor in the shared uclamp ledger

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

// ---------------------------------------------------------------------------
// B-005: sched_attr — SINGLE SOURCE OF TRUTH. The layout (size/policy/flags/nice/
// priority/runtime/deadline/period/util_min/util_max) now comes exclusively from
// aatpe_sched_abi.h (which provides the kernel-UAPI struct on Android and an
// exact 56-byte fallback on host). The old hand-copied `struct aatpe_sched_attr`
// here drifted on field order vs the ABI; alias it to the canonical definition so
// the rest of this TU keeps its `aatpe_sched_attr` spelling while there is only one
// layout to maintain.
// ---------------------------------------------------------------------------
using aatpe_sched_attr = struct sched_attr;

// C3 exploration: extern references to daemon globals (defined in aatped_main.cpp).
extern std::atomic<float> g_thermalPlacementScale;
extern std::atomic<bool> thermalSmoothEnabled;

// ROUND-W3: SCHED_FLAG_* constants removed — they now come exclusively from
// aatpe_sched_abi.h (included above), which uses the correct UAPI values
// (KEEP_POLICY=0x08, KEEP_PARAMS=0x10, UTIL_CLAMP_MIN=0x20, UTIL_CLAMP_MAX=0x40).
// The previous local fallback block (0x04/0x08/0x10/0x20) was shifted one bit low
// and, when NDK headers did not yet expose the flags, caused step2 of the two-step
// sched_setattr to carry wrong flag bits on 4.14 vendor kernels: the syscall
// returned 0 but the kernel silently ignored KEEP_POLICY/UTIL_CLAMP, so uclamp
// was never actually applied while step1's RT policy was at risk of being reset.

// __NR_sched_setattr 兜底（x86_64 host = 314, arm64 = 274；<sys/syscall.h> 已定义）。
#ifndef __NR_sched_setattr
#if defined(__x86_64__)
#define __NR_sched_setattr 314
#elif defined(__aarch64__)
#define __NR_sched_setattr 274
#else
#define __NR_sched_setattr 314  // host fallback
#endif
#endif

// SCHED_FIFO / SCHED_RR 由 <sched.h> 提供；兜底。
#ifndef SCHED_FIFO
#define SCHED_FIFO 1
#endif
#ifndef SCHED_RR
#define SCHED_RR 2
#endif
// SCHED_OTHER = 0 是普通 CFS 调度类；恢复 RT 时回落到这里。<sched.h> 已提供，
// 但 host glibc / 旧 bionic 可能未暴露，手动兜底以保证 -fsyntax-only 通过。
#ifndef SCHED_OTHER
#define SCHED_OTHER 0
#endif

// ---------------------------------------------------------------------------
// uclamp kernel-support one-time probe.
//
// On kernel 4.14 (MT6893 / Dimensity 1100), SCHED_FLAG_UTIL_CLAMP_MIN/MAX
// (Linux 5.0+) is not recognized. sched_setattr() returns -1 with errno=E2BIG
// (7) every single call. Before this produced a LOG_WARN per thread
// per migration — dozens of log writes per second, which by itself was a
// measurable CPU consumer (file I/O).
//
// Fix: probe once on the first step2 attempt. If the kernel rejects the
// uclamp flags with E2BIG or ENOSYS, latch s_uclampSupport = false and
// skip step2 silently for all future calls. RT policy (step1) is unaffected.
// The verdict is also written to /dev/aatpe/state/uclamp_supported for
// on-device diagnosis.
// ---------------------------------------------------------------------------
namespace {

// -1 = kernel does NOT support uclamp (probe done, skip step2 forever)
//  0 = not yet probed (first step2 call will run the probe)
//  1 = kernel supports uclamp (probe done, step2 is normal)
std::atomic<int> s_uclampSupport{0};
// B3 task6/M4: do NOT latch-forever. Some vendor kernels enable uclamp only
// after a late init stage, or the first probe hit a transient. Re-probe on a
// 60s throttle so a kernel that later gains uclamp is picked up (same pattern
// as the gpu_boost re-probe state machine).
std::atomic<long long> s_lastUnsupportedMs{0};
constexpr long long kUclampReprobeMs = 60000LL;
static long long uclampNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static bool dueForUclampReprobe() {
    int v = s_uclampSupport.load(std::memory_order_relaxed);
    if (v >= 0) return false;                 // not latched-unsupported
    long long last = s_lastUnsupportedMs.load(std::memory_order_relaxed);
    if (last == 0) { s_lastUnsupportedMs.store(uclampNowMs(), std::memory_order_relaxed); return false; }
    return (uclampNowMs() - last) >= kUclampReprobeMs;
}

}  // end anonymous namespace

namespace aatpe_priority {

bool uclampAvailable() {
    return s_uclampSupport.load(std::memory_order_relaxed) == 1;
}

int uclampSupportState() {
    return s_uclampSupport.load(std::memory_order_relaxed);
}

void persistUclampSupport(bool supported) {
    // writeFile() declared in aatped_utils.h, STATE_DIR declared in aatped.h
    // (both pulled in via #include "aatped.h"). STATE_DIR = "/dev/aatpe/state".
    std::string path = std::string(STATE_DIR) + "/uclamp_supported";
    writeFile(path.c_str(), supported ? 1 : 0);
}

}  // namespace

namespace aatpe_priority {

namespace {

// 角色 -> 基础 nice 降级值（EPERM 时使用）。
// 越关键的线程降得越多（nice 越负优先级越高），但不超过 -15 避免饿死系统。
int roleToNice(ThreadRole role) {
    switch (role) {
        case ROLE_S1_UI:     return -8;
        case ROLE_S2_RENDER: return -10;
        case ROLE_S3_GAME:   return -12;
        default:             return 0;
    }
}

// level(1/2/3) + role -> 实时调度类与静态优先级。
// 优先级故意压在 1~3 区间，避免 UI/RENDER/GAME 饿死内核线程与其他关键系统线程。
//   level 1: SCHED_RR, prio=1（轻度，轮转实时，最不易饿死系统）
//   level 2: SCHED_FIFO, prio=2（中度）
//   level 3: SCHED_FIFO, prio=3（激进，仍远低于 ksoftirqd/migration 等内核线程）
struct RtParams {
    int policy;
    int priority;
};
RtParams levelToRt(ThreadRole role, int level) {
    // UI 比 RENDER/GAME 略低一档（UI 线程短，过高 RT 反而放大抖动）。
    int basePrio = 1;
    if (role == ROLE_S2_RENDER || role == ROLE_S3_GAME) basePrio = 2;
    if (level >= 3) {
        return {SCHED_FIFO, basePrio + 1};
    } else if (level == 2) {
        return {SCHED_FIFO, basePrio};
    }
    return {SCHED_RR, basePrio};  // level 1（轻度）
}

// P1-5: role -> uclamp_min (0..1024).
//   RENDER/GAME: 1024 = clamp min to max util => force highest OPP for the
//   frame-critical path (replaces unreliable pure setaffinity against MTK
//   hmp_fork_balance / EAS find_best_target).
//   UI: 512 = soft hint (~50% util floor), not a hard highest-freq demand.
//   uclamp_max stays 1024 (no upper cap) for all.
// C3 exploration: when thermalSmoothEnabled, scale uclamp_min by
// g_thermalPlacementScale [0.4,1.0]. Hard wall (EMERGENCY) always wins; this
// layer can only tighten (lower the floor), never loosen.
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

// v4.4.8 MAJOR-2: pure idempotency decision for the RT uclamp_min write.
// Returns true when a re-write of the floor can be SKIPPED.
//   curMin          = kernel's current sched_util_min (read back via sched_getattr)
//   targetMin       = roleToUclampMin(role) — the floor this source wants now
//   lastPriorityMin = the floor THIS priority source last registered in the
//                     per-tid ledger (0 if it has never boosted this tid)
// Steady-state: if the kernel floor is already >= target, no raise is needed.
// BUT if priority's OWN desired floor dropped below what it last wrote (thermal
// soft-derate: g_thermalPlacementScale fell <1.0), the kernel currently reads
// >= the new (lower) target yet we MUST write it back down. The old `>=` test
// short-circuited exactly this case, so an already-boosted (1024) game thread
// never had its floor derated to ~614. We do NOT let this fire when *another*
// source (touch/spillover) raised the kernel floor above priority's genuine
// target: then lastPriorityMin already equals targetMin (we never asked for
// more), so iMustLower is false and we correctly leave the higher floor up.
//
// v4.4.9 R1-2: after a daemon restart (no reboot), the per-tid ledger
// g_floors() is empty (lastPriorityMin=0) but the kernel still carries the old
// instance's boost on SCHED_FIFO threads. Without the guard below, a thermal
// soft-derate (target=614) on a stale-boosted thread (kernel=1024, last=0)
// would compute kernelHighEnough=true && !iMustLower=true -> skip the write-down,
// leaving the thread's floor stuck at 1024 forever. The fix: when the ledger
// has no record for this tid+src (lastPriorityMin==0) AND the kernel reads
// HIGHER than our target, we cannot prove we previously wrote the higher value,
// so we must write it down to correct any stale boost from the prior instance.
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

// 薄封装：调 sched_setattr 系统调用。返回 -1 表示失败（errno 已设置）。
int schedSetAttr(int tid, aatpe_sched_attr* attr, unsigned long flags) {
    return static_cast<int>(syscall(__NR_sched_setattr, tid, attr, flags));
}

// __NR_sched_getattr fallback (x86_64 = 315, arm64 = 276).
#ifndef __NR_sched_getattr
#if defined(__aarch64__)
#define __NR_sched_getattr 276
#else
#define __NR_sched_getattr 315
#endif
#endif

// read the thread's CURRENT sched_attr BEFORE AATPE mutates it, so
// uninstall.sh can restore the original uclamp/policy instead of blindly resetting.
// Returns false on failure (thread gone / unsupported) and leaves out untouched.
bool schedGetAttr(int tid, aatpe_sched_attr* out) {
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    // P0-1: sched_getattr(pid, attr, size, flags). The 3rd arg is the size of
    // the attr buffer the kernel may fill. Passing 0 meant the kernel read/wrote
    // zero bytes, so out stayed all-zero and callers compared against a blank
    // policy (idempotency check always thought the thread was unboosted).
    return syscall(__NR_sched_getattr, tid, out, sizeof(*out), 0) == 0;
}

// ---------------------------------------------------------------------------
// global RT-budget + idempotency.
//
// Bound the number of simultaneously RT-boosted TIDs (kRtMaxBoosted=64). When the
// budget is exhausted we prune entries for dead threads and, if still full, skip
// the RT boost (caller falls back to the nice path). We also make the boost
// idempotent: if the thread already runs at the target policy+priority, we do not
// re-issue sched_setattr on every scan.
// raised from 32 to 64 — modern games (Honor of Kings) have 20-40
// frame-critical threads; 32 was too small and caused spurious nice-degrade.
// ---------------------------------------------------------------------------
static constexpr int kRtMaxBoosted = 64;
std::mutex           g_rtMutex;
std::set<int>        g_rtBoostedTids;
// ROUND-014 P3: starttime snapshot per boosted tid — guards against TID reuse.
static std::unordered_map<int, unsigned long long> g_rtStarttimes;

#ifdef AATPE_HOST_TEST
void aatpeRtDebugSeedImpl(int tid, unsigned long long starttime) {
    g_rtBoostedTids.insert(tid);
    g_rtStarttimes[tid] = starttime;
}
#endif
static bool readThreadStarttime(int tid, unsigned long long* out);   // fwd

#ifdef AATPE_HOST_TEST
using RtAttrOkFn = bool(*)(int tid, void* attrOut);
using RtStartFn  = bool(*)(int tid, unsigned long long* out);
static RtAttrOkFn g_testAttrFn = nullptr;
static RtStartFn  g_testStartFn = nullptr;
extern "C" void aatpeRtSetHooksForTest(RtAttrOkFn a, RtStartFn s) { g_testAttrFn=a; g_testStartFn=s; }
extern "C" int  aatpeRtCountForTest() { return (int)g_rtBoostedTids.size(); }
extern "C" int  aatpeRtContainsForTest(int tid) { return g_rtBoostedTids.count(tid); }
#endif

// STATE-04: prune dead RT slots WITHOUT holding g_rtMutex across the slow
// schedGetAttr(2)/proc-read calls. The old loop held g_rtMutex while doing up
// to 64 syscalls + 64 /proc/<tid>/stat opens, blocking aatpeRestoreThreadPriority()
// (which also takes g_rtMutex) for tens of ms on a busy system. We now:
//   1. take the lock, snapshot (tids, recorded starttimes), release the lock;
//   2. run the slow liveness probes lock-free;
//   3. re-take the lock briefly and drop only the snapshot tids that are dead
//      or TID-reused — tids inserted by other callers during step 2 are kept.
static void pruneDeadRtTids() {
    // 1. snapshot under the lock.
    std::set<int> snapTids;
    std::unordered_map<int, unsigned long long> snapStart;
    {
        std::lock_guard<std::mutex> lk(g_rtMutex);
        snapTids = g_rtBoostedTids;
        snapStart = g_rtStarttimes;
    }

    // 2. slow probes outside the lock.
    std::set<int> deadTids;  // tids from the snapshot that are dead/reused
    std::unordered_map<int, unsigned long long> liveStart;  // refreshed starttimes for live tids
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
        if (it != snapStart.end() && it->second != st) { deadTids.insert(tid); continue; }  // TID reused
        liveStart[tid] = st;
    }

    // 3. under a fresh lock, drop dead tids and refresh live starttimes.
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
// ROUND-014 P3: exported so placement tick prunes dead RT threads each cycle.
// D1 deadlock fix (v4.3.1h5): NEVER block the decision main loop on g_rtMutex.
// Pruning dead RT slots is a deferrable housekeeping action; if the lock is held
// by another path we skip this cycle rather than self-deadlock when placement
// calls us while a nested RT-boost path already holds g_rtMutex.
extern "C" void aatpePruneDeadRtTids() {
    // STATE-04: pruneDeadRtTids() does its own locking; it never blocks the
    // caller on g_rtMutex for long (snapshot/commit are microseconds).
    pruneDeadRtTids();
}

// record (orig policy/priority/nice/uclamp, applied ...) under
// RUN_DIR/recovery/<tid>.rec via the UNIFIED recovery module (aatpe_recovery).
//
// //   P0-2: the recovery set is rebuilt at daemon start by recoveryBootload() from
//         the on-disk files, so a restarted daemon does NOT re-snapshot a thread
//         whose state it already modified (which would corrupt "orig").
//   P0-4: recoveryRecordSched() returns bool; the tid is only considered recorded
//         after the atomic write succeeds. A failed write retries next scan.
//   P0-6: orig_sched_priority / applied_sched_priority are now captured so a
//         thread that was SCHED_RR prio=10 can be restored to prio=10.

// P1-10: read the thread's kernel starttime (stat field 22). This is the identity
// token: a TID can be reused after a thread exits, and the new thread has a
// DIFFERENT starttime, so a stale recovery record must not be applied to it.
bool readThreadStarttime(int tid, unsigned long long* out) {
    // P0-2: AATPE boosts threads of OTHER processes (the foreground game app),
    // not threads of the daemon itself. /proc/self/task/<tid> only enumerates
    // the daemon's own threads, so for a foreign tid this node never existed and
    // starttime was always 0 — defeating the TID-reuse identity check. The global
    // /proc/<tid>/stat node is visible for every task (process or thread) when
    // running as root/KernelSU.
    std::string path = "/proc/" + std::to_string(tid) + "/stat";
    std::ifstream f(path.c_str());
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    // P2-18: use rfind(") ") to locate the closing paren delimiter instead of
    // rfind(')') — comm may contain ')' characters, but the closing paren is
    // always the LAST ") " sequence in the line (everything after is numeric).
    size_t rp = line.rfind(") ");
    if (rp == std::string::npos || rp + 1 >= line.size()) return false;
    std::istringstream ss(line.substr(rp + 2));
    // P1-1: the first token after comm is the single-char process state
    // (R/S/D/Z/T). >> into unsigned long long fails on it, causing starttime
    // to always be 0. Read and discard it first, then parse numeric fields.
    char state;
    if (!(ss >> state)) return false;
    // Fields after state (proc_pid_stat(5)):
    //   4=ppid 5=pgrp 6=session 7=tty_nr 8=tpgid 9=flags
    //   10=minflt 11=cminflt 12=majflt 13=cmajflt
    //   14=utime 15=stime 16=cutime 17=cstime
    //   18=priority 19=nice 20=num_threads 21=itrealvalue
    //   22=starttime  <-- target (19th numeric after state)
    unsigned long long v;
    for (int i = 1; i <= 19; ++i) {
        if (!(ss >> v)) return false;
        if (i == 19) { *out = v; return true; }
    }
    return false;
}

}  // namespace

bool aatpeSetThreadPriority(int tid, ThreadRole role, int level) {
    // Safe Mode suspends RT boost entirely. In safe mode the
    // daemon keeps only low-risk affinity/monitoring; SCHED_FIFO elevation is
    // paused until the safe-mode latch clears.
    if (!aatpe::subsys::safemode::rtBoostAllowed()) {
        return false;
    }
    // 只对 UI/RENDER/GAME 生效；SYSTEM/BACKGROUND/THROUGHPUT 不动。
    if (role != ROLE_S1_UI && role != ROLE_S2_RENDER && role != ROLE_S3_GAME) {
        return false;
    }
    if (tid <= 0) return false;
    if (level < 1) level = 1;
    if (level > 3) level = 3;

    RtParams rt = levelToRt(role, level);

    // snapshot the thread's original scheduling attrs
    // ONLY when the existing record has not already captured the sched dimension.
    // recoveryBootload() repopulates the set from disk at daemon start, so a
    // restarted daemon never re-snapshots a thread it already modified (which
    // would corrupt "orig"). If affinity recorded first (haveOrigAffinity=true but
    // haveOrigSched=false), we still snapshot sched orig now — affinity never
    // touches policy/priority/nice/uclamp, so the live values here are the true
    // originals.
    aatpe_recovery::RecoveryRecord existingRec;
    bool schedAlreadyRecorded =
        aatpe_recovery::recoveryGet(tid, existingRec) && existingRec.haveOrigSched;

    aatpe_sched_attr origAttr{};
    bool origValid = false;
    unsigned long long starttime = 0;
    bool starttimeValid = false;
    if (!schedAlreadyRecorded) {
        origValid = schedGetAttr(tid, &origAttr);
        // R3-004: do not ignore the read result. A failed read left starttime=0,
        // which was then written into the recovery record and broke the TID-reuse
        // guard (a recycled tid could be mis-restored). Skip recording this round;
        // schedAlreadyRecorded stays false so we retry on the next scan.
        starttimeValid = readThreadStarttime(tid, &starttime);
    }

    // idempotency. If the thread already runs at the
    // target policy+priority, do NOT re-issue sched_setattr on every scan (avoids
    // spamming the syscall and racing the recovery record).
    {
        aatpe_sched_attr cur{};
        if (schedGetAttr(tid, &cur)) {
            // R3-005: ALSO verify uclamp_min. A previous step1 (policy/priority) may
            // have succeeded while step2 (uclamp) failed transiently; the old
            // policy+priority-only match then returned early and uclamp was never
            // retried. If uclamp_min is still below target, fall through so step2
            // re-issues it (step1 re-applies the same policy idempotently).
            uint32_t targetUclampMin = roleToUclampMin(role);
            // v4.4.8 MAJOR-2: consult the per-tid ledger to learn what floor THIS
            // priority source last wrote, so a thermal soft-derate (target dropped
            // below our own last write) forces the write-down instead of being
            // swallowed by the `cur >= target` steady-state test. See
            // uclampRewriteSkippable() for the full rationale.
            uint32_t lastPriorityMin =
                aatpe_mech::aatpeUclampFloorGet(tid, aatpe_mech::AATPE_UCLAMP_PRIORITY);
            bool uclampSatisfied =
                uclampRewriteSkippable(cur.sched_util_min, targetUclampMin, lastPriorityMin);
            if (cur.sched_policy == (uint32_t)rt.policy &&
                cur.sched_priority == (uint32_t)rt.priority &&
                uclampSatisfied) {
                // STATE-04: prune dead entries BEFORE taking g_rtMutex so the
                // slow schedGetAttr/probe loop doesn't block restore paths.
                pruneDeadRtTids();
                std::lock_guard<std::mutex> rtk(g_rtMutex);
                // The thread is already running at the target RT state. It MUST
                // be tracked in g_rtBoostedTids regardless of the budget cap:
                // after a daemon restart, threads boosted by the previous instance
                // are still SCHED_FIFO/RR but g_rtBoostedTids starts empty. If we
                // skipped insertion here, the budget would be under-counted and
                // we could over-boost on top of already-RT threads.
                g_rtBoostedTids.insert(tid);
                { unsigned long long st=0; if (readThreadStarttime(tid,&st)) g_rtStarttimes[tid]=st; }
                return true;
            }
        }
    }

    // global RT budget. If the tid is not already counted
    // and we are at the cap, prune dead entries; if still at the cap, refuse the RT
    // boost and route to the nice-degrade path below (rtBudgetFree=false).
    bool rtBudgetFree = true;
    // G2-006: remember whether THIS call inserted the tid into g_rtBoostedTids.
    // If step1 (sched_setattr policy) then fails (EPERM/ENOSYS/ESRCH...) the
    // thread never actually became RT, but it would otherwise keep occupying a
    // budget slot forever (pruneDeadRtTidsLocked only drops DEAD tids, not
    // non-RT live ones). Release the slot we took on every failure path.
    bool weTookBudget = false;
    {
        std::unique_lock<std::mutex> rtk(g_rtMutex);
        if (g_rtBoostedTids.count(tid) == 0 &&
            (int)g_rtBoostedTids.size() >= kRtMaxBoosted) {
            // STATE-04: release the lock during the slow prune probe loop.
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

    // G2-006 helper: drop the budget slot if we took it and the thread never went RT.
    auto releaseBudgetIfWeTook = [&]() {
        if (weTookBudget) {
            std::lock_guard<std::mutex> rtk(g_rtMutex);
            g_rtBoostedTids.erase(tid);
            g_rtStarttimes.erase(tid);
            weTookBudget = false;
        }
    };

    // ---- Step 1: 切换调度类到 SCHED_FIFO / SCHED_RR ----
    aatpe_sched_attr attr{};
    attr.size          = sizeof(attr);
    attr.sched_policy  = static_cast<uint32_t>(rt.policy);
    attr.sched_flags   = 0;
    attr.sched_priority = static_cast<uint32_t>(rt.priority);

    int rc;
    if (!rtBudgetFree) {
        rc = -1; errno = EBUSY;   // route into the degrade-to-nice branch below
    } else {
        rc = schedSetAttr(tid, &attr, 0);
    }
    bool rtOk = (rc == 0);
    if (!rtOk) {
        int e = errno;
        // EPERM = 权限不足；ENOSYS = 内核未开 CONFIG_SCHED_CORE / sched_setattr；
        // EBUSY = 本轮 RT budget 已用尽（round-4）。这三种都降级为 nice 调整。
        if (e == EPERM || e == ENOSYS || e == EBUSY) {
            int niceVal = roleToNice(role);
            if (niceVal != 0 && setpriority(PRIO_PROCESS, tid, niceVal) == 0) {
                // nice fallback DID mutate the thread. Record the applied nice so
                // uninstall.sh restores orig_nice (only when live nice == applied).
                // applied policy/priority/uclamp stay at orig (we only touched nice).
                // P0-4: recoveryRecordSched returns false on write failure; we do
                // NOT mark success based on a failed record.
                if (!schedAlreadyRecorded && origValid && starttimeValid) {
                    aatpe_recovery::recoveryRecordSched(
                        0, tid, starttime,
                        origAttr.sched_policy, origAttr.sched_priority, (int)origAttr.sched_nice,
                        origAttr.sched_util_min, origAttr.sched_util_max,
                        origAttr.sched_policy, origAttr.sched_priority, niceVal,
                        origAttr.sched_util_min, origAttr.sched_util_max);
                } else if (schedAlreadyRecorded) {
                    // ROUND-013: refresh applied sched dims on subsequent success.
                    aatpe_recovery::recoveryRefreshAppliedSched(
                        tid, origAttr.sched_policy, origAttr.sched_priority,
                        niceVal,
                        origAttr.sched_util_min, origAttr.sched_util_max);
                }
                LOG_WARN("aatpeSetThreadPriority: tid=%d role=%d RT denied (errno=%d=%s), "
                         "degraded to nice=%d",
                         tid, (int)role, e, aatpeErrStr(e), niceVal);
                releaseBudgetIfWeTook();  // G2-006: never went RT -> free the slot
                return true;  // nice 降级成功也算部分成功
            }
            LOG_WARN("aatpeSetThreadPriority: tid=%d role=%d RT setattr failed errno=%d=%s "
                     "and nice degrade failed errno=%d",
                     tid, (int)role, e, aatpeErrStr(e), errno);
            releaseBudgetIfWeTook();  // G2-006
            return false;
        }
        // 其他 errno（EINVAL/ESRCH）也是软错误：线程可能已退出，不崩溃。
        LOG_WARN("aatpeSetThreadPriority: tid=%d role=%d sched_setattr(policy) failed errno=%d=%s",
                 tid, (int)role, e, aatpeErrStr(e));
        releaseBudgetIfWeTook();  // G2-006
        return false;
    }

    // ---- Step 2: 设置 uclamp（P1-5: role-dependent min, max=1024）----
    bool uclampApplied = false;
    uint32_t uclampMin = roleToUclampMin(role);
    // s_uclampSupport encoding:
    //   -2 = partial: uclamp syscall exists but SCHED_FLAG_KEEP_POLICY was NOT
    //        backported (step2 returns 0 but silently resets policy to NORMAL).
    //        Discovered by post-step2 verification (P1-1). Skip step2 forever.
    //   -1 = kernel does NOT support uclamp (E2BIG/ENOSYS/EINVAL-on-first-probe).
    //    0 = not yet probed.
    //    1 = kernel supports uclamp + KEEP_POLICY (verified).
    // B3/M4: if latched unsupported but the re-probe interval elapsed, drop the
    // latch and re-probe this thread (transient / late kernel enable).
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
        clamp.sched_util_min = uclampMin;  // P1-5: RENDER/GAME=1024, UI=512
        clamp.sched_util_max = 1024;       // 不限制上限
        int rc2 = schedSetAttr(tid, &clamp, 0);
        if (rc2 != 0) {
            int e2 = errno;
            // P1-2: E2BIG/ENOSYS always latch as global-unsupported.
            // EINVAL latches as global-unsupported ONLY on the first probe
            // (s_uclampSupport==0); on later threads it is a per-thread parameter
            // issue (e.g. util_min out of range) and we just LOG_WARN once.
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
            // P1-1: On MTK vendor kernels that backported the uclamp fields but
            // NOT SCHED_FLAG_KEEP_POLICY, step2 returns 0 success but the kernel
            // ignores the KEEP_POLICY flag and resets sched_policy to SCHED_NORMAL
            // (0) because clamp.sched_policy is zero-initialized. The caller then
            // believes RT is active while the thread silently fell back to CFS.
            // Verify the policy survived; if it reverted, latch -2 (partial) so
            // future cycles skip step2 without wiping RT.
            aatpe_sched_attr verifyAttr{};
            bool policySurvived = false;
            bool readbackOk = schedGetAttr(tid, &verifyAttr);
            if (readbackOk) {
                policySurvived = (verifyAttr.sched_policy == (uint32_t)rt.policy);
            } else {
                // Transient: thread exited between step2 and readback (ESRCH).
                // This is NOT a kernel capability issue — skip this thread only,
                // do NOT latch global s_uclampSupport=-2 (would disable uclamp
                // for all threads for 60s on a transient exit).
                LOG_INFO("aatpeSetThreadPriority: tid=%d sched_getattr readback "
                         "failed errno=%d (thread may have exited, skipping)",
                         tid, errno);
                return false;
            }
            // P3-22: removed redundant second schedGetAttr(tid, &verifyAttr) —
            // verifyAttr already holds the post-step2 state read above.
            if (!policySurvived) {
                // KEEP_POLICY not backported: step2 clobbered our RT policy.
                // Re-apply step1 to restore RT (uclamp is lost, but RT is critical).
                aatpe_sched_attr repair{};
                repair.size        = sizeof(repair);
                repair.sched_policy = static_cast<uint32_t>(rt.policy);
                repair.sched_flags  = 0;
                repair.sched_priority = static_cast<uint32_t>(rt.priority);
                // P3-23: check schedSetAttr return value.
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
                // ROUND-005: also verify the util clamp fields actually stuck.
                // Research trap: 4.14 vendor kernels may return 0 AND keep the RT
                // policy, yet silently ignore sched_util_min/max. Comparing only
                // policySurvived then false-latches s_uclampSupport=1 and the mech
                // layer picks uclamp forever (D1100 prime boost silently dead).
                // Tolerate the system clamping the value: accept if readback is at
                // least half the request (avoids false-negative when sys max <1024).
                bool utilClampSticky =
                    (verifyAttr.sched_util_min >= (uclampMin / 2)) &&
                    (verifyAttr.sched_util_max > 0);
                if (!utilClampSticky) {
                    // policy kept, util ignored -> -3 policy-only (distinct from -2).
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
                    // audit4 F4: register the RT floor in the shared per-tid ledger so
                    // touch's RELEASE (write 0) recomputes max(touch, spillover,
                    // priority) instead of stomping this 512/1024 floor. The ledger
                    // write is idempotent (writes back the same effective floor).
                    aatpe_mech::aatpeUclampFloorSet(tid, aatpe_mech::AATPE_UCLAMP_PRIORITY,
                                                    uclampMin);
                }
            }
        }
    }

    // On the first sched mutation, record the FULL applied state so uninstall.sh
    // restores orig ONLY when live policy/priority/nice/min/max all still equal
    // what we set. If uclamp could not be applied, applied_min/max = orig so the
    // uclamp restore is a no-op and only policy/priority/nice are restored.
    // P0-4: the write failure is logged inside recoveryRecordSched; we still
    // return true (the syscall succeeded) but the tid stays unrecorded for retry.
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
        // ROUND-013: refresh applied sched dims on subsequent success.
        aatpe_recovery::recoveryRefreshAppliedSched(
            tid,
            static_cast<uint32_t>(rt.policy), static_cast<uint32_t>(rt.priority),
            (int)origAttr.sched_nice,
            uclampApplied ? uclampMin : origAttr.sched_util_min,
            uclampApplied ? 1024u      : origAttr.sched_util_max);
    }
    return true;
}

// ---------------------------------------------------------------------------
// #1/#2 fix: explicit, UNGATED runtime RT restore.
//
// The old "release" path called aatpeSetThreadPriority(tid, role, level=0), but
// that setter clamps level<1 to 1 (line ~358) and ONLY ever raises a thread to
// SCHED_FIFO/RR — it has no branch that writes SCHED_OTHER. So after a cold-start
// "release" the thread stayed at SCHED_RR + uclamp_min=1024 forever. Worse, the
// setter's very first line checks rtBoostAllowed(); when safe mode latches AFTER a
// boost, the release short-circuits (return false) and the RT thread is stuck until
// a reboot.
//
// This new restore path deliberately does NOT consult rtBoostAllowed(): dropping a
// thread back to SCHED_OTHER / original nice / original uclamp is a low-risk
// DE-escalation, safe to perform even when safe mode is active. It reads the
// recorded original attrs from the unified recovery record (captured at boost time)
// and reverts the thread to them, then drops the budget slot and the on-disk record
// (we just restored at runtime, so boot recovery has nothing left to do).
//
// If no recovery record exists for the tid (record write failed, or the boost only
// took the nice-degrade path), we fall back to the safe default: SCHED_OTHER, nice 0,
// uclamp_min=0 / max=1024.
// ---------------------------------------------------------------------------
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
// Production: actually issue the restore syscalls. Host test: record the intended
// parameters instead (so the test can assert policy==SCHED_OTHER without needing a
// live thread / real caps). The capture deliberately records the SAME parameters the
// production syscall path uses, so the test pins "restore really targets SCHED_OTHER"
// rather than only "a function was called".
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
        // ESRCH = thread already gone (benign). Anything else is soft — best-effort.
        if (errno != ESRCH)
            LOG_WARN("aatpeRestoreThreadPriority: tid=%d sched_setscheduler(%u) failed errno=%d=%s",
                     tid, policy, errno, aatpeErrStr(errno));
    }
    if (setpriority(PRIO_PROCESS, tid, nice) != 0) {
        // soft: ESRCH/EPERM are non-fatal for a best-effort release.
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
    // Kernels without uclamp support (MTK 4.14) reject this with E2BIG/ENOSYS — the
    // policy/nice restore above already undid the user-visible RT elevation, so a
    // uclamp failure here is non-fatal and intentionally not logged (one-shot per tid).
    if (schedSetAttr(tid, &attr, 0) != 0) { /* soft */ }
}
}  // namespace

bool aatpeRestoreThreadPriority(int tid) {
    if (tid <= 0) return false;

    // Pull the recorded originals. recoveryGet returns false when the tid was never
    // recorded (first boost syscall succeeded but the .rec write failed, or the thread
    // only ever got the nice-degrade path) — then we fall back to safe defaults.
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
    // audit4 F4: drop the RT floor from the shared ledger so touch/spillover can
    // recompute without this thread's RT reservation (idempotent).
    aatpe_mech::aatpeUclampFloorClear(tid, aatpe_mech::AATPE_UCLAMP_PRIORITY);

    // Drop the RT budget slot + starttime token regardless of syscall success — the
    // thread is no longer under our RT management.
    {
        std::lock_guard<std::mutex> lk(g_rtMutex);
        g_rtBoostedTids.erase(tid);
        g_rtStarttimes.erase(tid);
    }
    // We just restored live state at runtime; the on-disk record is obsolete and must
    // not drive boot recovery (which would otherwise sched_setscheduler twice).
    if (haveRec) aatpe_recovery::recoveryErase(tid);

    LOG_INFO("aatpeRestoreThreadPriority: tid=%d restored to policy=%u nice=%d "
             "(record=%d)", tid, targetPolicy, targetNice, (int)haveRec);
    return true;
}

}  // namespace aatpe_priority

#ifdef AATPE_HOST_TEST
extern "C" void aatpeRtDebugSeedForTest(int tid, unsigned long long starttime) {
    aatpe_priority::aatpeRtDebugSeedImpl(tid, starttime);
}
#endif
