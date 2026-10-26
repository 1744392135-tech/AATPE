// aatpe_placement_mech.cpp — see header. The selection table is PURE so the host
// unit test can inject arbitrary SchedCaps and assert the chosen mechanism.
// Probe + cpuset move use an injectable root path so host tests can build a fake
// sysfs under /tmp.
#include "aatpe_placement_mech.h"
#include "aatpe_caps.h"       // WS-C: G1 caps::getCaps()
#include "aatped.h"            // STATE_DIR, writeFile, readFile
#include "aatped_priority.h"   // aatpe_priority::uclampSupportState
#include "aatpe_sched_abi.h"   // ROUND-W3: SCHED_FLAG_* single source of truth (line 32)
#include "aatped_logging.h"
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/syscall.h>
#include <sys/types.h>

namespace aatpe_mech {

// WS-C: real per-task uclamp set (R2 §6). Uses unified sched_attr from
// aatpe_sched_abi.h (correct 56-byte layout incl. sched_period).
// GPUTOP-13: lastTid/lastMin are pure diagnostics — make them atomic so the
// diagnostic getters do not need to take g_floorMtx (which would block on the
// ledger lock). The syscall itself remains under g_floorMtx for ledger
// consistency.
static std::atomic<int> g_uclampLastTid{-1};
static std::atomic<unsigned int> g_uclampLastMin{0};

// audit4 F4: issue the actual sched_setattr util_min write (no bookkeeping).
// SCHED_FLAG_UTIL_CLAMP_MIN only -> util_max / policy / params are kept untouched.
static int issueSetMinSyscall(int tid, unsigned int utilMin) {
    g_uclampLastTid.store(tid, std::memory_order_relaxed);
    g_uclampLastMin.store(utilMin, std::memory_order_relaxed);
#ifdef __ANDROID__
    struct sched_attr a; memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_UTIL_CLAMP_MIN;
    a.sched_util_min = utilMin;   // UINT32_MAX = reset
    return syscall(SYS_sched_setattr, tid, &a, 0);
#else
    return 0;
#endif
}

// audit4 F4: per-tid, per-source desired util_min floors. The effective floor is
// the MAX across producers, so touch's RELEASE (utilMin=0) only removes touch's
// own lift and can no longer stomp the RT priority 512 floor or a spillover floor.
static std::mutex& g_floorMtx() { static std::mutex m; return m; }
static std::unordered_map<int, std::array<unsigned, AATPE_UCLAMP_SRC_N>>& g_floors() {
    static std::unordered_map<int, std::array<unsigned, AATPE_UCLAMP_SRC_N>> m;
    return m;
}

int aatpeUclampFloorSet(int tid, int src, unsigned int utilMin) {
    if (tid < 0 || src < 0 || src >= AATPE_UCLAMP_SRC_N) return -1;
    std::lock_guard<std::mutex> lk(g_floorMtx());
    auto& arr = g_floors()[tid];
    arr[src] = utilMin;
    unsigned eff = 0;
    for (unsigned v : arr) eff = std::max(eff, v);
    return issueSetMinSyscall(tid, eff);
}

void aatpeUclampFloorClear(int tid, int src) {
    (void)aatpeUclampFloorSet(tid, src, 0u);
}

unsigned int aatpeUclampFloorGet(int tid, int src) {
    if (tid < 0 || src < 0 || src >= AATPE_UCLAMP_SRC_N) return 0u;
    std::lock_guard<std::mutex> lk(g_floorMtx());
    auto it = g_floors().find(tid);
    if (it == g_floors().end()) return 0u;
    return it->second[src];
}

int aatpeUclampSetMin(int tid, unsigned int utilMin) {
    // Legacy entry point. After placement.cpp switched its spillover writes to the
    // source-tagged API, the ONLY caller left is the touch actuator callback
    // (input_monitor -> aatpe_mech::aatpeUclampSetMin), so this maps to the TOUCH
    // producer. utilMin=0 = touch release -> drops touch's lift but keeps the max
    // of the other producers. utilMin=UINT32_MAX = hard reset -> drop ALL producers.
    if (utilMin == 0xFFFFFFFFu) {
        std::lock_guard<std::mutex> lk(g_floorMtx());
        g_floors().erase(tid);
        return issueSetMinSyscall(tid, utilMin);
    }
    return aatpeUclampFloorSet(tid, AATPE_UCLAMP_TOUCH, utilMin);
}

// RC6 (B2): erase the per-tid floor ledger entry when a thread dies or its tid is
// recycled. g_floors was keyed by bare tid with no starttime check and no reap, so a
// stale floor survived death and was inherited (via max()) by a newly-spawned thread
// that reused the same tid, wrongly lifting it. The thread_scan death/REUSE chokepoint
// calls this; issueSetMinSyscall(tid, UINT32_MAX) on a dead tid is ESRCH (harmless) —
// the ledger erase is what matters. The existing thread_scan starttime-based REUSE
// detection (refreshTrackedThread) is the starttime guard; this is the clear-on-reap.
void aatpeUclampFloorReap(int tid) {
    if (tid < 0) return;
    std::lock_guard<std::mutex> lk(g_floorMtx());
    g_floors().erase(tid);
    issueSetMinSyscall(tid, 0xFFFFFFFFu);
}
// GPUTOP-13: diagnostic getters no longer take g_floorMtx — lastTid/lastMin are
// atomic, so readers do not block on the ledger lock.
int aatpeUclampGetLastTid() {
    return g_uclampLastTid.load(std::memory_order_relaxed);
}
unsigned int aatpeUclampGetLastMin() {
    return g_uclampLastMin.load(std::memory_order_relaxed);
}

// --- test seam: when non-empty, all sysfs paths are prefixed with this root ---
static std::string& testRootRef() { static std::string r; return r; }
#ifdef AATPE_HOST_TEST
void mechSetTestRoot(const std::string& root) { testRootRef() = root; }
#endif
static std::string pfx(const std::string& p) { return testRootRef() + p; }

// injectable uclamp verdict (host test simulates yes/partial/no without a kernel).
static int& uclampVerdictRef() { static int v = -99; return v; }  // -99 = not set (real probe)
#ifdef AATPE_HOST_TEST
void mechSetUclampVerdict(int v) { uclampVerdictRef() = v; }
#endif

// ROUND-013 P0-2: cgroup uclamp group-floor ownership with reference counting.
// ROUND-014 P3: cgPrior stores original node string (not parsed uint),
//   so v2 "12.34" format is preserved exactly on restore.
static std::mutex& cgUclampMutex() { static std::mutex m; return m; }
static std::unordered_map<std::string,std::string>& cgPriorStr() {
    static std::unordered_map<std::string,std::string> m; return m;
}
static std::unordered_map<std::string,bool>& cgDefaulted() {
    static std::unordered_map<std::string,bool> m; return m;
}
static std::unordered_map<std::string,int>& cgActive() {
    static std::unordered_map<std::string,int> m; return m;
}

// Rebuild cg_uclamp_active marker from active map. Unlink when empty.
// Must be called with cgUclampMutex held.
static void rebuildCgMarkerLocked() {
    std::string payload;
    for (auto& [g, a] : cgActive()) {
        if (a > 0) {
            if (!payload.empty()) payload += ",";
            payload += g;
        }
    }
    if (payload.empty()) {
        unlink((std::string(STATE_DIR) + "/cg_uclamp_active").c_str());
    } else {
        writeFile((std::string(STATE_DIR) + "/cg_uclamp_active").c_str(), payload);
    }
}

// Convert 0-1024 util value to the string the cgroup's cpu.uclamp.min node expects.
// #23: the scale is probed at startup (aatpe_caps probeCgroupUclampScale):
//   "percent"  -> 0-100 percentage (standard v2 / most v1 backports)
//   "raw1024"  -> 0-1024 raw util (2019 LPC prototype, rare)
//   "unknown"  -> safe default: percent.
// Previously this was hard-coded to percent, so on a raw1024 kernel a target of
// 880 was written as "85.94" and interpreted as ~8.4% — boost silently dead.
static std::string utilToCgroupPct(unsigned int util) {
    if (util > 1024) util = 1024;
    std::string scale = aatpe::caps::getCaps().cgroupUclampScale;
    if (scale == "raw1024") {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", util);
        return std::string(buf);
    }
    float pct = (float)util * 100.0f / 1024.0f;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", pct);
    return std::string(buf);
}

// WS-C: real cgroup uclamp.min write. The tid must first join the CPU
// controller group (v1 /dev/cpuctl, v2 unified), else group-level uclamp does
// not apply to it.
bool writeCgroupUclampMin(int tid, unsigned int minValue, const std::string& group) {
    aatpe::caps::DeviceCaps caps = aatpe::caps::getCaps();
    // 1) move tid into the cpu controller group (three-state; -2 = v2 no thread granularity -> fail).
    int wrc = aatpe::caps::writeTidToCpuCgroupGroup(tid, group);
    if (wrc != 0) return false;   // member migration failed: do NOT write group floor.
    // 2) write group uclamp.min with prior-floor ownership.
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    auto it = cgPriorStr().find(group);
    if (it == cgPriorStr().end()) {
        // C31: distinguish read failure from empty content.
        std::string p = caps.cpuRoot + "/" + group + "/cpu.uclamp.min";
        // Check if file exists first (access)
        if (::access(p.c_str(), F_OK) != 0) {
            // File doesn't exist at all → defaulted, nothing to restore
            cgDefaulted()[group] = true;
            cgPriorStr()[group] = "";
        } else {
            std::string cur = readFile(p.c_str());
            if (cur.empty()) {
                // File exists but read returned empty (IO error or zero bytes)
                // If read failed, don't cache - let next boost retry.
                // We can't distinguish EIO from empty easily; treat as defaulted
                // only if we're confident. Use access() + fopen check.
                FILE* f = fopen(p.c_str(), "r");
                if (!f) {
                    // Read failure: don't cache, return error
                    return false;
                }
                char buf[8];
                size_t n = fread(buf, 1, sizeof(buf)-1, f);
                fclose(f);
                if (n == 0) {
                    // File exists but is empty → defaulted
                    cgDefaulted()[group] = true;
                    cgPriorStr()[group] = "";
                } else {
                    // Partial read - shouldn't happen for tiny files, but handle
                    cur.assign(buf, n);
                    cur.erase(0, cur.find_first_not_of(" \t\r\n"));
                    cur.erase(cur.find_last_not_of(" \t\r\n") + 1);
                    cgPriorStr()[group] = cur;
                }
            } else {
                // Successful read, trim whitespace
                cur.erase(0, cur.find_first_not_of(" \t\r\n"));
                cur.erase(cur.find_last_not_of(" \t\r\n") + 1);
                cgPriorStr()[group] = cur;
            }
        }
    }
    // C06: write floor first, then increment active count / publish marker.
    // If floor write fails, don't count this boost.
    std::string path = caps.cpuRoot + "/" + group + "/cpu.uclamp.min";
    std::string val = utilToCgroupPct(minValue);
    if (!writeFile(path.c_str(), val)) {
        // Floor write failed - don't increment active count
        return false;
    }
    int& active = cgActive()[group];
    active++;
    rebuildCgMarkerLocked();
    return true;
}

// ROUND-013: boostGroup = the floor group we boosted to (e.g. "top-app");
// savedGroup = the tid's original cgroup to migrate back to.
// #4: the active refcount is bound to the thread STILL relying on the group floor.
// If the move back to savedGroup FAILS, the thread is physically still in the
// boost group and MUST keep its floor — so we do NOT decrement active (and do NOT
// restore the floor) in that case. The caller keeps the record and retries next
// tick. Returns the move result so the caller can decide whether to drop the record.
bool restoreTidFromCgroupUclamp(int tid, const std::string& boostGroup, const std::string& savedGroup) {
    if (boostGroup.empty()) return false;
    aatpe::caps::DeviceCaps caps = aatpe::caps::getCaps();
    bool moved = true;
    if (!savedGroup.empty()) moved = (aatpe::caps::writeTidToCpuCgroupGroup(tid, savedGroup) == 0);
    if (!moved) {
        // #4: move-back failed (ESRCH/EINVAL). Keep active + floor up; caller retries.
        return false;
    }
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    int& active = cgActive()[boostGroup];
    if (active > 0) active--;
    if (active == 0) {
        // C02: defaulted groups write "0", non-defaulted write back original string.
        std::string path = caps.cpuRoot + "/" + boostGroup + "/cpu.uclamp.min";
        if (cgDefaulted()[boostGroup]) {
            writeFile(path.c_str(), "0");
        } else {
            auto it = cgPriorStr().find(boostGroup);
            if (it != cgPriorStr().end()) {
                writeFile(path.c_str(), it->second);
            }
        }
        rebuildCgMarkerLocked();
    }
    return true;
}

// #4: force-release one unit of the group floor bookkeeping WITHOUT a successful
// move-back. Used by the caller only after retries are exhausted (dead/recycled
// tid) so a stuck thread cannot pin the group floor forever.
void forceReleaseCgroupUclamp(const std::string& boostGroup) {
    if (boostGroup.empty()) return;
    aatpe::caps::DeviceCaps caps = aatpe::caps::getCaps();
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    int& active = cgActive()[boostGroup];
    if (active > 0) active--;
    if (active == 0) {
        std::string path = caps.cpuRoot + "/" + boostGroup + "/cpu.uclamp.min";
        if (cgDefaulted()[boostGroup]) {
            writeFile(path.c_str(), "0");
        } else {
            auto it = cgPriorStr().find(boostGroup);
            if (it != cgPriorStr().end()) writeFile(path.c_str(), it->second);
        }
        rebuildCgMarkerLocked();
    }
}

void restoreAllCgroupUclamp() {
    aatpe::caps::DeviceCaps caps = aatpe::caps::getCaps();
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    // C01: iterate ALL groups with prior records, don't skip active>0.
    for (auto& [g, priorStr] : cgPriorStr()) {
        std::string path = caps.cpuRoot + "/" + g + "/cpu.uclamp.min";
        if (cgDefaulted()[g]) {
            writeFile(path.c_str(), "0");
        } else {
            writeFile(path.c_str(), priorStr);
        }
        cgActive()[g] = 0;
    }
    rebuildCgMarkerLocked();
}

// audit4 F3: record group's verbatim original on first touch (idempotent).
// Caller holds cgUclampMutex(). "" means the node was absent / unreadable at first
// touch (treated as defaulted -> restore writes "0").
static std::string recordOriginalLocked(const std::string& group) {
    auto it = cgPriorStr().find(group);
    if (it != cgPriorStr().end()) return it->second;
    aatpe::caps::DeviceCaps caps = aatpe::caps::getCaps();
    const std::string path = caps.cpuRoot + "/" + group + "/cpu.uclamp.min";
    if (::access(path.c_str(), F_OK) != 0) {
        cgDefaulted()[group] = true;
        cgPriorStr()[group] = "";
        return std::string();
    }
    std::string cur = readFile(path.c_str());
    cur.erase(0, cur.find_first_not_of(" \t\r\n"));
    cur.erase(cur.find_last_not_of(" \t\r\n") + 1);
    cgPriorStr()[group] = cur;
    if (cur.empty()) cgDefaulted()[group] = true;
    return cur;
}

std::string cgUclampRecordOriginal(const std::string& group) {
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    return recordOriginalLocked(group);
}

float cgUclampOriginalPct(const std::string& group) {
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    auto it = cgPriorStr().find(group);
    if (it == cgPriorStr().end() || it->second.empty()) return -1.0f;
    double v = strtod(it->second.c_str(), nullptr);
    if (!std::isfinite(v)) return -1.0f;
    aatpe::caps::DeviceCaps caps = aatpe::caps::getCaps();
    if (caps.cgroupUclampScale == "raw1024") v = v * 100.0 / 1024.0;
    if (v < 0.0) v = 0.0;
    if (v > 100.0) v = 100.0;
    return (float)v;
}

void cgUclampHold(const std::string& group) {
    aatpe::caps::DeviceCaps caps = aatpe::caps::getCaps();
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    recordOriginalLocked(group);
    int& active = cgActive()[group];
    active++;
    rebuildCgMarkerLocked();
}

void cgUclampReleaseHold(const std::string& group) {
    aatpe::caps::DeviceCaps caps = aatpe::caps::getCaps();
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    auto it = cgActive().find(group);
    if (it == cgActive().end() || it->second <= 0) return;
    it->second--;
    if (it->second == 0) {
        std::string path = caps.cpuRoot + "/" + group + "/cpu.uclamp.min";
        if (cgDefaulted()[group]) {
            writeFile(path.c_str(), "0");
        } else {
            auto p = cgPriorStr().find(group);
            if (p != cgPriorStr().end()) writeFile(path.c_str(), p->second);
        }
        rebuildCgMarkerLocked();
    }
}

#ifdef AATPE_HOST_TEST
void cgUclampResetForTest() {
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    cgPriorStr().clear();
    cgDefaulted().clear();
    cgActive().clear();
}

void aatpeUclampFloorsResetForTest() {
    std::lock_guard<std::mutex> lk(g_floorMtx());
    g_floors().clear();
}
#endif

BoostMechanism selectBoostMechanism(const SchedCaps& c) {
    if (c.uclamp)        return BoostMechanism::UCLAMP;
    if (c.cgroupUclamp)  return BoostMechanism::CGROUP_UCLAMP;
    if (c.cpusetTopApp && c.hasStune) return BoostMechanism::STUNE_CPUSET;
    return BoostMechanism::AFFINITY;
}

const char* mechanismName(BoostMechanism m) {
    switch (m) {
        case BoostMechanism::UCLAMP:        return "uclamp";
        case BoostMechanism::CGROUP_UCLAMP:  return "cgroup_uclamp";
        case BoostMechanism::STUNE_CPUSET:   return "stune_cpuset";
        case BoostMechanism::AFFINITY:       return "affinity";
        case BoostMechanism::NICE_ONLY:      return "nice_only";
    }
    return "unknown";
}

std::string renderCapabilities(const SchedCaps& c) {
    std::string s = "uclamp=";
    s += c.uclamp ? "yes" : (c.uclampPartial ? "partial" : "no");
    s += " cgroup_uclamp=";       s += c.cgroupUclamp ? "yes" : "no";
    s += " stune=";               s += c.hasStune ? "yes" : "no";
    s += " cpuset_topapp=";       s += c.cpusetTopApp ? "yes" : "no";
    s += " -> mechanism=";        s += mechanismName(selectBoostMechanism(c));
    return s;
}

static bool nodeWritable(const std::string& path) {
    return ::access(pfx(path).c_str(), W_OK) == 0;
}

// Read the tid's current cpuset group path from /proc/<tid>/cgroup.
// ROUND-013 P0-3: supports v1 "N:cpuset:/path" and v2 "0::/path". Hybrid:
// prefer v1 cpuset controller line; fall back to v2 "0::" line.
std::string readThreadCpusetGroup(int tid) {
    std::string outBase, outPath, outHier;
    readThreadCgroupInfo(tid, &outBase, &outPath, &outHier);
    return outBase;
}

// H14: full cgroup introspection. Parses /proc/<tid>/cgroup into:
//   outBase  — basename (e.g. "foreground", "top-app") for legacy dual-write
//   outPath  — full path WITHOUT leading slash (e.g. "a/b/foreground") suitable
//              for joining onto the cgroup mount root
//   outHier  — "v1-cpuset" | "v1-cpu" | "v2-unified" | "" (unknown)
// Returns true if a parseable line was found; false on open/parse failure
// (caller must leave haveOrigCgroupPath=false and fall back to basename).
//
// v1 multi-mount: a line "N:cpuset:/a/b/top-app" identifies the cpuset hierarchy;
//                  "N:cpu:/a/b/top-app" identifies the cpu hierarchy. We record
//                  whichever controller the thread was actually on (cpuset first,
//                  then cpu).
// v2 unified:       "0::/a/b/top-app" -> outHier="v2-unified".
bool readThreadCgroupInfo(int tid, std::string* outBase, std::string* outPath,
                          std::string* outHier) {
    if (outBase)  outBase->clear();
    if (outPath)  outPath->clear();
    if (outHier)  outHier->clear();
    std::string path = pfx("/proc/" + std::to_string(tid) + "/cgroup");
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[512];
    std::string v1Base, v1Path, v2Base, v2Path;
    std::string v1Hier;  // "v1-cpuset" or "v1-cpu"
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        l.erase(l.find_last_not_of(" \t\r\n") + 1);
        // v2 line: "0::/path"
        if (l.rfind("0::", 0) == 0) {
            std::string p = l.substr(3);
            while (!p.empty() && p.front() == '/') p.erase(p.begin());
            if (!p.empty()) {
                size_t slash = p.find_last_of('/');
                v2Base = (slash==std::string::npos)?p:p.substr(slash+1);
                v2Path = p;
            }
            continue;
        }
        // v1 line: "N:ctrl1,cpuset,ctrl3:/path"
        auto c1 = l.find(':');
        auto c2 = l.find(':', c1+1);
        if (c1==std::string::npos || c2==std::string::npos) continue;
        std::string ctrls = l.substr(c1+1, c2-c1-1);
        bool isCpuset = (ctrls=="cpuset") ||
                        (ctrls.rfind("cpuset,",0)==0) ||
                        (ctrls.find(",cpuset,")!=std::string::npos) ||
                        (ctrls.rfind(",cpuset")==ctrls.size()-7);
        bool isCpuCtl = (ctrls=="cpu") ||
                        (ctrls.rfind("cpu,",0)==0) ||
                        (ctrls.find(",cpu,")!=std::string::npos) ||
                        // A5: ",cpu" is 4 chars; must match rfind==size-4. (Was size-3,
                        // an off-by-one that missed v1 lines like "N:sched,cpu:/x".)
                        (ctrls.size()>=4 && ctrls.rfind(",cpu")==ctrls.size()-4);
        if (!isCpuset && !isCpuCtl) continue;
        std::string p = l.substr(c2+1);
        while (!p.empty() && p.front()=='/') p.erase(p.begin());
        if (p.empty()) continue;
        size_t slash = p.find_last_of('/');
        std::string base = (slash==std::string::npos)?p:p.substr(slash+1);
        // Prefer cpuset line over cpu line.
        if (isCpuset || v1Hier.empty()) {
            v1Base = base; v1Path = p;
            v1Hier = isCpuset ? "v1-cpuset" : "v1-cpu";
        }
    }
    fclose(f);
    // Hybrid: v1 cpuset wins; else v2.
    bool found = false;
    if (!v1Base.empty()) {
        if (outBase) *outBase = v1Base;
        if (outPath) *outPath = v1Path;
        if (outHier) *outHier = v1Hier;
        found = true;
    } else if (!v2Base.empty()) {
        if (outBase) *outBase = v2Base;
        if (outPath) *outPath = v2Path;
        if (outHier) *outHier = "v2-unified";
        found = true;
    }
    return found;
}

