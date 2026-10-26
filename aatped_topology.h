// AATPE - CPU topology sub-header
#ifndef AATPED_TOPOLOGY_H
#define AATPED_TOPOLOGY_H

#include <vector>
#include <set>
#include <map>
#include <string>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <sched.h>
#include "aatped_types.h"

// P3-28: g_cpuCount is a plain int whose DEFINITION lives in aatped_main.cpp
// (out of scope for this module), so it cannot be re-declared as
// std::atomic<int> here without breaking that definition. It is WRITTEN only
// by detectCpuTopology()/detectCpuTopologyUnlocked() under g_topologyRwMutex.
// Cross-thread readers (writeStateJson in aatped_state.cpp, webui_json, etc.)
// MUST snapshot it under g_topologyRwMutex (the same lock used for the pool
// vectors) to avoid a data race on the write side.
extern int g_cpuCount;
// P1-4: bumped under g_topologyRwMutex but read by the WebUI JSON writer
// without that lock. Make it atomic to eliminate the C++ data race.
extern std::atomic<uint64_t> g_topologyGeneration;
extern std::vector<int> g_allCpus;
extern std::vector<int> g_littlePool;
extern std::vector<int> g_midPool;
extern std::vector<int> g_bigPool;
// ROUND-004 B1: generic clusters ordered weakest -> strongest. The legacy
// little/mid/big pools are reverse-derived from this for compatibility; PRIME is
// NOT merged into BIG. Accessors below are the supported API for G5/G7.
extern std::vector<CpuCluster> g_clusters;

extern std::mutex g_topologyRwMutex;

CpuPool getPoolForCpu(int cpu);
std::vector<int> getPoolCpus(CpuPool pool);
const char* getPoolName(CpuPool pool);
// ROUND-004 B1 accessors (read snapshot under g_topologyRwMutex in consumers):
CpuTier            tierOfCpu(int cpu);          // LITTLE/MID/BIG/PRIME
const std::vector<CpuCluster>& clusters();       // weak -> strong
// B-002: lock-protected VALUE copy of g_clusters. clusters() returns a const ref
// WITHOUT taking g_topologyRwMutex; callers that do NOT already hold the lock
// (placement spillover / feature builder) must use this snapshot instead, so a
// concurrent detectCpuTopology() rewrite cannot reallocate the vector mid-iteration
// and invalidate the caller's iterator/size read (UB).
std::vector<CpuCluster> clustersSnapshot();
int                clusterCount();               // number of detected clusters
std::vector<int>   primePool();                  // highest (PRIME) cluster cpus (empty if none)
std::vector<int>   primePoolLocked();            // ROUND-007h: caller holds g_topologyRwMutex (h6 D1 self-deadlock fix)
const char*        tierName(CpuTier t);
std::string        clustersStateText();          // "tier,capacity,minKhz-maxKhz:cpulist" per line
bool               clustersTextPublishable(const std::string& txt);  // ROUND-007d guard
// Test seam: run generic clustering directly on synthetic inputs (g_allCpus must
// be set by caller). Used by tests/v94/test_topology.cpp.
void buildGenericClusters(const std::vector<int>& caps, const std::vector<int>& cpuinfoFreqs);
void applyManualPrimeOverride(const std::string& primeCfg);  // PRODUCTION config path (cfg.primePoolCfg at topology.cpp:1015); tests also exercise it, but NOT a test-only seam -> not guarded.
#ifdef AATPE_HOST_TEST
void topologyInjectPolicyForTest(const std::map<int,int>& m);  // test seam
void topologyInjectPartForTest(const std::map<int,int>& m);   // v4.4.9 R1-4: test seam
void topologyInjectOnlineForTest(const std::map<int,bool>& m);// v4.4.9 R1-4: test seam
#endif
// ROUND-005: startup backoff retry.
void markTopologyReady();
bool topologyRetryDue(uint64_t nowMs);
// GPUTOP-11: getOnlineCpus() and seedOnlineCacheFromSysfs() are declared here
// but DEFINED in placement.cpp (they own the online-cache + sysfs raw reader).
// aatped_topology.cpp calls seedOnlineCacheFromSysfs() after a successful
// rebuild; placement.cpp owns the cache. This is a deliberate cross-TU split,
// not a dangling declaration — both TUs are linked together in the daemon.
std::vector<int> getOnlineCpus();
void refreshCpuTopology();
void detectCpuTopology();
void detectCpuTopologyUnlocked();
// P3-a: seed the online cache from raw sysfs. Called by detectCpuTopology() at
// the end of its successful rebuild; also callable from tests.
void seedOnlineCacheFromSysfs();

// H13 test seams (host tests/v94/test_h13_hotplug.cpp).
bool isCpuOnlineNow(int cpu);          // hard offline filter against online cache
int  topologyRebuildCount();           // count of actual detectCpuTopology() rebuilds
bool isTopologyRebuilding();           // true mid-rebuild
#ifdef AATPE_HOST_TEST  // RC7(C6): test seam, no production caller
void setTopologyDebounceMsForTest(int ms);  // 0 = disable debounce; default 500ms
#endif
std::set<int> parseAffinityMask(const std::string& maskStr);
// P1-1: returns cpu_set_t (stack, 128 bytes) instead of std::set<int> (heap).
// Callers use CPU_ISSET() bit operations — O(1) instead of O(log n) tree lookup.
cpu_set_t getThreadAffinityCpus(int tid);
int readThreadCurrentCpu(int pid, int tid);

#endif // AATPED_TOPOLOGY_H
