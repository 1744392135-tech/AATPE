// AATPE - Unified thread recovery record implementation 
#include "aatpe_recovery.h"
#include "aatped.h"          // RUN_DIR, writeFileAtomic
#include "aatped_logging.h"
#include "aatpe_caps.h"      // D1: aatpe::caps::writeTidTo{Cpu,}CgroupGroup (v2 threads/v1 tasks)

#include <cstdio>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/resource.h>  // setpriority / PRIO_PROCESS
#include <unistd.h>
#include <sys/syscall.h>
#include "aatpe_sched_abi.h"  // sched_attr + SCHED_FLAG_*

namespace aatpe_recovery {
namespace {

std::mutex s_recMutex;
std::unordered_map<int, RecoveryRecord> s_records;  // tid -> record

std::string recoveryDir() {
    return std::string(RUN_DIR) + "/recovery";
}

std::string recordPath(int tid) {
    return recoveryDir() + "/" + std::to_string(tid) + ".rec";
}

// D-007: resolve the thread-group id (tgid == process pid) and the kernel
// starttime (clock ticks since boot) for a live tid by reading /proc/<tid>/stat.
//
// /proc/<tid>/stat layout:
//   field1  = pid       (for a task thread this is the TGID, i.e. the real pid)
//   field2  = comm      (parenthesized, may contain spaces)
//   field3..= state, ppid, ..., field22 = starttime (ticks)
// We split at the LAST ')' (comm can itself contain ')'), then skip fields until
// field 22. Returns false if the proc node is gone or unparseable.
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
    // Fields after ')': field3 = state. starttime is field22, i.e. the (22-3)=19th
    // whitespace-separated token AFTER the ')'.
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

// D1: parse placement's persisted `STATE_DIR/promoted_tids` into a map of
// tid -> original cgroup group name.
//
// promoted_tids line format (written by placement.cpp rewritePromotedTidsFile):
//   `tid:starttime:orig_affinity:mechanism`
// where orig_affinity == "grp=<group>" for threads AATPE moved into a cgroup
// (STUNE_CPUSET / CGROUP_UCLAMP); otherwise it is a cpulist ("0-7") or "-".
// We only consume the "grp=" rows. starttime is checked so a recycled tid is
// not handed the group of a dead/different thread.
//
// This is the ONLY way recovery learns the original cgroup group without
// modifying placement.cpp: placement already persisted it; we just read it
// back at boot. Best-effort: any I/O/parse failure yields an empty map, which
// means "no cgroup restore" (graceful fallback to the legacy path).
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
        // split into up to 4 ':'-separated fields.
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
        if (orig.rfind("grp=", 0) != 0) continue;   // cpulist / "-" -> no cgroup
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

// Parse "key=value" lines into the record. Returns false on I/O error.
bool parseRecord(const std::string& path, RecoveryRecord& r) {
    std::ifstream f(path.c_str());
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        // trim
        while (!v.empty() && (v.back()=='\r'||v.back()==' '||v.back()=='\n')) v.pop_back();
        while (!k.empty() && (k.back()=='\r'||k.back()==' ')) k.pop_back();
        try {
            if      (k == "schema_version") { /* validated by caller */ }
            else if (k == "pid")            r.pid = std::stoi(v);
            else if (k == "tid")             r.tid = std::stoi(v);
            else if (k == "starttime")       r.starttime = std::stoull(v);
            else if (k == "orig_affinity")   { r.origAffinity = v; r.haveOrigAffinity = true; }
            else if (k == "applied_affinity") r.appliedAffinity = v;
            // P3-32: range-check stoul before narrowing to uint32_t.
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
            // D1: cgroup membership. Unknown/absent keys on a legacy .rec leave
            // haveOrigCgroup=false (graceful compat -> old restore path).
            else if (k == "orig_cgroup_group") { r.origCgroupGroup = v; r.haveOrigCgroup = true; }
            else if (k == "have_orig_cgroup") {
                // Only honors an explicit 1 if a group was actually recorded.
                if (std::stoi(v) != 0 && !r.origCgroupGroup.empty()) r.haveOrigCgroup = true;
            }
            // H14: full path + hierarchy. Absent on legacy .rec -> empty ->
            // restore falls back to legacy basename dual-write.
            else if (k == "orig_cgroup_path") { r.origCgroupPath = v; if (!v.empty()) r.haveOrigCgroupPath = true; }
            else if (k == "orig_cgroup_hier") { r.origCgroupHier = v; }
            else if (k == "ts")              r.timestamp = std::stoll(v);
        } catch (...) {
            // Malformed field: skip it. A partial record still restores what we have.
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
    // D1: cgroup membership. Always emitted (empty value + have=0 for legacy
    // records that never promoted via a cgroup) so a self-contained .rec carries
    // the original group; an absent block on an OLD .rec parses back as
    // haveOrigCgroup=false (old restore path).
    out += "orig_cgroup_group=" + r.origCgroupGroup + "\n";
    out += "have_orig_cgroup=" + std::string(r.haveOrigCgroup ? "1" : "0") + "\n";
    // H14: full path + hierarchy (optional; empty on legacy records).
    out += "orig_cgroup_path=" + r.origCgroupPath + "\n";
    out += "orig_cgroup_hier=" + r.origCgroupHier + "\n";
    // P3-33: timestamp is refreshed to current time on every commit. This is
    // intentional — the ts field records "last AATPE mutation", not "first
    // snapshot time". Uninstall.sh uses it to detect stale records.
    out += "ts=" + std::to_string(r.timestamp ? r.timestamp : (long long)time(nullptr)) + "\n";
    return out;
}

}  // namespace

void recoveryBootload() {
    // P0 startup-hardening: never let .rec loading throw out to main. A
    // truncated/corrupt .rec from a v4.2.1 install, or an unexpected
    // bad_alloc while iterating, must leave the daemon running with an empty
    // recovery table (no restore = no regression for this boot).
    try {
        std::lock_guard<std::mutex> lk(s_recMutex);
        std::string dir = recoveryDir();
        // P3-31: check mkdir return value (EEXIST is fine, other errors warn).
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
            // Only accept unified .rec files (ignore legacy .uclamp/.affinity which
            // uninstall.sh still reads for backward compat; we do not re-parse them
            // into the cache — they have no schema_version and lack priority).
            if (name.size() < 5 || name.compare(name.size() - 4, 4, ".rec") != 0) continue;
            std::string path = dir + "/" + name;
            RecoveryRecord r;
            // G1-013: the on-disk filename stem must equal the record's embedded tid.
            // uninstall.sh scans by filename; a mismatch would orphan the record.
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
    // D1: backfill cgroup membership from placement's persisted promoted_tids.
    // placement (which we do NOT modify) already records the original cgroup group
    // for every thread it promoted into "top-app". A .rec written by an older build
    // has no orig_cgroup_group key; if promoted_tids still carries a "grp=<group>"
    // row for this tid (crash-restart without a reboot; on a normal reboot
    // service.sh has already swept+cleared promoted_tids so this is a no-op), we
    // attach the group here so the boot restore can move the thread back.
    // starttime must match (TID-reuse guard). Records that already have
    // haveOrigCgroup=true (self-contained .rec from a newer build) are left as-is.
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

// Assume-locked commit core: callers MUST hold s_recMutex. Splitting the
// commit lets the recordAffinity/recordSched read-modify-write run entirely
// under one lock acquisition (no non-recursive mutex re-entry).
static bool commitRecordLocked(const RecoveryRecord& rec) {
    if (rec.tid <= 0) return false;
    // Ensure the recovery dir exists on EVERY commit path. ROUND-003: the
    // recordAffinity/recordSched RMW locked paths call this core directly
    // (bypassing recoveryCommit's old call_once), so the mkdir must live here
    // or a first write before recoveryBootload() fails with ENOENT.
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
    // G1-003 fix: entire read-modify-write under s_recMutex so a concurrent
    // sched commit cannot interleave between our snapshot read and our write.
    std::lock_guard<std::mutex> lk(s_recMutex);
    RecoveryRecord rec;
    bool existed = false;
    auto it = s_records.find(tid);
    if (it != s_records.end()) { rec = it->second; existed = true; }
    if (existed && rec.haveOrigAffinity) {
        // Already snapshotted orig affinity (this run or a prior daemon boot).
        // Do NOT re-snapshot (P0-2). Just refresh the applied list.
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
    // G1-003 fix: entire read-modify-write under s_recMutex.
    std::lock_guard<std::mutex> lk(s_recMutex);
    RecoveryRecord rec;
    bool existed = false;
    auto it = s_records.find(tid);
    if (it != s_records.end()) { rec = it->second; existed = true; }
    if (existed) {
        if (pid > 0) rec.pid = pid;
        // P0-2: if the sched orig was already snapshotted (prior boot or a prior
        // call), do NOT overwrite origPolicy/origPriority/origNice/origMin/Max.
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
        // Applied values always reflect the most recent AATPE mutation.
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

// H14: fill origCgroupPath / origCgroupHier on the existing record. If no record
// exists yet, create a bare one (caller is expected to record affinity/sched
// separately). Empty path -> no-op (leave haveOrigCgroupPath=false -> basename).
//
// D-007: when we have to CREATE that bare record (cgroup observation arrived
// before any affinity/sched snapshot), a default RecoveryRecord has pid=0 and
// starttime=0. On boot restore the live-check builds /proc/<pid>/task/<tid> with
// pid=0 -> /proc/0/task/<tid>, which never exists, so the record is immediately
// erased as a "dead thread" and its cgroup info is lost. Resolve the live
// tgid + starttime from /proc/<tid>/stat here so a bare cgroup-only record still
// survives boot restore and can actually move the thread back. We never
// overwrite a pid/starttime that an affinity/sched snapshot already filled.
bool recoveryRecordCgroupPath(int tid, const std::string& path, const std::string& hier) {
    if (tid <= 0 || path.empty()) return false;
    std::lock_guard<std::mutex> lk(s_recMutex);
    auto it = s_records.find(tid);
    RecoveryRecord rec;
    bool existed = (it != s_records.end());
    if (existed) rec = it->second;
    rec.tid = tid;
    // Fill identity only when missing: a brand-new bare record, or a record that
    // somehow landed with pid==0 (legacy/cgroup-first ordering). An already
    // snapshotted pid/starttime wins and is never clobbered.
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

// ROUND-013: refresh-only APIs. Update ONLY applied fields; orig never touched.
// No-op (return false) if no record exists for the tid.
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

// erase a thread's recovery record after it exits.
// Drops the in-memory entry and unlinks the on-disk .rec file so dead tids
// don't accumulate forever. Called from thread_scan gone-thread branch and
// eBPF onExit. The .rec file is best-effort unlink (ENOENT is fine).
void recoveryErase(int tid) {
    if (tid <= 0) return;
    // G1-004: hold s_recMutex across BOTH the map erase and the disk unlink.
    // Previously the map was dropped, the lock released, and the unlink ran
    // unlocked — a concurrent recoveryCommit() for a recycled tid could write
    // the new record in that window, only to have us unlink it.
    std::lock_guard<std::mutex> lk(s_recMutex);
    s_records.erase(tid);
    std::string path = recordPath(tid);
    // Best-effort unlink; ignore ENOENT (file already gone).
    ::unlink(path.c_str());
}

// ROUND-014 P3: boot-time sched restore. Called once after recoveryBootload().
// Iterates all loaded records; for each live thread with matching starttime,
// restores origPolicy/origPriority/origNice/origUclampMin/Max.
// Dead threads and TID-recycled entries are erased.
// D-P3-2: only restore if live sched policy matches applied (AATPE still last modifier).
// After successful restore, erase record + unlink .rec.
//
// WD-08: this function previously held s_recMutex across the ENTIRE per-record
// slow path (access(/proc/...), ifstream parse of /proc/<tid>/stat, sched_getscheduler,
// sched_getattr, sched_setscheduler, setpriority, sched_setattr, and two cgroup
// node writes). On a boot with hundreds of promoted threads that blocked every
// concurrent recoveryCommit/record call for seconds. We now follow the
// pruneDeadRtTids() two-phase short-lock pattern:
//   Phase 1 (short lock): snapshot s_records into a local vector.
//   Phase 2 (no lock): do ALL slow probes + restore syscalls for every record.
//                      Produce an action per tid (ERASE / KEEP).
//   Phase 3 (short lock): apply the ERASE decisions, but ONLY when the in-memory
//                      record's starttime still matches the snapshot — a
//                      concurrent recoveryCommit() may have written a fresh
//                      record for this tid mid-phase-2, and we must not erase it.
void recoveryRestoreSchedOnBoot() {
    // Phase 1: snapshot under a short lock.
    struct SnapshotEntry { int tid; RecoveryRecord rec; };
    std::vector<SnapshotEntry> snap;
    {
        std::lock_guard<std::mutex> lk(s_recMutex);
        snap.reserve(s_records.size());
        for (const auto& kv : s_records) snap.push_back({kv.first, kv.second});
    }

    // Per-tid outcome from phase 2.
    enum class Act { ERASE_OK, ERASE_GONE, KEEP_PARTIAL };
    struct Outcome { int tid; unsigned long long starttime; Act act; };
    std::vector<Outcome> outcomes;
    outcomes.reserve(snap.size());

    // Phase 2: slow work, no lock.
    for (const auto& e : snap) {
        const int tid = e.tid;
        const RecoveryRecord& rec = e.rec;

        // 1. Check if tid is alive (proc entry exists)
        std::string taskPath = "/proc/" + std::to_string(rec.pid) + "/task/" + std::to_string(tid);
        if (::access(taskPath.c_str(), F_OK) != 0) {
            // Thread dead - erase record
            outcomes.push_back({tid, rec.starttime, Act::ERASE_GONE});
            continue;
        }

        // 2. Read starttime from /proc/<tid>/stat (field 22, after last ')')
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
                    // A4: /proc/<pid>/stat is parsed from a live, externally-owned
                    // file. A malformed/non-numeric starttime token makes std::stoull
                    // throw (invalid_argument / out_of_range), which would crash the
                    // recovery thread. Fail-safe: on any parse error treat the record
                    // as unverifiable (starttimeMatch stays false -> erased below).
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
            // TID reused or can't verify - erase record, don't restore
            outcomes.push_back({tid, rec.starttime, Act::ERASE_GONE});
            continue;
        }

        // 3. D-P3-2: check live == applied. Read current sched policy.
        // If live policy != appliedPolicy, the app or something else changed it
        // after AATPE set it — don't touch, discard record.
        // D-P3-3: also check uclamp min/max via sched_getattr.
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
            // P2-1 fix: sched_getattr(pid, attr, size, flags) — the 3rd arg is the
            // size of the attr buffer the kernel may fill. Passing 0 made the syscall
            // return -EINVAL without copying anything, so rc was always != 0 and we
            // fell into the conservative "external takeover" branch below, discarding
            // the recovery record for EVERY long-lived uclamp-boosted thread on boot
            // (restore leak). Mirror the correct call in aatped_priority.cpp:224.
            long rc = syscall(SYS_sched_getattr, tid, &liveAttr, sizeof(liveAttr), 0);
            if (rc == 0) {
                if (liveAttr.sched_util_min != rec.appliedUclampMin ||
                    liveAttr.sched_util_max != rec.appliedUclampMax) {
                    liveMatchesApplied = false;
                }
            }
            // If getattr fails, we can't verify — be conservative and don't restore.
            else {
                liveMatchesApplied = false;
            }
        }
#endif

        if (!liveMatchesApplied) {
            // #5: distinguish "someone else took it over" from "we already partially
            // restored on a prior boot and the restore failed mid-way". If the live
            // policy already equals origPolicy (a restore pulled sched back last time
            // but a later dimension failed), this is OUR retry — proceed and let the
            // transactional block below re-assert orig and retry the failed part.
            bool partialRestoreRetry = false;
            if (rec.haveOrigSched) {
                int livePolicy = sched_getscheduler(tid);
                if (livePolicy == (int)rec.origPolicy &&
                    (int)rec.origPolicy != (int)rec.appliedPolicy) {
                    partialRestoreRetry = true;
                }
            }
            if (!partialRestoreRetry) {
                // External takeover — don't touch, discard record.
                outcomes.push_back({tid, rec.starttime, Act::ERASE_GONE});
                continue;
            }
            LOG_INFO("recovery: tid=%d live!=applied but matches orig — retrying partial restore", tid);
        }

        // 4. #5: transactional restore — collect every return value; erase the
        // record ONLY when ALL requested restores succeeded. A partial restore
        // leaves the .rec in place (and the in-memory record) so the next boot
        // retries the failed dimension instead of silently dropping it.
        int restoreRc = 0;
        if (rec.haveOrigSched) {
            struct sched_param sp = {};
            sp.sched_priority = rec.origPriority;
            if (sched_setscheduler(tid, rec.origPolicy, &sp) != 0) restoreRc |= 1;
            // D-001: setpriority() returns the PREVIOUS nice value, which can
            // legitimately be -1 (nice == -1 is a valid value). The only way to
            // distinguish "returned -1 because the prior nice was -1" from
            // "returned -1 because of an error" is to clear errno BEFORE the
            // call and then require (rc == -1 && errno != 0). Previously errno was
            // left stale from the sched_setscheduler() above, so a thread whose
            // origNice was legitimately -1 was misjudged as a failed restore ->
            // restoreRc |= 2 -> the .rec was kept and re-restored on every boot.
            errno = 0;
            if (setpriority(PRIO_PROCESS, tid, rec.origNice) == -1 && errno != 0) restoreRc |= 2;
        }
        if (rec.haveOrigUclamp) {
#ifdef __ANDROID__
            struct sched_attr attr = {};
            attr.size = sizeof(attr);
            // P1 round7: must set UTIL_CLAMP_MIN|MAX flags else the kernel ignores
            // the util fields (mirror priority.cpp). If the call still fails, do NOT
            // return 0 (which would delete the .rec before state was restored).
            attr.sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS |
                               SCHED_FLAG_UTIL_CLAMP_MIN | SCHED_FLAG_UTIL_CLAMP_MAX;
            attr.sched_util_min = rec.origUclampMin;
            attr.sched_util_max = rec.origUclampMax;
            if (syscall(SYS_sched_setattr, tid, &attr, 0) != 0) restoreRc |= 4;
#endif
        }
        // D1: cgroup membership restore. Move the thread back to its ORIGINAL
        // cgroup group. Reuse the caps migration functions (NO raw node writes):
        //   - writeTidToCgroupGroup()     -> cpuset root (v1 tasks / v2 cgroup.threads)
        //   - writeTidToCpuCgroupGroup()  -> cpu root  (v1 tasks / v2 cgroup.threads)
        // H14: if we recorded a full path (nested cgroup), use it as the group
        // argument — caps::resolveCgroupThreadPath joins root + group, so a
        // relative path "a/b/foreground" resolves to the exact nested node. If
        // no full path (legacy .rec), fall back to basename dual-write.
        if (rec.haveOrigCgroup && !rec.origCgroupGroup.empty()) {
            std::string restoreGroup = rec.origCgroupGroup;
            if (rec.haveOrigCgroupPath && !rec.origCgroupPath.empty()) {
                restoreGroup = rec.origCgroupPath;
            }
            int rcCpuset = aatpe::caps::writeTidToCgroupGroup(tid, restoreGroup);
            int rcCpu    = aatpe::caps::writeTidToCpuCgroupGroup(tid, restoreGroup);
            // WD-09: cpuset and cpu cgroup writes are INDEPENDENT. Previously
            // only the BOTH-failed case set restoreRc|=8, so a half-success
            // (cpuset restored, cpu failed) was treated as a full success and
            // the .rec was erased — leaving the thread in a half-restored cgroup
            // state that would never be retried. Each failure now sets its own
            // bit: bit 8 = cpuset failed, bit 16 = cpu failed. Any non-zero
            // result keeps the .rec for next-boot retry.
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

        // 5. #5: all succeeded -> drop the record + unlink .rec. Any failure keeps
        // the record for a retry next boot (do NOT erase/unlink on partial restore).
        if (restoreRc == 0) {
            outcomes.push_back({tid, rec.starttime, Act::ERASE_OK});
        } else {
            LOG_WARN("recovery: tid=%d restore PARTIAL rc=0x%x — keeping .rec for retry",
                     tid, restoreRc);
            outcomes.push_back({tid, rec.starttime, Act::KEEP_PARTIAL});
        }
    }

    // Phase 3: short lock to apply erases. Guard against a concurrent
    // recoveryCommit() that may have inserted a FRESH record for this tid
    // while we were doing slow syscalls: only erase if the in-memory record's
    // starttime still matches what we snapshotted.
    {
        std::lock_guard<std::mutex> lk(s_recMutex);
        for (const auto& o : outcomes) {
            if (o.act == Act::KEEP_PARTIAL) continue;
            auto it = s_records.find(o.tid);
            if (it == s_records.end()) continue;  // already erased by someone else
            if (it->second.starttime != o.starttime) continue;  // fresh record — don't clobber
            s_records.erase(it);
            ::unlink(recordPath(o.tid).c_str());
        }
    }
}

// D-P3-2: clear all persisted records on clean shutdown.
// Called from daemon shutdown sequence so boot restore only handles crash recovery.
void recoveryClearAllPersisted() {
    std::lock_guard<std::mutex> lk(s_recMutex);
    for (auto& [tid, rec] : s_records) {
        ::unlink(recordPath(tid).c_str());
    }
    s_records.clear();
}

}  // namespace aatpe_recovery
