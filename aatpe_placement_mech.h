// aatpe_placement_mech.h — unified scheduling-boost mechanism layer (ROUND-005).
#pragma once
#include <cstdint>
#include <string>

namespace aatpe_mech {

struct SchedCaps {
    bool hasStune        = false;
    bool uclamp          = false;  // sched_setattr util_min/max + sched_getattr readback MATCHES
    bool uclampPartial   = false;  // returns 0 but readback mismatches (fake positive)
    bool cgroupUclamp    = false;
    bool cpusetTopApp    = false;
};

enum class BoostMechanism {
    UCLAMP, CGROUP_UCLAMP, STUNE_CPUSET, AFFINITY, NICE_ONLY
};

BoostMechanism selectBoostMechanism(const SchedCaps& c);
// WS-C: real per-task uclamp set (arm64 syscall). utilMin=UINT32_MAX resets.
int           aatpeUclampSetMin(int tid, unsigned int utilMin);
int           aatpeUclampGetLastTid();
unsigned int  aatpeUclampGetLastMin();
// WS-C: cgroup uclamp.min write (v2 percent / v1 raw).
bool          writeCgroupUclampMin(int tid, unsigned int minValue, const std::string& group);
// WS-C: symmetric teardown — move tid back to saved group, then restore floor.
// #4: returns false (and leaves the group floor up) if the move-back failed, so
// the caller can retry instead of tearing down a floor the thread still needs.
bool          restoreTidFromCgroupUclamp(int tid, const std::string& boostGroup, const std::string& savedGroup);
// #4: drop one unit of group-floor bookkeeping after move-back retries are
// exhausted (dead/recycled tid). Never migrates the thread.
void          forceReleaseCgroupUclamp(const std::string& boostGroup);
// ROUND-013: bulk restore all cgroup uclamp floors on shutdown.
void          restoreAllCgroupUclamp();

// audit4 F3: SINGLE OWNER of cgroup-uclamp original-node strings + reference-counted
// group holds. PSI (background cut / top-app boost) and the spillover boost both
// write cpuRoot/<group>/cpu.uclamp.min; previously each layer snapshotted the node's
// "original" independently at different times and restored its own baseline, so the
// two restores overwrote each other and the true original could be lost. Now the
// FIRST touch records the verbatim original once, every holder increments a refcount,
// and the recorded original is restored exactly when the LAST holder releases.
// Record group's verbatim original on first touch (idempotent). Returns the
// recorded original string ("" when the node is absent / was empty at first touch).
// Thread-safe.
std::string   cgUclampRecordOriginal(const std::string& group);
// 0..100 parse of the recorded original; -1.0f if not recorded / node absent.
float         cgUclampOriginalPct(const std::string& group);
// One more holder wants the group floor kept up (ensures the original is recorded).
void          cgUclampHold(const std::string& group);
// Drop one holder. When the refcount hits 0, restore the recorded original (or
// "0" if the node was absent at first touch). No-op if the group was never held.
void          cgUclampReleaseHold(const std::string& group);
// Test seam: clear the entire cgroup-uclamp ledger (originals + refcounts).
#ifdef AATPE_HOST_TEST
void          cgUclampResetForTest();
#endif
// Test seam: clear the entire per-tid uclamp floor ledger.
#ifdef AATPE_HOST_TEST
void          aatpeUclampFloorsResetForTest();
#endif

// audit4 F4: per-tid, per-source util_min floor ledger. Three producers (touch
// window, placement spillover, RT priority) all drive sched_setattr util_min;
// last-writer-wins let touch's RELEASE (write 0) stomp the RT 512 floor. Each
// producer registers its desired floor separately and the effective value is the
// MAX across producers, so no producer can step another's floor down.
enum AatpeUclampSrc {
    AATPE_UCLAMP_TOUCH = 0,
    AATPE_UCLAMP_SPILLOVER = 1,
    AATPE_UCLAMP_PRIORITY = 2,
    AATPE_UCLAMP_SRC_N = 3
};
// Set (or clear, with utilMin=0) one source's desired floor for tid; recomputes the
// effective floor = max over all sources and issues it. Returns the syscall rc.
int           aatpeUclampFloorSet(int tid, int src, unsigned int utilMin);
// Drop one source's contribution (equivalent to aatpeUclampFloorSet(tid,src,0)).
void          aatpeUclampFloorClear(int tid, int src);
// v4.4.8 MAJOR-2: read-only getter for one source's desired floor (0 if never
// set / no ledger entry). Lets the RT-priority path tell "the floor I myself
// last wrote" apart from "another source raised the kernel floor", so a thermal
// soft-derate (target dropped below our own last write) can force the write-down
// instead of being short-circuited by the `cur >= target` idempotency test.
unsigned int  aatpeUclampFloorGet(int tid, int src);
// RC6 (B2): erase the per-tid floor ledger entry. Called by thread_scan when a tid
// dies or is recycled (starttime changed), so a stale floor is NOT inherited via
// max() by a new thread reusing the same tid.
void          aatpeUclampFloorReap(int tid);
const char*    mechanismName(BoostMechanism m);
std::string    renderCapabilities(const SchedCaps& c);
SchedCaps      probeCapabilities();

// cpuset helpers (STUNE_CPUSET real action).
std::string readThreadCpusetGroup(int tid);          // e.g. "foreground"
// H14: full cgroup introspection. outBase/outPath/outHier may be nullptr.
// Returns false on open/parse failure (caller must fall back to basename).
bool        readThreadCgroupInfo(int tid, std::string* outBase,
                                 std::string* outPath, std::string* outHier);
bool        moveTidToCpusetGroup(int tid, const std::string& groupName);
// ROUND-008: does /dev/cpuset/top-app/cpus cover cpu? If it excludes PRIME,
// stune_cpuset cannot land threads on PRIME.
bool        topAppCpusContainsCpu(int cpu);

// test seam: prefix all sysfs paths with root (e.g. /tmp/fake_sys); set uclamp
// verdict (1=yes, 2=partial, other=no).
#ifdef AATPE_HOST_TEST
void mechSetTestRoot(const std::string& root);
void mechSetUclampVerdict(int v);
#endif

}  // namespace aatpe_mech
