// AATPE - Utility functions sub-header
#ifndef AATPED_UTILS_H
#define AATPED_UTILS_H

#include <string>
#include <vector>
#include <set>
#include <cstdint>
#include <sched.h>

// Android version detection
int aatpeAndroidSdkInt();
const char* aatpeAndroidVersionName();
bool aatpeIsCgroupV2Cpuset();
std::string aatpeCpusetMountRoot();

// File utilities
void mkdirIfNotExist(const char* path, mode_t mode);
std::string readFile(const char* path);
bool writeFile(const char* path, const std::string& content);
bool writeFile(const char* path, int val);
bool writeFile(const char* path, float value);
bool writeFile(const char* path, long long value);
bool writeFileAtomic(const char* path, const std::string& content);

// v4.4.9 A2: host-only fault-injection seams for writeFileAtomic. release builds
// compile these out entirely. Each is reset to 0 before every scenario.
#ifdef AATPE_HOST_TEST
extern int g_fi_write_short_after;  // >0: Nth write() returns a 1-byte short write
extern int g_fi_write_fail_after;   // >0: Nth write() returns -1/errno=EIO
extern int g_fi_fsync_fail;         // 1: fsync() returns -1/errno=EIO
void aatpeTestFiReset();            // reset all three to 0
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

// in-memory state-file cache.
// The daemon is the sole producer of files under STATE_DIR and ALL of its writes
// go through writeFile()/writeFileAtomic() (verified: no direct open()/ofstream
// writes to STATE_DIR). Each write mirrors into an in-memory map. The SSE
// aggregate JSON (buildAggregateStateJson) is then built from that map instead of
// opendir() + N open()/read() every 500ms. Seeded once from the filesystem on
// first read so any pre-existing file is captured; afterwards the map stays
// byte-for-byte identical to the on-disk content (same trailing trim).
bool stateCacheIsSeeded();
void stateCacheSeedFromDir(const char* dir);
std::vector<std::pair<std::string,std::string>> stateCacheSnapshot();
// cache invalidation for deletions. Mirrors stateCacheNoteWrite's
// path validation and drops the flat STATE_DIR entry from the in-memory cache so
// an unlink() on disk is not still served as stale data on the next SSE push.
void stateCacheNoteRemove(const char* path);

// #20: state-generation counter. Bumps every time a state file's content actually
// changes (added/removed/value differs). SSE compares this single number instead
// of diffing every state file each tick; the aggregate JSON embeds it as
// "state_generation".
uint64_t stateCacheGeneration();

// Thread classification
extern std::vector<ThreadRule> g_rules;
void loadThreadRules();
bool matchPattern(const std::string& text, const std::string& pattern);
// P2-8: cachedStarttime lets callers that already have ThreadInfo.starttime
// (from a prior populate or a TID-reuse check) skip the /proc/<pid>/task/<tid>/stat
// open()/read() on every classifyThread() call. When 0, classifyThread falls back
// to reading starttime from /proc (preserves the eBPF onExec path which has no
// cached starttime).
ThreadRole classifyThread(int pid, int tid, const std::string& tname, const std::string& pname, int& outPriority, std::string& outSubtype, int* outConfidence = nullptr, unsigned long long cachedStarttime = 0);

// P0-opt2: g_threads is std::unordered_map (O(1) lookup) instead of std::map (O(log n)).
// No iteration-order dependency exists in production code; persistThreadDb just writes
// a snapshot file whose ordering is not consumed by any reader.
extern std::unordered_map<int, ThreadInfo> g_threads;
extern std::mutex g_threadsMutex;
bool isKernelThread(const std::string& tname, int tid);
bool isKthreaddChild(int tid);
int findPidByPackage(const std::string& pkgName);
void scanThreads();
void persistThreadDb(const std::unordered_map<int, ThreadInfo>& threads);

// Thread role cache
// C-007: g_threadRoleCacheMutex protects all access to g_threadRoleCache.
extern std::unordered_map<std::string, int> g_threadRoleCache;
extern std::mutex g_threadRoleCacheMutex;
int getCachedThreadRole(const std::string& threadName);
void setCachedThreadRole(const std::string& threadName, int role);
void invalidateThreadRoleCache(const std::string& threadName);

// cpuset
cpu_set_t parseCpusetCpuList(const std::string& listStr);
std::string readThreadCpuset(int tid);
cpu_set_t readCpusetCpus(const std::string& cpusetPath);
cpu_set_t getThreadAllowedCpus(int tid);
bool isCpuAllowed(int cpu, const cpu_set_t& allowed);

