#include "aatpe_recovery.h"
#include "aatped.h"
#include "aatped_logging.h"
#include "aatpe_caps.h"

#include <cstdio>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "aatpe_sched_abi.h"

namespace aatpe_recovery {
namespace {

std::mutex s_recMutex;
std::unordered_map<int, RecoveryRecord> s_records;

std::string recoveryDir() {
    return std::string(RUN_DIR) + "/recovery";
}

std::string recordPath(int tid) {
    return recoveryDir() + "/" + std::to_string(tid) + ".rec";
}

static bool procTidIdentity(int tid, int& tgidOut, unsigned long long& starttimeOut) {
    tgidOut = 0; starttimeOut = 0;
    std::string path = "/proc/" + std::to_string(tid) + "/stat";
    std::ifstream f(path.c_str());
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    size_t firstSpace = line.find(' ');
    size_t lastParen = line.rfind(')');
    if (firstSpace == std::string::npos || lastParen == std::string::npos || lastParen + 2 >= line.size()) {
        return false;
    }
    try {
        tgidOut = std::stoi(line.substr(0, firstSpace));
    } catch (...) { tgidOut = 0; }
    std::istringstream iss(line.substr(lastParen + 2));
    std::string tok;
    for (int i = 0; i < 20; i++) {
        if (!(iss >> tok)) return false;
    }
    if (tok.empty()) return false;
    try {
        starttimeOut = std::stoull(tok);
    } catch (...) { return false; }
    return tgidOut > 0 && starttimeOut > 0;
}

static std::unordered_map<int, std::pair<unsigned long long,std::string>>
readPromotedTidsCgroups(void) {
    std::unordered_map<int, std::pair<unsigned long long,std::string>> out;
    std::string path = std::string(STATE_DIR) + "/promoted_tids";
    std::ifstream f(path.c_str());
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back()=='\r'||line.back()=='\n')) line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> fld;
        size_t pos = 0;
        for (int i = 0; i < 4; i++) {
            size_t c = line.find(':', pos);
            if (c == std::string::npos) { fld.push_back(line.substr(pos)); break; }
            fld.push_back(line.substr(pos, c - pos));
            pos = c + 1;
        }
        if (fld.size() < 3) continue;
        const std::string& tidStr = fld[0];
        const std::string& stStr  = fld[1];
        const std::string& orig   = fld[2];
        if (orig.rfind("grp=", 0) != 0) continue;
        std::string group = orig.substr(4);
        if (group.empty()) continue;
        try {
            int tid = std::stoi(tidStr);
            unsigned long long st = stStr.empty() ? 0 : std::stoull(stStr);
            if (tid > 0) out[tid] = {st, group};
        } catch (...) {
            LOG_DEBUG("recovery: promoted_tids malformed row '%s:%s:%s' — skipping",
                      tidStr.c_str(), stStr.c_str(), group.c_str());
        }
    }
    return out;
}

