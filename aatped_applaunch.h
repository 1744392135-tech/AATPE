#ifndef AATPED_APPLAUNCH_H
#define AATPED_APPLAUNCH_H

#include <string>
#include <unordered_map>
#include <mutex>

struct AppLaunchRecord {
    std::string packageName;
    int mainThreadTid = 0;
    int renderThreadTid = 0;
    int preferredCpu = 4;
    long long lastLaunchTime = 0;
};
extern std::unordered_map<std::string, AppLaunchRecord> g_appLaunchHistory;
extern std::mutex g_appLaunchHistoryMutex;
void recordAppLaunch(const char* packageName, int mainTid, int renderTid);
void checkAndPreloadNewApps(void);
int getPreloadedPreferredCpu(const char* packageName);

#endif
