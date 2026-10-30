#pragma once
#include <cstdint>
#include <string>

namespace aatpe_mech {

struct SchedCaps {
    bool hasStune        = false;
    bool uclamp          = false;
    bool uclampPartial   = false;
    bool cgroupUclamp    = false;
    bool cpusetTopApp    = false;
};

enum class BoostMechanism {
    UCLAMP, CGROUP_UCLAMP, STUNE_CPUSET, AFFINITY, NICE_ONLY
};

BoostMechanism selectBoostMechanism(const SchedCaps& c);
int           aatpeUclampSetMin(int tid, unsigned int utilMin);
int           aatpeUclampGetLastTid();
unsigned int  aatpeUclampGetLastMin();
bool          writeCgroupUclampMin(int tid, unsigned int minValue, const std::string& group);
bool          restoreTidFromCgroupUclamp(int tid, const std::string& boostGroup, const std::string& savedGroup);
void          forceReleaseCgroupUclamp(const std::string& boostGroup);
void          restoreAllCgroupUclamp();

std::string   cgUclampRecordOriginal(const std::string& group);
float         cgUclampOriginalPct(const std::string& group);
void          cgUclampHold(const std::string& group);
void          cgUclampReleaseHold(const std::string& group);
#ifdef AATPE_HOST_TEST
void          cgUclampResetForTest();
#endif
#ifdef AATPE_HOST_TEST
void          aatpeUclampFloorsResetForTest();
#endif

enum AatpeUclampSrc {
    AATPE_UCLAMP_TOUCH = 0,
    AATPE_UCLAMP_SPILLOVER = 1,
    AATPE_UCLAMP_PRIORITY = 2,
    AATPE_UCLAMP_SRC_N = 3
};
int           aatpeUclampFloorSet(int tid, int src, unsigned int utilMin);
void          aatpeUclampFloorClear(int tid, int src);
unsigned int  aatpeUclampFloorGet(int tid, int src);
void          aatpeUclampFloorReap(int tid);
const char*    mechanismName(BoostMechanism m);
std::string    renderCapabilities(const SchedCaps& c);
SchedCaps      probeCapabilities();

std::string readThreadCpusetGroup(int tid);
bool        readThreadCgroupInfo(int tid, std::string* outBase,
                                 std::string* outPath, std::string* outHier);
bool        moveTidToCpusetGroup(int tid, const std::string& groupName);
bool        topAppCpusContainsCpu(int cpu);

#ifdef AATPE_HOST_TEST
void mechSetTestRoot(const std::string& root);
void mechSetUclampVerdict(int v);
#endif

}