bool parseRecord(const std::string& path, RecoveryRecord& r) {
    std::ifstream f(path.c_str());
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.back()=='\r'||v.back()==' '||v.back()=='\n')) v.pop_back();
        while (!k.empty() && (k.back()=='\r'||k.back()==' ')) k.pop_back();
        try {
            if      (k == "schema_version") {  }
            else if (k == "pid")            r.pid = std::stoi(v);
            else if (k == "tid")             r.tid = std::stoi(v);
            else if (k == "starttime")       r.starttime = std::stoull(v);
            else if (k == "orig_affinity")   { r.origAffinity = v; r.haveOrigAffinity = true; }
            else if (k == "applied_affinity") r.appliedAffinity = v;
            else if (k == "orig_policy")     { unsigned long ul = std::stoul(v); if (ul > 0xFFFFFFFFu) ul = 0xFFFFFFFFu; r.origPolicy = (uint32_t)ul; }
            else if (k == "applied_policy")  { unsigned long ul = std::stoul(v); if (ul > 0xFFFFFFFFu) ul = 0xFFFFFFFFu; r.appliedPolicy = (uint32_t)ul; }
            else if (k == "orig_priority")   { unsigned long ul = std::stoul(v); if (ul > 0xFFFFFFFFu) ul = 0xFFFFFFFFu; r.origPriority = (uint32_t)ul; }
            else if (k == "applied_priority") { unsigned long ul = std::stoul(v); if (ul > 0xFFFFFFFFu) ul = 0xFFFFFFFFu; r.appliedPriority = (uint32_t)ul; }
            else if (k == "orig_nice")       r.origNice = std::stoi(v);
            else if (k == "applied_nice")    r.appliedNice = std::stoi(v);
            else if (k == "orig_min")        { unsigned long ul = std::stoul(v); if (ul > 0xFFFFFFFFu) ul = 0xFFFFFFFFu; r.origUclampMin = (uint32_t)ul; }
            else if (k == "applied_min")      { unsigned long ul = std::stoul(v); if (ul > 0xFFFFFFFFu) ul = 0xFFFFFFFFu; r.appliedUclampMin = (uint32_t)ul; }
            else if (k == "orig_max")        { unsigned long ul = std::stoul(v); if (ul > 0xFFFFFFFFu) ul = 0xFFFFFFFFu; r.origUclampMax = (uint32_t)ul; }
            else if (k == "applied_max")     { unsigned long ul = std::stoul(v); if (ul > 0xFFFFFFFFu) ul = 0xFFFFFFFFu; r.appliedUclampMax = (uint32_t)ul; }
            else if (k == "have_orig_sched") r.haveOrigSched = (std::stoi(v) != 0);
            else if (k == "have_orig_uclamp") r.haveOrigUclamp = (std::stoi(v) != 0);
            else if (k == "orig_cgroup_group") { r.origCgroupGroup = v; r.haveOrigCgroup = true; }
            else if (k == "have_orig_cgroup") {
                if (std::stoi(v) != 0 && !r.origCgroupGroup.empty()) r.haveOrigCgroup = true;
            }
            else if (k == "orig_cgroup_path") { r.origCgroupPath = v; if (!v.empty()) r.haveOrigCgroupPath = true; }
            else if (k == "orig_cgroup_hier") { r.origCgroupHier = v; }
            else if (k == "ts")              r.timestamp = std::stoll(v);
        } catch (...) {
            LOG_DEBUG("recovery: malformed field '%s=%s' in .rec — skipping", k.c_str(), v.c_str());
        }
    }
    return true;
}

std::string serializeRecord(const RecoveryRecord& r) {
    std::string out;
    out += "schema_version=" + std::to_string(RECOVERY_SCHEMA_VERSION) + "\n";
    out += "pid="  + std::to_string(r.pid) + "\n";
    out += "tid="  + std::to_string(r.tid) + "\n";
    out += "starttime=" + std::to_string(r.starttime) + "\n";
    out += "orig_affinity="   + r.origAffinity   + "\n";
    out += "applied_affinity=" + r.appliedAffinity + "\n";
    out += "orig_policy="    + std::to_string(r.origPolicy)    + "\n";
    out += "applied_policy=" + std::to_string(r.appliedPolicy)  + "\n";
    out += "orig_priority="  + std::to_string(r.origPriority)   + "\n";
    out += "applied_priority=" + std::to_string(r.appliedPriority) + "\n";
    out += "orig_nice="      + std::to_string(r.origNice)      + "\n";
    out += "applied_nice="   + std::to_string(r.appliedNice)    + "\n";
    out += "orig_min="       + std::to_string(r.origUclampMin)   + "\n";
    out += "applied_min="    + std::to_string(r.appliedUclampMin) + "\n";
    out += "orig_max="       + std::to_string(r.origUclampMax)   + "\n";
    out += "applied_max="    + std::to_string(r.appliedUclampMax) + "\n";
    out += "have_orig_sched="  + std::string(r.haveOrigSched ? "1" : "0") + "\n";
    out += "have_orig_uclamp=" + std::string(r.haveOrigUclamp ? "1" : "0") + "\n";
    out += "orig_cgroup_group=" + r.origCgroupGroup + "\n";
    out += "have_orig_cgroup=" + std::string(r.haveOrigCgroup ? "1" : "0") + "\n";
    out += "orig_cgroup_path=" + r.origCgroupPath + "\n";
    out += "orig_cgroup_hier=" + r.origCgroupHier + "\n";
    out += "ts=" + std::to_string(r.timestamp ? r.timestamp : (long long)time(nullptr)) + "\n";
    return out;
}

}

