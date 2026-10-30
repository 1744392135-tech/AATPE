#include "aatped.h"
#include <unistd.h>
#include <cstring>

static const char* aatpeInitRunDir() {
    char tmpl[] = "/tmp/aatpe_v94_run.XXXXXX";
    if (::mkdtemp(tmpl) != nullptr) return ::strdup(tmpl);
    return "/tmp";
}
const char* RUN_DIR = aatpeInitRunDir();
std::string g_autoProfileMode = "balanced";
std::atomic<bool> g_helperApkInstalled{false};
std::atomic<sig_atomic_t> g_shouldExit{0};
std::atomic<sig_atomic_t> g_reloadProfile{0};
std::atomic<bool> g_userModeLock{false};
std::atomic<bool> g_watchdogPassiveLocked{false};

bool isWatchdogPassiveLocked() { return g_watchdogPassiveLocked.load(); }
void persistConfigKeyValue(const char*, const char*) {}
bool verifyCommandHmac(const char*, const char*) { return true; }

void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}
bool atomicWriteFile(const char*, const std::string&) { return true; }
bool atomicWriteFile(const char*, int) { return true; }

#include <sys/stat.h>
void mkdirIfNotExist(const char* path, mode_t mode) {
    ::mkdir(path, mode);
}

std::mutex g_hybridMutex;
std::atomic<unsigned long> g_mlSaveSuccess{0}, g_mlSaveFail{0};
std::atomic<int> g_explainTid{0};
void backupModelFile(const char*) {}
void loadConfig() {}
void logClear() {}
const char* mlModelPath() { return ""; }
void updateMlSaveStateFiles(bool, const char*) {}

#include "aatped_state.h"
const char* systemStateToString(SystemState s) {
    switch (s) {
        case SystemState::NORMAL: return "NORMAL";
        case SystemState::DEGRADED: return "DEGRADED";
        case SystemState::PASSIVE: return "PASSIVE";
        case SystemState::EMERGENCY: return "EMERGENCY";
        default: return "?";
    }
}

namespace aatpe { namespace webui_http {
void auditSensitiveCommand(const std::string&, const std::string&, bool, const std::string&) {}
}}