// Exec helper
// MON-08: aatpeExec runs `sh -c <cmd>` — a shell. If any caller ever concatenates
// external input into `cmd`, that is command injection. The hardened aatpeExecArgv
// below does NOT invoke a shell. New code MUST use aatpeExecArgv. This symbol is
// retained only for existing callers (audio dumpsys, ml getprop/dumpsys gfxinfo,
// cli pm list) whose cmd strings are fixed or whitelist-validated.
[[deprecated("use aatpeExecArgv (no shell) — sh -c injection risk")]]
std::string aatpeExec(const std::string& cmd, const char* debugTag);
// #8/#9 fix: argv-based hardened exec, NO shell. The canonical, shared home lives in
// aatped_utils.cpp (moved here from frame.cpp so cold_start_rules / thermal can reuse
// it without pulling frame.cpp). Same hard timeout (3000ms wall clock) + setpgid +
// killpg + bounded WNOHANG reap + dumpsys cooldown as aatpeExec. Use this whenever the
// command is a fixed argv (e.g. {"dumpsys","activity","activities"}) — grep/head
// filtering must be done in C++, never through `sh -c`.
//
// D2 (v4.2): two optional extensions for the bpftool loader:
//   *outExitStatus  : if non-null, receives the raw waitpid(2) status of the child
//                     on a clean reaping (use WIFEXITED/WEXITSTATUS), or -1 on
//                     timeout / launch failure / not-reaped. Lets callers branch on
//                     the real exit code (bpftool loadall tries several variants).
//   bypassCooldown  : true to SKIP the dumpsys cooldown gate. The gate exists to
//                     avoid hammering a hung SurfaceFlinger after a dumpsys timeout;
//                     bpftool (and other non-dumpsys fixed-argv tools) must NOT be
//                     suppressed by an unrelated dumpsys stall. Defaults to false so
//                     existing dumpsys callers keep their cooldown behavior.
std::string aatpeExecArgv(const std::vector<std::string>& argv, const char* debugTag,
                          int* outExitStatus = nullptr, bool bypassCooldown = false);

// Helper APK detection
// C-011: changed to atomic<bool> for thread-safe cross-thread access.
extern std::atomic<bool> g_helperApkInstalled;
// runtime auto-profile suppression by installed Helper APK.
// effectiveAutoProfile = g_cfg.autoProfile && !g_helperAutoSuppressed.
#include <atomic>
extern std::atomic<bool> g_helperAutoSuppressed;

// fix95 misc
void lazyCleanupDecisionsDir(void);
void watchdogTakeoverScheduling(void);
int computeAdaptiveInterval(float systemLoad, bool jankDetected, bool hasForeground, int configInterval);
void cleanupDecisionsDir();
void protectCriticalThreads();
int emergencyMigrateCriticalThreads();
int lightweightMigrateCriticalThreads();
void aatpeProbeEnvironment();
int detectBatteryState();

// Frame / refresh
bool dumpsysAvailable();
void triggerDumpsysCooldown();
std::string aatpeDetectForegroundApp(bool forceRefresh = false);
bool detectGameProcess();

// ============================================================================
// Event-driven scoring cache (方案3 事件驱动架构重构)
//
// Instead of re-scoring ~200 threads every decision cycle (200ms, or 50ms on
// jank), each thread caches its last Decision + timestamp. A thread is re-scored
// only when:
//   (a) an eBPF sched_switch event touched it (dirty bit set by onSwitch), OR
//   (b) its cached score expired (kScoreTtlMs), AND it is still "active"
//       (had a sched_switch within kInactiveMs — sleeping threads are skipped).
// A low-frequency global rebalance (kRebalanceIntervalMs, or on a temperature /
// load step) force-invalidates all caches as a correctness backstop.
//
// Single-threaded: onSwitch() runs on the daemon main loop (via
// dispatchQueuedEvents()), and the decision loop also runs on the main loop —
// same access discipline as g_threads (no extra lock needed).
// ============================================================================
struct ThreadScoreEntry {
    Decision   lastDecision;       // cached decision from last rescore
    long long  cachedAtMs = 0;     // steady_clock ms when last scored (0 = never)
    long long  lastSwitchMs = 0;   // steady_clock ms of last sched_switch (activity)
    bool       dirty = false;      // needs immediate rescore (event-triggered)
};

extern std::unordered_map<int, ThreadScoreEntry> g_scoreCache;
extern std::mutex g_scoreCacheMutex;
extern const long long kScoreTtlMs;       // cache validity before a fresh rescore
extern const long long kInactiveMs;       // no sched_switch this long -> "inactive"
extern const long long kRebalanceMs;     // global full-sweep interval backstop

// Called by onSwitch() (eBPF sched_switch consumer): mark the thread dirty and
// record activity. Safe no-op if the tid has no cache entry yet.
void markThreadScoreDirty(int tid, long long nowMs);

// Force-invalidate every cached score (global rebalance backstop).
void invalidateAllScores();

#endif // AATPED_UTILS_H
