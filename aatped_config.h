// AATPE - Configuration sub-header
#ifndef AATPED_CONFIG_H
#define AATPED_CONFIG_H

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

// ============================================================================
// GLOBAL LOCK ORDERING (concurrency fix — single canonical order)
// ============================================================================
//
// The ONLY allowed lock acquisition order is:
//
//   g_cfgMutex  ->  g_thermalMutex  ->  g_mlMutex  ->  g_hybridMutex
//
// Rules:
//   1. g_cfgMutex is the HIGHEST-level lock. It may be taken alone.
//   2. g_thermalMutex may only be taken while ALREADY holding g_cfgMutex,
//      or alone (never before g_cfgMutex).
//   3. g_mlMutex may only be taken while holding g_cfgMutex (or alone).
//   4. g_hybridMutex may only be taken while holding g_cfgMutex AND optionally
//      g_mlMutex (or alone).
//   5. NEVER reverse this order. Specifically:
//        - Do NOT take g_mlMutex then g_cfgMutex.
//        - Do NOT take g_hybridMutex then g_cfgMutex.
//        - Do NOT take g_hybridMutex then g_mlMutex.
//      (These would deadlock against a concurrent reader that follows the
//      documented order above.)
//   6. Helper locks (g_threadsMutex, g_stateMutex, g_foregroundMutex,
//      g_hmacMutex, g_migRateMutex, g_frameCacheMutex) are leaf locks: they
//      may be taken while holding any of the above, but NEVER while another
//      leaf lock is held. They never nest into the cfg/thermal/ml/hybrid chain.
//
// Runtime side effects (ML setEnabled, hybrid setEnabled) MUST be performed
// while holding g_cfgMutex (and the relevant ml/hybrid lock per the order
// above) so that g_cfg state and runtime state update atomically — no reader
// can observe g_cfg saying "enabled" while the runtime model is still
// "disabled" (or vice versa).
// ============================================================================

extern Config g_cfg;
extern std::mutex g_cfgMutex;
void snapshotConfig(Config& out);
extern Config g_preEmergencyCfg;
extern std::string g_preEmergencyProfile;
extern bool g_preEmergencyAutoProfile;
extern std::string g_currentProfile;
extern ProfileType g_currentProfileEnum;

void loadConfig();
void loadProfile(const std::string& profile);

// Config hot-reload
extern std::atomic<bool> g_reloadConfigRequested;
void startConfigHotReload();
// stop the inotify thread gracefully.
// Sets running=false; the select() loop has a 1s timeout so the thread exits
// within at most 1 second. The thread itself does inotify_rm_watch + close(fd).
// Must be called from the daemon shutdown path (after main loop breaks).
void stopConfigHotReload();
extern time_t g_configMtime;
void reloadConfigIfChanged();

// Per-app config
extern std::unordered_map<std::string, PerAppConfig> g_perAppConfigs;
extern PerAppConfig g_currentAppConfig;
// P0-2: dedicated leaf mutex protecting g_perAppConfigs and g_currentAppConfig.
// loadPerAppConfigs() (called under g_cfgMutex) clears/repopulates the map;
// checkAndApplyPerAppConfig() (called from the daemon hot loop) reads it.
// Without this, the hot-loop find() raced loadPerAppConfigs()'s clear() on the
// unordered_map -> SIGSEGV. This is a leaf lock: it is taken AFTER g_cfgMutex
// in the reload path and NEVER nests into g_cfgMutex.
extern std::mutex g_perAppMutex;
void loadPerAppConfigs(const char* confPath);
void checkAndApplyPerAppConfig(const char* foregroundApp);
bool isPackageInGameList(const std::string& pkg);

// Refresh rate mode enum
extern RefreshRateMode g_refreshRateModeEnum;

// Dynamic scoring weights
extern DynamicScoreWeights g_dynamicScoreWeights;
extern std::mutex g_dynamicWeightsMutex;

// Score fusion
float softmaxFuseScores(float modelScore, float heuristicScore, float temperature);
extern float g_fusionTemperature;

// Enum conversion
RefreshRateMode parseRefreshRateMode(const std::string& s);
ProfileType parseProfileType(const std::string& s);
const char* refreshRateModeToString(RefreshRateMode m);
const char* profileTypeToString(ProfileType p);

// ============================================================================
// ROUND-004: Config reload hook registry.
//
// Subsystems that were historically BOOT-only (eBPF ring pages / poll fallback,
// HTTP server, CPU topology / pools, refresh-rate inotify) register a hook at
// their init. reloadConfig() snapshots the freshly-committed Config and invokes
// every registered hook OUTSIDE g_cfgMutex (so a hook may take leaf locks but
// must NOT re-take g_cfgMutex). Each hook is wrapped in try/catch + LOG_WARN so
// one failing subsystem never aborts the rest of the reload path.
//
//   aatpe::config::registerReloadHook("ebpf", [](const Config& cfg){ ... });
//
// Hooks fire on: boot load path (via reloadConfig) and every hot-reload
// (inotify / mtime / reload_config command).
// ============================================================================
namespace aatpe { namespace config {
void registerReloadHook(const std::string& name,
                        std::function<void(const Config&)> hook);
}}

#endif // AATPED_CONFIG_H
