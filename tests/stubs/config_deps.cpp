// config_deps.cpp - host link deps for aatped_config.cpp (does NOT use
// v94_daemon_deps, because config.cpp itself DEFINES snapshotConfig which that
// stub also defines). Provides g_cfg/cfgMutex/STATE_DIR/writeFile + the hybrid
// mutex. hmac + hybrid TUs are linked directly in the recipe for the rest.
#include "aatped.h"

Config g_cfg{};
std::mutex g_cfgMutex;
const char* STATE_DIR = "/tmp/aatpe_v94_state";
const char* CONF_DIR  = "/tmp/aatpe_v94_state";
// fix-v96 #8: getModuleDir() is provided by log_stub (linked into this recipe).

std::atomic<bool> g_reloadConfigRequested{false};
std::atomic<sig_atomic_t> g_reloadProfile{0};
std::atomic<unsigned long> g_mlSaveSuccess{0};
std::atomic<unsigned long> g_mlSaveFail{0};
std::mutex g_hybridMutex;

RefreshRateMode g_refreshRateModeEnum = RefreshRateMode::PEAK;
ProfileType g_currentProfileEnum = ProfileType::BALANCED;
std::string g_currentProfile = "balanced";

void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}
bool atomicWriteFile(const char*, const std::string&) { return true; }

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
bool writeFile(const char*, float) { return true; }
bool writeFile(const char*, long long) { return true; }
// v6.0.0 N-01: persistConfigKeyValue now delegates to the canonical
// writeFileAtomic(). This stub marks the success path for the config unit test;
// the REAL fsync-failure / no-publish / no-tmp-left-behind contract is covered
// end-to-end by tests/qa/fault_inject.cpp FI-23 (links the real aatped_utils.o
// with the g_fi_fsync_fail seam).
bool writeFileAtomic(const char*, const std::string&) { return true; }

// ML model singleton + mutex (normally in aatped_main.cpp / state).
std::mutex g_mlMutex;
aatpe_ml::LearningModel g_mlModel;

// C1/C3 feature switches (normally in aatped_main.cpp)
std::atomic<bool> preboostEnabled{false};
std::atomic<bool> thermalSmoothEnabled{false};
// W5 scene_infer + C2 envelope switches
std::atomic<bool> sceneInferEnabled{false};
std::atomic<bool> c2EnvelopeEnabled{false};
