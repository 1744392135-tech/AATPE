#ifndef AATPE_RECOVERY_H
#define AATPE_RECOVERY_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace aatpe_recovery {

constexpr int RECOVERY_SCHEMA_VERSION = 2;

struct RecoveryRecord {
    int pid = 0;
    int tid = 0;
    unsigned long long starttime = 0;

    std::string origAffinity;
    std::string appliedAffinity;
    bool haveOrigAffinity = false;

    uint32_t origPolicy = 0;     uint32_t appliedPolicy = 0;
    uint32_t origPriority = 0;  uint32_t appliedPriority = 0;
    int      origNice = 0;       int      appliedNice = 0;
    bool haveOrigSched = false;

    uint32_t origUclampMin = 0;  uint32_t appliedUclampMin = 0;
    uint32_t origUclampMax = 0;   uint32_t appliedUclampMax = 0;
    bool haveOrigUclamp = false;

    std::string origCgroupGroup;
    bool haveOrigCgroup = false;

    std::string origCgroupPath;
    std::string origCgroupHier;
    bool haveOrigCgroupPath = false;

    long long timestamp = 0;
};

void recoveryBootload();

bool recoveryIsRecorded(int tid);

bool recoveryGet(int tid, RecoveryRecord& out);

bool recoveryCommit(const RecoveryRecord& rec);

bool recoveryRecordAffinity(int pid, int tid, unsigned long long starttime,
                           const std::string& origList,
                           const std::string& appliedList);

bool recoveryRecordSched(int pid, int tid, unsigned long long starttime,
                         int origPolicy, int origPriority, int origNice,
                         uint32_t origMin, uint32_t origMax);
bool recoveryRecordSched(int pid, int tid, unsigned long long starttime,
                         uint32_t origPolicy, uint32_t origPriority, int origNice,
                         uint32_t origMin, uint32_t origMax,
                         uint32_t appliedPolicy, uint32_t appliedPriority, int appliedNice,
                         uint32_t appliedMin, uint32_t appliedMax);

bool recoveryRecordCgroupPath(int tid, const std::string& path, const std::string& hier);

bool recoveryRefreshAppliedAffinity(int tid, const std::string& appliedList);
bool recoveryRefreshAppliedSched(int tid, uint32_t appliedPolicy, uint32_t appliedPriority,
                                int appliedNice, uint32_t appliedMin, uint32_t appliedMax);

size_t recoveryCount();

void recoveryErase(int tid);

void recoveryRestoreSchedOnBoot();

void recoveryClearAllPersisted();

}

#endif