void recoveryBootload() {
    try {
        std::lock_guard<std::mutex> lk(s_recMutex);
        std::string dir = recoveryDir();
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            int e = errno; char ebuf[128];
            LOG_WARN("recovery: mkdir %s failed: %s", dir.c_str(), aatpeStrError(e, ebuf, sizeof(ebuf)));
        }
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        struct dirent* ent;
        int loaded = 0;
        while ((ent = readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            if (name.size() < 5 || name.compare(name.size() - 4, 4, ".rec") != 0) continue;
            std::string path = dir + "/" + name;
            RecoveryRecord r;
            bool tidMatchesFile = false;
            {
                std::string stem = name.substr(0, name.size() - 4);
                char* endp = nullptr;
                long stemTid = std::strtol(stem.c_str(), &endp, 10);
                tidMatchesFile = (endp && *endp == '\0' && stemTid > 0);
            }
            if (parseRecord(path, r) && r.tid > 0 && r.starttime != 0 && tidMatchesFile
                && std::to_string(r.tid) == name.substr(0, name.size() - 4)) {
                s_records[r.tid] = r;
                ++loaded;
            }
        }
        closedir(d);
    {
        auto prom = readPromotedTidsCgroups();
        int backfilled = 0;
        for (auto& [tid, rec] : s_records) {
            if (rec.haveOrigCgroup) continue;
            auto it = prom.find(tid);
            if (it == prom.end()) continue;
            if (it->second.first != 0 && rec.starttime != it->second.first) continue;
            rec.origCgroupGroup = it->second.second;
            rec.haveOrigCgroup = true;
            ++backfilled;
        }
        if (backfilled > 0) {
            LOG_INFO("recovery: D1 backfilled cgroup group for %d record(s) from promoted_tids",
                     backfilled);
        }
    }
    if (loaded > 0) {
        LOG_INFO("recovery: bootloaded %d existing record(s) from %s (restart guard active)",
                 loaded, dir.c_str());
    }
    } catch (const std::exception& e) {
        LOG_ERROR("recoveryBootload() threw '%s' — starting with empty recovery table", e.what());
    } catch (...) {
        LOG_ERROR("recoveryBootload() threw unknown exception — starting with empty recovery table");
    }
}

bool recoveryIsRecorded(int tid) {
    std::lock_guard<std::mutex> lk(s_recMutex);
    return s_records.find(tid) != s_records.end();
}

bool recoveryGet(int tid, RecoveryRecord& out) {
    std::lock_guard<std::mutex> lk(s_recMutex);
    auto it = s_records.find(tid);
    if (it == s_records.end()) return false;
    out = it->second;
    return true;
}

static bool commitRecordLocked(const RecoveryRecord& rec) {
    if (rec.tid <= 0) return false;
    static std::once_flag s_dirOnce;
    std::call_once(s_dirOnce, []() {
        if (::mkdir(recoveryDir().c_str(), 0700) != 0 && errno != EEXIST) {
            int e = errno; char ebuf[128];
            LOG_WARN("recovery: mkdir %s failed: %s", recoveryDir().c_str(), aatpeStrError(e, ebuf, sizeof(ebuf)));
        }
    });
    std::string path = recordPath(rec.tid);
    std::string content = serializeRecord(rec);
    if (!writeFileAtomic(path.c_str(), content)) {
        LOG_WARN("recovery: atomic write FAILED for tid=%d (will retry next cycle)", rec.tid);
        return false;
    }
    s_records[rec.tid] = rec;
    return true;
}

