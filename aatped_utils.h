#ifndef AATPED_UTILS_H
#define AATPED_UTILS_H

#include <string>
#include <vector>
#include <set>
#include <cstdint>
#include <sched.h>

int aatpeAndroidSdkInt();
const char* aatpeAndroidVersionName();
bool aatpeIsCgroupV2Cpuset();
std::string aatpeCpusetMountRoot();

void mkdirIfNotExist(const char* path, mode_t mode);
std::string readFile(const char* path);
bool writeFile(const char* path, const std::string& content);
bool writeFile(const char* path, int val);
bool writeFile(const char* path, float value);
bool writeFile(const char* path, long long value);
bool writeFileAtomic(const char* path, const std::string& content);

#ifdef AATPE_HOST_TEST
extern int g_fi_write_short_after;
extern int g_fi_write_fail_after;
extern int g_fi_fsync_fail;
void aatpeTestFiReset();
#endif
bool atomicWriteFile(const char* path, const std::string& content);
bool atomicWriteFile(const char* path, int val);
bool atomicWriteFile(const char* path, long long value);
std::string getModuleDir();
bool validateConfig();
int readInt(const char* path, int def);
long long readInt64(const char* path, long long def);
void trim(std::string& s);
std::string readFirstLine(const char* path);

bool stateCacheIsSeeded();
void stateCacheSeedFromDir(const char* dir);
std::vector<std::pair<std::string,std::string>> stateCacheSnapshot();
void stateCacheNoteRemove(const char* path);

uint64_t stateCacheGeneration();

extern std::vector<ThreadRule> g_rules;
void loadThreadRules();
bool matchPattern(const std::string& text, const std::string& pattern);
ThreadRole classifyThread(int pid, int tid, const std::string& tname, const std::string& pname, int& outPriority, std::string& outSubtype, int* outConfidence = nullptr, unsigned long long cachedStarttime = 0);

extern std::unordered_map<int, ThreadInfo> g_threads;
extern std::mutex g_threadsMutex;
bool isKernelThread(const std::string& tname, int tid);
bool isKthreaddChild(int tid);
int findPidByPackage(const std::string& pkgName);
void scanThreads();
void persistThreadDb(const std::unordered_map<int, ThreadInfo>& threads);

extern std::unordered_map<std::string, int> g_threadRoleCache;
extern std::mutex g_threadRoleCacheMutex;
int getCachedThreadRole(const std::string& threadName);
void setCachedThreadRole(const std::string& threadName, int role);
void invalidateThreadRoleCache(const std::string& threadName);

cpu_set_t parseCpusetCpuList(const std::string& listStr);
std::string readThreadCpuset(int tid);
cpu_set_t readCpusetCpus(const std::string& cpusetPath);
cpu_set_t getThreadAllowedCpus(int tid);
bool isCpuAllowed(int cpu, const cpu_set_t& allowed);

[[deprecated("use aatpeExecArgv (no shell) — sh -c injection risk")]]
std::string aatpeExec(const std::string& cmd, const char* debugTag);
std::string aatpeExecArgv(const std::vector<std::string>& argv, const char* debugTag,
                          int* outExitStatus = nullptr, bool bypassCooldown = false);

extern std::atomic<bool> g_helperApkInstalled;
#include <atomic>
extern std::atomic<bool> g_helperAutoSuppressed;

void lazyCleanupDecisionsDir(void);
void watchdogTakeoverScheduling(void);
int computeAdaptiveInterval(float systemLoad, bool jankDetected, bool hasForeground, int configInterval);
void cleanupDecisionsDir();
void protectCriticalThreads();
int emergencyMigrateCriticalThreads();
int lightweightMigrateCriticalThreads();
void aatpeProbeEnvironment();
int detectBatteryState();

bool dumpsysAvailable();
void triggerDumpsysCooldown();
std::string aatpeDetectForegroundApp(bool forceRefresh = false);
bool detectGameProcess();

struct ThreadScoreEntry {
    Decision   lastDecision;
    long long  cachedAtMs = 0;
    long long  lastSwitchMs = 0;
    bool       dirty = false;
    bool       forceRescore = false;
    bool       wakeHint = false;
};

extern std::unordered_map<int, ThreadScoreEntry> g_scoreCache;
extern std::mutex g_scoreCacheMutex;
extern const long long kScoreTtlMs;
extern const long long kInactiveMs;
extern const long long kRebalanceMs;

void markThreadScoreDirty(int tid, long long nowMs);

void invalidateAllScores();

void forceRescoreAllScores();

constexpr int kSchedstatIdleRefreshesActive = 6;

inline bool threadIsActiveBySchedstat(const ThreadInfo& ti) {
    return ti.switchesPerSec > 0.0 || ti.idleRefreshes < kSchedstatIdleRefreshesActive;
}

inline bool shouldRescoreThread(ThreadScoreEntry& entry,
                               const ThreadInfo& ti,
                               long long nowMs,
                               long long ttlMs,
                               long long inactiveMs,
                               bool ebpfLive) {
    bool neverScored = (entry.cachedAtMs == 0);
    bool expired     = (entry.cachedAtMs != 0) && ((nowMs - entry.cachedAtMs) >= ttlMs);

    bool activeByEBPF = false;
    if (ebpfLive) {
        activeByEBPF = (entry.lastSwitchMs == 0) ||
                       ((nowMs - entry.lastSwitchMs) < inactiveMs);
    }
    bool activeBySchedstat = threadIsActiveBySchedstat(ti);

    bool active = activeByEBPF || activeBySchedstat;

    bool bypass = entry.forceRescore || entry.wakeHint;

    bool needsRescore = neverScored || bypass || ((entry.dirty || expired) && active);
    entry.dirty = false;
    entry.forceRescore = false;
    entry.wakeHint = false;
    return needsRescore;
}

#endif
