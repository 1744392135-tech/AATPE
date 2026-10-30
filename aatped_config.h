#ifndef AATPED_CONFIG_H
#define AATPED_CONFIG_H

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

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

extern std::atomic<bool> g_reloadConfigRequested;
void startConfigHotReload();
void stopConfigHotReload();
extern time_t g_configMtime;
void reloadConfigIfChanged();

extern std::unordered_map<std::string, PerAppConfig> g_perAppConfigs;
extern PerAppConfig g_currentAppConfig;
extern std::mutex g_perAppMutex;
void loadPerAppConfigs(const char* confPath);
void checkAndApplyPerAppConfig(const char* foregroundApp);
bool isPackageInGameList(const std::string& pkg);

extern RefreshRateMode g_refreshRateModeEnum;

extern DynamicScoreWeights g_dynamicScoreWeights;
extern std::mutex g_dynamicWeightsMutex;

float softmaxFuseScores(float modelScore, float heuristicScore, float temperature);
extern float g_fusionTemperature;

RefreshRateMode parseRefreshRateMode(const std::string& s);
ProfileType parseProfileType(const std::string& s);
const char* refreshRateModeToString(RefreshRateMode m);
const char* profileTypeToString(ProfileType p);

namespace aatpe { namespace config {
void registerReloadHook(const std::string& name,
                        std::function<void(const Config&)> hook);
}}

#endif