bool recoveryCommit(const RecoveryRecord& rec) {
    if (rec.tid <= 0) return false;
    std::lock_guard<std::mutex> lk(s_recMutex);
    return commitRecordLocked(rec);
}

bool recoveryRecordAffinity(int pid, int tid, unsigned long long starttime,
                            const std::string& origList,
                            const std::string& appliedList) {
    if (tid <= 0) return false;
    std::lock_guard<std::mutex> lk(s_recMutex);
    RecoveryRecord rec;
    bool existed = false;
    auto it = s_records.find(tid);
    if (it != s_records.end()) { rec = it->second; existed = true; }
    if (existed && rec.haveOrigAffinity) {
        rec.appliedAffinity = appliedList;
    } else {
        if (!existed) {
            rec.tid = tid;
            rec.starttime = starttime;
        }
        rec.pid = pid;
        rec.origAffinity   = origList;
        rec.appliedAffinity = appliedList;
        rec.haveOrigAffinity = true;
    }
    return commitRecordLocked(rec);
}

bool recoveryRecordSched(int pid, int tid, unsigned long long starttime,
                         uint32_t origPolicy, uint32_t origPriority, int origNice,
                         uint32_t origMin, uint32_t origMax,
                         uint32_t appliedPolicy, uint32_t appliedPriority, int appliedNice,
                         uint32_t appliedMin, uint32_t appliedMax) {
    if (tid <= 0) return false;
    std::lock_guard<std::mutex> lk(s_recMutex);
    RecoveryRecord rec;
    bool existed = false;
    auto it = s_records.find(tid);
    if (it != s_records.end()) { rec = it->second; existed = true; }
    if (existed) {
        if (pid > 0) rec.pid = pid;
        if (!rec.haveOrigSched) {
            rec.origPolicy   = origPolicy;
            rec.origPriority = origPriority;
            rec.origNice     = origNice;
            rec.haveOrigSched = true;
        }
        if (!rec.haveOrigUclamp) {
            rec.origUclampMin = origMin;
            rec.origUclampMax = origMax;
            rec.haveOrigUclamp = true;
        }
        rec.appliedPolicy   = appliedPolicy;
        rec.appliedPriority = appliedPriority;
        rec.appliedNice     = appliedNice;
        rec.appliedUclampMin = appliedMin;
        rec.appliedUclampMax = appliedMax;
    } else {
        rec.tid = tid;
        rec.pid = pid;
        rec.starttime = starttime;
        rec.origPolicy   = origPolicy;
        rec.origPriority = origPriority;
        rec.origNice     = origNice;
        rec.origUclampMin = origMin;
        rec.origUclampMax = origMax;
        rec.appliedPolicy   = appliedPolicy;
        rec.appliedPriority = appliedPriority;
        rec.appliedNice     = appliedNice;
        rec.appliedUclampMin = appliedMin;
        rec.appliedUclampMax = appliedMax;
        rec.haveOrigSched = true;
        rec.haveOrigUclamp = true;
    }
    return commitRecordLocked(rec);
}

bool recoveryRecordCgroupPath(int tid, const std::string& path, const std::string& hier) {
    if (tid <= 0 || path.empty()) return false;
    std::lock_guard<std::mutex> lk(s_recMutex);
    auto it = s_records.find(tid);
    RecoveryRecord rec;
    bool existed = (it != s_records.end());
    if (existed) rec = it->second;
    rec.tid = tid;
    if (rec.pid <= 0 || rec.starttime == 0) {
        int tgid = 0; unsigned long long liveStart = 0;
        if (procTidIdentity(tid, tgid, liveStart)) {
            if (rec.pid <= 0) rec.pid = tgid;
            if (rec.starttime == 0) rec.starttime = liveStart;
        }
    }
    rec.origCgroupPath = path;
    rec.origCgroupHier = hier;
    rec.haveOrigCgroupPath = true;
    return commitRecordLocked(rec);
}

