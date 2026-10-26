// control_deps.cpp - host link deps for aatped_control.cpp (on top of
// v94_daemon_deps, which covers most globals). Fills the handful of symbols
// control.cpp references that live in other TUs on device.
#include "aatped.h"
#include <unistd.h>
#include <cstring>

// Per-process unique RUN_DIR. The old fixed "/tmp/aatpe_v94_run" was shared
// across targets; when v94 targets ran concurrently a leftover control_cmd (or
// commands/ fifo) from one process would be picked up by another's
// processControlCommands(), making the "no-op leaves controlMode unchanged"
// assertion flaky-fail. mkdtemp() hands each test process its own empty dir,
// created at load time and reused for the whole process. Fall back to /tmp if
// mkdtemp ever fails (never on the host runner).
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
// P3-1 flag g_emergencyUserOverride is intentionally NOT defined here: every
// recipe linking control_deps (control/cmdq/cmdq_v449) also links
// v94_daemon_deps, which is the single owner of this symbol. Defining it here
// too causes a multiple-definition link error.

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

// round-4: command queue needs a real mkdir for its directory.
#include <sys/stat.h>
void mkdirIfNotExist(const char* path, mode_t mode) {
    ::mkdir(path, mode);  // best-effort; EEXIST is fine
}

// control.cpp also reaches into hybrid/config/main on device; neutral stubs here.
std::mutex g_hybridMutex;
unsigned long g_mlSaveSuccess = 0, g_mlSaveFail = 0;
std::atomic<int> g_explainTid{0};
void backupModelFile(const char*) {}
void loadConfig() {}
void logClear() {}
const char* mlModelPath() { return ""; }
void updateMlSaveStateFiles(bool, const char*) {}

// P2 stub: systemStateToString lives in aatped_state.cpp on device; provide a
// neutral mapping so control/cmdq tests link without pulling in all of state.cpp.
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

// P3-42: aatpe_command_dispatcher now calls webui_http::auditSensitiveCommand()
// after dispatch. On device this lives in webui_http.cpp; the control/cmdq test
// link does not pull in all of webui_http.cpp (which would drag webui_deps and
// duplicate g_cfgMutex/STATE_DIR against v94_daemon_deps). Provide a best-effort
// no-op stub in the namespace the dispatcher calls into.
namespace aatpe { namespace webui_http {
void auditSensitiveCommand(const std::string&, const std::string&, bool, const std::string&) {}
}}  // namespace aatpe::webui_http
