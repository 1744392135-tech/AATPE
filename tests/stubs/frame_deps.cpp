#include "aatped.h"

std::atomic<unsigned int> g_debugSampleCounter{0};
std::atomic<bool> g_helperApkInstalled{false};
std::atomic<bool> g_helperAutoSuppressed{false};
std::atomic<bool> g_frameCritical{false};
RefreshRateMode g_refreshRateModeEnum = RefreshRateMode::PEAK;
bool isPackageInGameList(const std::string&) { return false; }

Config g_cfg{};
std::mutex g_cfgMutex;
std::mutex g_foregroundMutex;
const char* STATE_DIR = "/tmp/aatpe_v94_state";
void snapshotConfig(Config& out) { out = g_cfg; }
#include <fstream>
std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return "";
    std::getline(f, s); return s;
}
int readInt(const char* path, int def) {
    std::ifstream f(path); int v; if (f >> v) return v; return def;
}
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int) { return true; }
bool writeFile(const char*, long long) { return true; }
std::string g_currentProfile;
void loadProfile(const std::string&) {}
std::atomic<bool> g_thermalEmergency{false};
std::atomic<bool> g_safetyLatencyActive{false};
std::atomic<bool> g_profileLock{false};
PerAppConfig g_currentAppConfig;

int aatpeAndroidSdkInt() { return 30; }
const char* aatpeAndroidVersionName() { return "12"; }
std::string aatpeCpusetMountRoot() { return "/dev/cpuset"; }
bool aatpeIsCgroupV2Cpuset() { return false; }
int lightweightMigrateCriticalThreads() { return 0; }

void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}

std::string aatpeExec(const std::string&, const char*) { return std::string(); }
std::string aatpeExecArgv(const std::vector<std::string>&, const char*, int*, bool) { return std::string(); }
bool dumpsysAvailable() { return true; }
void triggerDumpsysCooldown() {}

std::mutex g_perAppMutex;

#include <string>

#include <atomic>
std::atomic<sig_atomic_t> g_shouldExit{0};