// Move a single tid into a cpuset group's tasks file (e.g. "/top-app").
// Returns true on write success. A2: check BOTH fprintf and fclose return —
// /dev/cpuset/<g>/tasks write errors (dead tid=ESRCH, missing group=ENOENT,
// bad tid=EINVAL) only surface at flush/fclose. Returning true on a failed write
// left the caller believing the tid was in top-app (joined=true) and skipped the
// affinity fallback, desyncing the promote belief from the kernel.
bool moveTidToCpusetGroup(int tid, const std::string& groupName) {
    std::string tasks = "/dev/cpuset/" + groupName + "/tasks";
    FILE* f = fopen(pfx(tasks).c_str(), "w");
    if (!f) return false;
    if (fprintf(f, "%d\n", tid) < 0) { fclose(f); return false; }
    if (fclose(f) != 0) return false;
    return true;
}

// Real probe with 60s throttled re-probe caching.
// audit2: g_cachedCaps is a multi-byte struct that can be read by the placement
// loop and written by the state-export path on DIFFERENT threads. Guard the
// cached value with g_capsMutex so a reader never observes a torn/partially
// published SchedCaps. The slow on-disk probe runs OUTSIDE the mutex; only the
// tiny copy-out / publish critical sections take it.
static SchedCaps g_cachedCaps;
static std::mutex g_capsMutex;
static std::atomic<long long> g_lastProbeMs{0};

