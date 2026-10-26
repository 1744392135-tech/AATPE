// AATPE - Unified thread recovery record 
// ============================================================================
// Replaces the two ad-hoc record writers (placement.cpp saveRecoveryAffinity
// -> <tid>.affinity, aatped_priority.cpp saveRecoveryRecord -> <tid>.uclamp)
// with ONE per-tid record that captures every dimension AATPE may touch:
//   affinity / sched policy / sched priority / nice / uclamp min / uclamp max.
//
// Bugs fixed in this module:
//   P0-2: recovery is PERSISTENT. At daemon start recoveryBootload() reads the
//         existing recovery/<tid>.rec files back into the in-memory tracking set.
//         A record that already exists (from a previous daemon incarnation) is
//         NEVER overwritten with a fresh "original" snapshot — otherwise a
//         restarted daemon would mistake the already-AATPE-modified live state
//         for the thread's true original state and uninstall could not restore.
//   P0-4: recoveryWrite() returns bool. The in-memory "recorded" flag is set
//         ONLY after the atomic write (tmp + fsync + rename) succeeds. A failed
//         write leaves the tid unrecorded so the next scan retries.
//   P0-6: orig_sched_priority / applied_sched_priority are now captured, so a
//         thread that was SCHED_RR prio=10 and AATPE set to prio=1 is restored
//         to prio=10 (previously chrt hardcoded prio=1 on restore).
//   P1-16: one unified RecoveryRecord for all dimensions.
//   P1-17: every write is atomic (writeFileAtomic: .tmp -> fsync -> rename ->
//          dir-fsync) and carries schema_version=2 for future migration.
//
// Thread identity: records are keyed by tid AND the kernel starttime (stat
// field 22). A recycled tid with a different starttime is rejected on restore.
// ============================================================================
#ifndef AATPE_RECOVERY_H
#define AATPE_RECOVERY_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace aatpe_recovery {

// On-disk schema version. Bump when the record format changes in a way that
// old uninstall.sh readers must not mis-parse.
constexpr int RECOVERY_SCHEMA_VERSION = 2;

struct RecoveryRecord {
    int pid = 0;
    int tid = 0;
    unsigned long long starttime = 0;

    // affinity (cpu list string, e.g. "4-7"). haveOrigAffinity guards re-snapshot.
    std::string origAffinity;
    std::string appliedAffinity;
    bool haveOrigAffinity = false;

    // sched policy / static priority / nice.
    uint32_t origPolicy = 0;     uint32_t appliedPolicy = 0;
    uint32_t origPriority = 0;  uint32_t appliedPriority = 0;
    int      origNice = 0;       int      appliedNice = 0;
    bool haveOrigSched = false;

    // uclamp util min/max.
    uint32_t origUclampMin = 0;  uint32_t appliedUclampMin = 0;
    uint32_t origUclampMax = 0;   uint32_t appliedUclampMax = 0;
    bool haveOrigUclamp = false;

    // D1 (v4.2): original cgroup membership. When AATPE promoted a thread into the
    // "top-app" cpu/cpuset cgroup (STUNE_CPUSET / CGROUP_UCLAMP mechanisms), the
    // thread's ORIGINAL cgroup group (e.g. "foreground", "background",
    // "system-background") must be restored on crash-recovery, otherwise a killed
    // daemon leaves the thread pinned in top-app forever. The group name is
    // consumed from placement's already-persisted `STATE_DIR/promoted_tids`
    // (field 3 == "grp=<group>") during recoveryBootload(); placement.cpp itself
    // is NOT modified. haveOrigCgroup guards the field: a .rec written by an
    // older build (no orig_cgroup_group key, no promoted_tids row) leaves it
    // false and the boot restore simply skips cgroup restoration (graceful
    // backward compatibility).
    std::string origCgroupGroup;
    bool haveOrigCgroup = false;

    // H14 (v4.2.3): full cgroup path + hierarchy tag. The legacy origCgroupGroup
    // is the basename (e.g. "foreground") and is resolved by caps::resolveCgroup*()
    // at restore time. On nested-cgroup systems the basename can be ambiguous; the
    // full path (e.g. "/a/b/foreground") lets us write the thread back to the exact
    // original cgroup node. origCgroupHier records which hierarchy the thread was
    // on: "cpuset" (v1 cpuset root), "cpu" (v1 cpuctl root), "unified" (v2), or
    // "" (unknown -> fall back to legacy basename dual-write).
    // Both fields are optional: a .rec written by v4.2.1/v4.2.2 (no these keys)
    // leaves them empty and the boot restore uses the legacy basename path.
    std::string origCgroupPath;
    std::string origCgroupHier;
    bool haveOrigCgroupPath = false;

