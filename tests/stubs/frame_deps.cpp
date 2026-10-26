// frame_deps.cpp - host link deps for frame.cpp.
#include "aatped.h"

std::atomic<unsigned int> g_debugSampleCounter{0};
std::atomic<bool> g_helperApkInstalled{false};
// round-4: referenced by frame.cpp via aatpe_utils.h.
std::atomic<bool> g_helperAutoSuppressed{false};
std::atomic<bool> g_frameCritical{false};
RefreshRateMode g_refreshRateModeEnum = RefreshRateMode::PEAK;
bool isPackageInGameList(const std::string&) { return false; }

Config g_cfg{};
std::mutex g_cfgMutex;
// P0-5: frame.cpp's aatpeGetForegroundPackage()/aatpeSetForegroundLocked() now
// take the shared g_foregroundMutex (defined in aatped_ebpf_agg.cpp on device).
// g_foregroundApp itself is defined in frame.cpp, so do NOT define it here.
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
// v95-fix: independent safety-latency gate referenced by frame.cpp.
std::atomic<bool> g_safetyLatencyActive{false};
std::atomic<bool> g_profileLock{false};
// 问题4 fix: frame.cpp now reads g_currentAppConfig.maxFpsHint.
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

// #8/#9: the hardened exec (aatpeExec / aatpeExecArgv) and dumpsys cooldown state have
// moved from frame.cpp into aatped_utils.cpp. The `frame` host test links frame.o
// WITHOUT aatped_utils.o, so provide neutral stubs here (frame.cpp itself only calls
// them; it no longer defines them).
std::string aatpeExec(const std::string&, const char*) { return std::string(); }
std::string aatpeExecArgv(const std::vector<std::string>&, const char*, int*, bool) { return std::string(); }
bool dumpsysAvailable() { return true; }
void triggerDumpsysCooldown() {}

// W2 gate: g_perAppMutex is DEFINED in aatped_config.cpp (device build). The frame
// host test links frame.o WITHOUT aatped_config.o; frame.cpp now references it via
// extern (was a duplicate defining decl that broke the daemon link). Provide the
// single host definition here so the hermetic link resolves.
std::mutex g_perAppMutex;

// v4.3.2 R7: peekCachedLabel/labelForPackage/listInstalledPackagesAllUsers now come
// from the real aatped_apklabel.o (linked by frame host TUs). The old empty stubs are
// removed to avoid multiple-definition when both frame_deps and aatped_apklabel link.
#include <string>

// v4.3.1 hotfix: the detached label-warmup thread polls g_shouldExit to stop.
// Defined in aatped.cpp on device; provide a host definition so the frame TU links.
#include <atomic>
std::atomic<sig_atomic_t> g_shouldExit{0};