// ROUND-008: read /dev/cpuset/top-app/cpus and check whether it covers cpu.
// If top-app/cpus excludes PRIME (cpu7 on D1100), stune_cpuset can NEVER land a
// thread on PRIME — must fall back to uclamp/affinity.
bool topAppCpusContainsCpu(int cpu) {
    std::ifstream f(pfx("/dev/cpuset/top-app/cpus"));
    std::string s;
    if (f) std::getline(f, s);
    if (s.empty()) return false;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string part = (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
        size_t dash = part.find('-');
        if (dash != std::string::npos) {
            int lo = atoi(part.substr(0, dash).c_str());
            int hi = atoi(part.substr(dash + 1).c_str());
            if (cpu >= lo && cpu <= hi) return true;
        } else if (!part.empty()) {
            if (atoi(part.c_str()) == cpu) return true;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

SchedCaps probeCapabilities() {
    if (!testRootRef().empty()) {
        // host test: always re-probe against the fake root (no cache).
        SchedCaps c;
        c.hasStune       = nodeWritable("/dev/stune/top-app/schedtune.boost");
        c.cgroupUclamp   = nodeWritable("/dev/cpuctl/cpu.uclamp.min");
        c.cpusetTopApp   = nodeWritable("/dev/cpuset/top-app/tasks");
        int v = uclampVerdictRef();
        if (v == 1) c.uclamp = true; else if (v == 2) c.uclampPartial = true;
        return c;
    }

    using namespace std::chrono;
    long long now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    long long last = g_lastProbeMs.load(std::memory_order_acquire);
    if (last != 0 && (now - last) < 60000) {
        // Copy the cached struct under the mutex so a concurrent publish cannot
        // leave us with a torn SchedCaps (audit2).
        std::lock_guard<std::mutex> lk(g_capsMutex);
        return g_cachedCaps;
    }

    SchedCaps c;
    c.hasStune = nodeWritable("/dev/stune/top-app/schedtune.boost") ||
                 nodeWritable("/sys/fs/cgroup/stune/top-app/schedtune.boost") ||
                 nodeWritable("/sys/fs/cgroup/top-app/schedtune.boost");
    c.cgroupUclamp = nodeWritable("/dev/cpuctl/cpu.uclamp.min") ||
                     nodeWritable("/sys/fs/cgroup/cpu.uclamp.min");
    c.cpusetTopApp = nodeWritable("/dev/cpuset/top-app/tasks");
    // per-task uclamp verdict is latched by aatpe_priority (sched_setattr +
    // sched_getattr readback of util_min/util_max).
    int s = aatpe_priority::uclampSupportState();
    if (s == 1) c.uclamp = true;
    else if (s == -2) c.uclampPartial = true;  // KEEP_POLICY not backported
    // -3 policy-only (util ignored): NOT trusted -> uclamp stays false, fall to
    // stune_cpuset/affinity.

    // Publish under the mutex; the timestamp store stays release-ordered. The
    // slow probe above ran lock-free; only the publish is serialized.
    {
        std::lock_guard<std::mutex> lk(g_capsMutex);
        g_cachedCaps = c;
        g_lastProbeMs.store(now, std::memory_order_release);
    }
    // Persist the bitmap for on-device verification.
    writeFile((std::string(STATE_DIR) + "/scheduler_capabilities").c_str(),
              renderCapabilities(c));
    return c;
}

}  // namespace aatpe_mech