    long long timestamp = 0;
};

// Must be called once at daemon startup AFTER RUN_DIR exists. Reads every
// recovery/<tid>.rec into the in-memory cache so a restarted daemon knows which
// tids were already snapshotted and must NOT be re-snapshotted. Safe to call
// multiple times (idempotent; existing in-memory entries win).
void recoveryBootload();

// True if `tid` already has a recovery record (loaded from disk or written this
// run). Used to skip re-snapshotting the original on a restarted daemon.
bool recoveryIsRecorded(int tid);

// Fetch a COPY of the record for `tid`. Returns false if no record exists.
// Callers merge their dimension into the returned copy and call recoveryCommit().
bool recoveryGet(int tid, RecoveryRecord& out);

// Atomically write `rec` to recovery/<tid>.rec (tmp + fsync + rename). Marks
// the tid recorded in memory ONLY on success. Returns false on any I/O failure
// so the caller may retry on the next scan. Never overwrites an existing
// record's orig fields — the caller must load() then merge, not reconstruct.
bool recoveryCommit(const RecoveryRecord& rec);

// Convenience: record affinity for the first time. If a record already exists
// and haveOrigAffinity is true, this is a no-op success (do NOT re-snapshot).
// Otherwise fills orig/applied affinity and commits. Returns false on I/O
// failure or if the thread could not be identified.
bool recoveryRecordAffinity(int pid, int tid, unsigned long long starttime,
                           const std::string& origList,
                           const std::string& appliedList);

// Convenience: record sched (policy/priority/nice) + uclamp for the first time.
// If a record already has haveOrigSched, orig policy/priority/nice/uclamp are
// NOT re-read (cross-restart guard); only the applied values are refreshed.
// Returns false on I/O failure.
bool recoveryRecordSched(int pid, int tid, unsigned long long starttime,
                         int origPolicy, int origPriority, int origNice,
                         uint32_t origMin, uint32_t origMax);
bool recoveryRecordSched(int pid, int tid, unsigned long long starttime,
                         uint32_t origPolicy, uint32_t origPriority, int origNice,
                         uint32_t origMin, uint32_t origMax,
                         uint32_t appliedPolicy, uint32_t appliedPriority, int appliedNice,
                         uint32_t appliedMin, uint32_t appliedMax);

// H14: record the original cgroup full path + hierarchy. Fills origCgroupPath /
// origCgroupHier / haveOrigCgroupPath on the existing record (creates a bare
// record if needed). Caller must have already recorded origCgroupGroup (basename)
// via the D1 backfill or an earlier call. Safe to call with empty path -> no-op.
bool recoveryRecordCgroupPath(int tid, const std::string& path, const std::string& hier);

// ROUND-013: refresh-only APIs (called by G5 after each successful syscall).
// These update ONLY the applied fields; orig fields are never overwritten.
// If no record exists for the tid, these are no-ops (return false).
bool recoveryRefreshAppliedAffinity(int tid, const std::string& appliedList);
bool recoveryRefreshAppliedSched(int tid, uint32_t appliedPolicy, uint32_t appliedPriority,
                                int appliedNice, uint32_t appliedMin, uint32_t appliedMax);

// Number of records currently tracked (tests / diagnostics).
size_t recoveryCount();

// erase the recovery record for a thread that has exited.
// Called from thread_scan.cpp (gone-thread branch) and eBPF onExit.
// Removes the in-memory map entry and unlinks the on-disk .rec file.
// No-op if the tid has no record. Safe to call on dead/recycled tids.
void recoveryErase(int tid);

// ROUND-014 P3: boot-time sched restore. Called once after recoveryBootload().
// Iterates all loaded records; for each live thread with matching starttime,
// restores origPolicy/origPriority/origNice/origUclampMin/Max.
// Dead threads and TID-recycled entries are erased.
void recoveryRestoreSchedOnBoot();

// ROUND-014 P3: clear all persisted records on clean shutdown.
// Called from daemon shutdown sequence so boot restore only handles crash recovery.
void recoveryClearAllPersisted();

}  // namespace aatpe_recovery

#endif  // AATPE_RECOVERY_H
