#include "aatpe_placement_mech.h"
#include "aatpe_caps.h"
#include "aatped.h"
#include "aatped_priority.h"
#include "aatpe_sched_abi.h"
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

static std::atomic<int> g_uclampLastTid{-1};
static std::atomic<unsigned int> g_uclampLastMin{0};

static int issueSetMinSyscall(int tid, unsigned int utilMin) {
    g_uclampLastTid.store(tid, std::memory_order_relaxed);
    g_uclampLastMin.store(utilMin, std::memory_order_relaxed);
#ifdef __ANDROID__
    struct sched_attr a; memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_UTIL_CLAMP_MIN;
    a.sched_util_min = utilMin;
    return syscall(SYS_sched_setattr, tid, &a, 0);
#else
    return 0;
#endif
}

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
    if (utilMin == 0xFFFFFFFFu) {
        std::lock_guard<std::mutex> lk(g_floorMtx());
        g_floors().erase(tid);
        return issueSetMinSyscall(tid, utilMin);
    }
    return aatpeUclampFloorSet(tid, AATPE_UCLAMP_TOUCH, utilMin);
}

void aatpeUclampFloorReap(int tid) {
    if (tid < 0) return;
    std::lock_guard<std::mutex> lk(g_floorMtx());
    g_floors().erase(tid);
    issueSetMinSyscall(tid, 0xFFFFFFFFu);
}
int aatpeUclampGetLastTid() {
    return g_uclampLastTid.load(std::memory_order_relaxed);
}
unsigned int aatpeUclampGetLastMin() {
    return g_uclampLastMin.load(std::memory_order_relaxed);
}

static std::string& testRootRef() { static std::string r; return r; }
#ifdef AATPE_HOST_TEST
void mechSetTestRoot(const std::string& root) { testRootRef() = root; }
#endif
static std::string pfx(const std::string& p) { return testRootRef() + p; }

static int& uclampVerdictRef() { static int v = -99; return v; }
#ifdef AATPE_HOST_TEST
void mechSetUclampVerdict(int v) { uclampVerdictRef() = v; }
#endif

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

bool writeCgroupUclampMin(int tid, unsigned int minValue, const std::string& group) {
    aatpe::caps::DeviceCaps caps = aatpe::caps::getCaps();
    int wrc = aatpe::caps::writeTidToCpuCgroupGroup(tid, group);
    if (wrc != 0) return false;
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    auto it = cgPriorStr().find(group);
    if (it == cgPriorStr().end()) {
        std::string p = caps.cpuRoot + "/" + group + "/cpu.uclamp.min";
        if (::access(p.c_str(), F_OK) != 0) {
            cgDefaulted()[group] = true;
            cgPriorStr()[group] = "";
        } else {
            std::string cur = readFile(p.c_str());
            if (cur.empty()) {
                FILE* f = fopen(p.c_str(), "r");
                if (!f) {
                    return false;
                }
                char buf[8];
                size_t n = fread(buf, 1, sizeof(buf)-1, f);
                fclose(f);
                if (n == 0) {
                    cgDefaulted()[group] = true;
                    cgPriorStr()[group] = "";
                } else {
                    cur.assign(buf, n);
                    cur.erase(0, cur.find_first_not_of(" \t\r\n"));
                    cur.erase(cur.find_last_not_of(" \t\r\n") + 1);
                    cgPriorStr()[group] = cur;
                }
            } else {
                cur.erase(0, cur.find_first_not_of(" \t\r\n"));
                cur.erase(cur.find_last_not_of(" \t\r\n") + 1);
                cgPriorStr()[group] = cur;
            }
        }
    }
    std::string path = caps.cpuRoot + "/" + group + "/cpu.uclamp.min";
    std::string val = utilToCgroupPct(minValue);
    if (!writeFile(path.c_str(), val)) {
        return false;
    }
    int& active = cgActive()[group];
    active++;
    rebuildCgMarkerLocked();
    return true;
}

bool restoreTidFromCgroupUclamp(int tid, const std::string& boostGroup, const std::string& savedGroup) {
    if (boostGroup.empty()) return false;
    aatpe::caps::DeviceCaps caps = aatpe::caps::getCaps();
    bool moved = true;
    if (!savedGroup.empty()) moved = (aatpe::caps::writeTidToCpuCgroupGroup(tid, savedGroup) == 0);
    if (!moved) {
        return false;
    }
    std::lock_guard<std::mutex> lk(cgUclampMutex());
    int& active = cgActive()[boostGroup];
    if (active > 0) active--;
    if (active == 0) {
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

std::string readThreadCpusetGroup(int tid) {
    std::string outBase, outPath, outHier;
    readThreadCgroupInfo(tid, &outBase, &outPath, &outHier);
    return outBase;
}

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
    std::string v1Hier;
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        l.erase(l.find_last_not_of(" \t\r\n") + 1);
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
                        (ctrls.size()>=4 && ctrls.rfind(",cpu")==ctrls.size()-4);
        if (!isCpuset && !isCpuCtl) continue;
        std::string p = l.substr(c2+1);
        while (!p.empty() && p.front()=='/') p.erase(p.begin());
        if (p.empty()) continue;
        size_t slash = p.find_last_of('/');
        std::string base = (slash==std::string::npos)?p:p.substr(slash+1);
        if (isCpuset || v1Hier.empty()) {
            v1Base = base; v1Path = p;
            v1Hier = isCpuset ? "v1-cpuset" : "v1-cpu";
        }
    }
    fclose(f);
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

bool moveTidToCpusetGroup(int tid, const std::string& groupName) {
    std::string tasks = "/dev/cpuset/" + groupName + "/tasks";
    FILE* f = fopen(pfx(tasks).c_str(), "w");
    if (!f) return false;
    if (fprintf(f, "%d\n", tid) < 0) { fclose(f); return false; }
    if (fclose(f) != 0) return false;
    return true;
}

static SchedCaps g_cachedCaps;
static std::mutex g_capsMutex;
static std::atomic<long long> g_lastProbeMs{0};

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
    int s = aatpe_priority::uclampSupportState();
    if (s == 1) c.uclamp = true;
    else if (s == -2) c.uclampPartial = true;

    {
        std::lock_guard<std::mutex> lk(g_capsMutex);
        g_cachedCaps = c;
        g_lastProbeMs.store(now, std::memory_order_release);
    }
    writeFile((std::string(STATE_DIR) + "/scheduler_capabilities").c_str(),
              renderCapabilities(c));
    return c;
}

}