bool recoveryRefreshAppliedAffinity(int tid, const std::string& appliedList) {
    if (tid <= 0) return false;
    std::lock_guard<std::mutex> lk(s_recMutex);
    auto it = s_records.find(tid);
    if (it == s_records.end()) return false;
    it->second.appliedAffinity = appliedList;
    return commitRecordLocked(it->second);
}

bool recoveryRefreshAppliedSched(int tid, uint32_t appliedPolicy, uint32_t appliedPriority,
                                int appliedNice, uint32_t appliedMin, uint32_t appliedMax) {
    if (tid <= 0) return false;
    std::lock_guard<std::mutex> lk(s_recMutex);
    auto it = s_records.find(tid);
    if (it == s_records.end()) return false;
    it->second.appliedPolicy   = appliedPolicy;
    it->second.appliedPriority = appliedPriority;
    it->second.appliedNice     = appliedNice;
    it->second.appliedUclampMin = appliedMin;
    it->second.appliedUclampMax = appliedMax;
    return commitRecordLocked(it->second);
}

size_t recoveryCount() {
    std::lock_guard<std::mutex> lk(s_recMutex);
    return s_records.size();
}

void recoveryErase(int tid) {
    if (tid <= 0) return;
    std::lock_guard<std::mutex> lk(s_recMutex);
    s_records.erase(tid);
    std::string path = recordPath(tid);
    ::unlink(path.c_str());
}

