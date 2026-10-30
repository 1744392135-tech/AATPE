#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <string>
#include "aatped.h"
#include "aatped_config.h"
#include "aatped_state.h"
#include "aatped_topology.h"
#include "aatpe_ml.h"
#include "aatped_ebpf_agg.h"

namespace aatpe_ml { extern LearningModel g_mlModel; }

Config g_cfg{};
std::mutex g_cfgMutex;

std::atomic<sig_atomic_t> g_shouldExit{0};
std::atomic<sig_atomic_t> g_reloadProfile{0};
std::atomic<bool> g_profileLock{false};
std::atomic<bool> g_userModeLock{false};
std::atomic<bool> g_watchdogPassiveLocked{false};
std::atomic<bool> g_helperApkInstalled{false};
std::atomic<bool> g_helperAutoSuppressed{false};
std::string g_autoProfileMode = "balanced";

std::vector<int> g_allCpus{0,1,2,3,4,5,6,7};
int g_cpuCount = 8;
std::mutex g_topologyRwMutex;

aatpe_ml::LearningModel g_mlModel;
std::mutex g_mlMutex;
std::mutex g_hybridMutex;
std::atomic<unsigned long> g_mlSaveSuccess{0}, g_mlSaveFail{0};

PerAppConfig g_currentAppConfig{};
std::mutex g_perAppMutex;
RefreshRateMode g_refreshRateModeEnum{};
std::mutex g_foregroundMutex;
std::atomic<bool> g_thermalEmergency{false};
std::atomic<bool> g_frameCritical{false};
std::atomic<bool> g_safetyLatencyActive{false};
std::atomic<int> g_sseClientCount{0};
bool isPackageInGameList(const std::string&) { return false; }
void snapshotConfig(Config& out) { out = g_cfg; }
int lightweightMigrateCriticalThreads() { return 0; }

const char* RUN_DIR = "/tmp/aatpe_fuzz_run";
const char* LOG_FILE = "/tmp/aatpe_fuzz.log";

bool transitionToState(SystemState, const char*) { return true; }
SystemState getSystemState() { return SystemState::NORMAL; }
const char* systemStateToString(SystemState) { return "NORMAL"; }
void loadProfile(const std::string&) {}
void loadConfig() {}
std::vector<int> getOnlineCpus() { return g_allCpus; }
void persistConfigKeyValue(const char*, const char*) {}
bool isWatchdogPassiveLocked() { return g_watchdogPassiveLocked.load(); }
void backupModelFile(const char*) {}
const char* mlModelPath() { return ""; }
void updateMlSaveStateFiles(bool, const char*) {}
void placementInvalidateSettle(int) {}

extern "C" int __system_property_get(const char*, char*) { return 0; }

namespace aatpe { namespace webui_http {
__attribute__((weak)) void auditSensitiveCommand(const std::string&, const std::string&, bool, const std::string&) {}
}}
