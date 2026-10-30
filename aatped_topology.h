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

extern int g_cpuCount;
extern std::atomic<uint64_t> g_topologyGeneration;
extern std::vector<int> g_allCpus;
extern std::vector<int> g_littlePool;
extern std::vector<int> g_midPool;
extern std::vector<int> g_bigPool;
extern std::vector<CpuCluster> g_clusters;

extern std::mutex g_topologyRwMutex;

CpuPool getPoolForCpu(int cpu);
std::vector<int> getPoolCpus(CpuPool pool);
const char* getPoolName(CpuPool pool);
CpuTier            tierOfCpu(int cpu);
const std::vector<CpuCluster>& clusters();
std::vector<CpuCluster> clustersSnapshot();
int                clusterCount();
std::vector<int>   primePool();
std::vector<int>   primePoolLocked();
const char*        tierName(CpuTier t);
std::string        clustersStateText();
bool               clustersTextPublishable(const std::string& txt);
void buildGenericClusters(const std::vector<int>& caps, const std::vector<int>& cpuinfoFreqs);
void applyManualPrimeOverride(const std::string& primeCfg);
#ifdef AATPE_HOST_TEST
void topologyInjectPolicyForTest(const std::map<int,int>& m);
void topologyInjectPartForTest(const std::map<int,int>& m);
void topologyInjectOnlineForTest(const std::map<int,bool>& m);
#endif
void markTopologyReady();
bool topologyRetryDue(uint64_t nowMs);
std::vector<int> getOnlineCpus();
void refreshCpuTopology();
void detectCpuTopology();
void detectCpuTopologyUnlocked();
void seedOnlineCacheFromSysfs();

bool isCpuOnlineNow(int cpu);
int  topologyRebuildCount();
bool isTopologyRebuilding();
#ifdef AATPE_HOST_TEST
void setTopologyDebounceMsForTest(int ms);
#endif
std::set<int> parseAffinityMask(const std::string& maskStr);
cpu_set_t getThreadAffinityCpus(int tid);
int readThreadCurrentCpu(int pid, int tid);

#endif