void recoveryRestoreSchedOnBoot() {
    struct SnapshotEntry { int tid; RecoveryRecord rec; };
    std::vector<SnapshotEntry> snap;
    {
        std::lock_guard<std::mutex> lk(s_recMutex);
        snap.reserve(s_records.size());
        for (const auto& kv : s_records) snap.push_back({kv.first, kv.second});
    }

    enum class Act { ERASE_OK, ERASE_GONE, KEEP_PARTIAL };
    struct Outcome { int tid; unsigned long long starttime; Act act; };
    std::vector<Outcome> outcomes;
    outcomes.reserve(snap.size());

    for (const auto& e : snap) {
        const int tid = e.tid;
        const RecoveryRecord& rec = e.rec;

        std::string taskPath = "/proc/" + std::to_string(rec.pid) + "/task/" + std::to_string(tid);
        if (::access(taskPath.c_str(), F_OK) != 0) {
            outcomes.push_back({tid, rec.starttime, Act::ERASE_GONE});
            continue;
        }

        std::string statPath = taskPath + "/stat";
        std::ifstream f(statPath.c_str());
        bool starttimeMatch = false;
        if (f) {
            std::string line;
            std::getline(f, line);
            size_t lastParen = line.rfind(')');
            if (lastParen != std::string::npos && lastParen + 2 < line.size()) {
                std::istringstream iss(line.substr(lastParen + 2));
                std::string tmp;
                for (int i = 0; i < 20; i++) {
                    if (!(iss >> tmp)) break;
                }
                if (!tmp.empty()) {
                    try {
                        unsigned long long liveStart = std::stoull(tmp);
                        starttimeMatch = (liveStart == rec.starttime);
                    } catch (...) {
                        starttimeMatch = false;
                    }
                }
            }
        }

        if (!starttimeMatch) {
            outcomes.push_back({tid, rec.starttime, Act::ERASE_GONE});
            continue;
        }

        bool liveMatchesApplied = true;
        if (rec.haveOrigSched) {
            int livePolicy = sched_getscheduler(tid);
            if (livePolicy != (int)rec.appliedPolicy) {
                liveMatchesApplied = false;
            }
        }
#ifdef __ANDROID__
        if (liveMatchesApplied && rec.haveOrigUclamp) {
            struct sched_attr liveAttr = {};
            liveAttr.size = sizeof(liveAttr);
            long rc = syscall(SYS_sched_getattr, tid, &liveAttr, sizeof(liveAttr), 0);
            if (rc == 0) {
                if (liveAttr.sched_util_min != rec.appliedUclampMin ||
                    liveAttr.sched_util_max != rec.appliedUclampMax) {
                    liveMatchesApplied = false;
                }
            }
            else {
                liveMatchesApplied = false;
            }
        }
#endif

        if (!liveMatchesApplied) {
            bool partialRestoreRetry = false;
            if (rec.haveOrigSched) {
                int livePolicy = sched_getscheduler(tid);
                if (livePolicy == (int)rec.origPolicy &&
                    (int)rec.origPolicy != (int)rec.appliedPolicy) {
                    partialRestoreRetry = true;
                }
            }
            if (!partialRestoreRetry) {
                outcomes.push_back({tid, rec.starttime, Act::ERASE_GONE});
                continue;
            }
            LOG_INFO("recovery: tid=%d live!=applied but matches orig — retrying partial restore", tid);
        }

        int restoreRc = 0;
        if (rec.haveOrigSched) {
            struct sched_param sp = {};
            sp.sched_priority = rec.origPriority;
            if (sched_setscheduler(tid, rec.origPolicy, &sp) != 0) restoreRc |= 1;
            errno = 0;
            if (setpriority(PRIO_PROCESS, tid, rec.origNice) == -1 && errno != 0) restoreRc |= 2;
        }
        if (rec.haveOrigUclamp) {
#ifdef __ANDROID__
            struct sched_attr attr = {};
            attr.size = sizeof(attr);
            attr.sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS |
                               SCHED_FLAG_UTIL_CLAMP_MIN | SCHED_FLAG_UTIL_CLAMP_MAX;
            attr.sched_util_min = rec.origUclampMin;
            attr.sched_util_max = rec.origUclampMax;
            if (syscall(SYS_sched_setattr, tid, &attr, 0) != 0) restoreRc |= 4;
#endif
        }
        if (rec.haveOrigCgroup && !rec.origCgroupGroup.empty()) {
            std::string restoreGroup = rec.origCgroupGroup;
            if (rec.haveOrigCgroupPath && !rec.origCgroupPath.empty()) {
                restoreGroup = rec.origCgroupPath;
            }
            int rcCpuset = aatpe::caps::writeTidToCgroupGroup(tid, restoreGroup);
            int rcCpu    = aatpe::caps::writeTidToCpuCgroupGroup(tid, restoreGroup);
            if (rcCpuset != 0) {
                restoreRc |= 8;
                LOG_WARN("recovery: tid=%d cpuset cgroup restore to '%s' failed (rc=%d) — retry next boot",
                         tid, restoreGroup.c_str(), rcCpuset);
            }
            if (rcCpu != 0) {
                restoreRc |= 16;
                LOG_WARN("recovery: tid=%d cpu cgroup restore to '%s' failed (rc=%d) — retry next boot",
                         tid, restoreGroup.c_str(), rcCpu);
            }
        }

        if (restoreRc == 0) {
            outcomes.push_back({tid, rec.starttime, Act::ERASE_OK});
        } else {
            LOG_WARN("recovery: tid=%d restore PARTIAL rc=0x%x — keeping .rec for retry",
                     tid, restoreRc);
            outcomes.push_back({tid, rec.starttime, Act::KEEP_PARTIAL});
        }
    }

    {
        std::lock_guard<std::mutex> lk(s_recMutex);
        for (const auto& o : outcomes) {
            if (o.act == Act::KEEP_PARTIAL) continue;
            auto it = s_records.find(o.tid);
            if (it == s_records.end()) continue;
            if (it->second.starttime != o.starttime) continue;
            s_records.erase(it);
            ::unlink(recordPath(o.tid).c_str());
        }
    }
}

void recoveryClearAllPersisted() {
    std::lock_guard<std::mutex> lk(s_recMutex);
    for (auto& [tid, rec] : s_records) {
        ::unlink(recordPath(tid).c_str());
    }
    s_records.clear();
}

}
