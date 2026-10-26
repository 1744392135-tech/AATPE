// poc_stub.cpp — host stubs to link the REAL webui_http.cpp + real aatped_hmac.cpp
// + real aatpe_session_token.cpp + real aatpe_command_queue.cpp into a PoC binary.
// This file does NOT weaken production security logic; it only provides the
// Android/daemon-environment symbols that the test host cannot supply.
#include "aatped.h"
#include "aatped_control.h"
#include "aatped_logging.h"
#include "aatpe_command_dispatcher.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <utility>

// --- directory roots (overridden by main() to point at a temp sandbox) ---
const char* RUN_DIR   = "/tmp/aatpe_poc_run";
const char* STATE_DIR = "/tmp/aatpe_poc_state";
const char* DECISION_DIR = "/tmp/aatpe_poc_state";
const char* WEBUI_DIR = "/tmp/aatpe_poc_state";

std::atomic<int> g_logLevel{3};
std::atomic<unsigned int> g_debugSampleCounter{0};

// global config + lock (real Config type from aatped_types.h)
Config g_cfg{};
std::mutex g_cfgMutex;
// live SSE connection counter (real one lives in aatped_main.cpp)
std::atomic<int> g_sseClientCount{0};

// G3-001 integrity write gate: the PoC targets HMAC/nonce/path-traversal, not the
// boot-time manifest verify. Keep privileged ops allowed so the control surface
// under test is reachable; the fail-closed (Unknown=>403) gate itself is pinned
// end-to-end by the integration suite (S6). This does NOT weaken the real gate.
namespace aatpe { namespace integrity {
    bool privilegedOpsAllowed() { return true; }
}}

void logInit() {} void logReinit() {} void logFlush() {} void logShutdown() {} void logClear() {}
void logMsg(int level, const char* fmt, ...) {
    (void)level;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "  [log] %s\n", buf);
}

// --- file helpers ---
bool writeFile(const char* path, const std::string& content) {
    FILE* f = fopen(path, "w"); if (!f) return false;
    fwrite(content.data(), 1, content.size(), f); fclose(f); return true;
}
bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    { std::ofstream o(tmp, std::ios::binary); if (!o) return false; o << content; }
    return ::rename(tmp.c_str(), path) == 0;
}
std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return s;
    std::getline(f, s, '\0'); return s;
}
void mkdirIfNotExist(const char* path, mode_t mode) { ::mkdir(path, mode); }

std::string getModuleDir() { return "/tmp/aatpe_poc_module"; }

// --- state cache (used by buildAggregateStateJson / /state/public) ---
bool stateCacheIsSeeded() { return false; }
void stateCacheSeedFromDir(const char*) {}
uint64_t stateCacheGeneration() { return 1; }
std::vector<std::pair<std::string,std::string>> stateCacheSnapshot() {
    std::vector<std::pair<std::string,std::string>> out;
    out.push_back({"system_state", "running"});
    out.push_back({"control_mode", "assist"});
    out.push_back({"thermal_cpu", "45000"});
    return out;
}

// --- command queue drain side (only status/echo is exercised over HTTP) ---
ControlResult executeControlLine(const std::string& line) {
    ControlResult r; r.success = true; r.message = "applied:" + line;
    r.applied_value = line; r.error_code = 0; return r;
}
bool isValidControlChars(const std::string& s) {
    for (char c : s) {
        if (!( (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='='||c=='.'||c=='-' )) return false;
    }
    return !s.empty();
}

// --- host env shims added so the REAL webui_http.cpp + aatped_hmac.cpp link ---
// cleanly on the test host (these are device-daemon symbols, not security logic).
// diag_bundle exec: never actually runs tar on host; record-nothing, return empty.
std::string aatpeExecArgv(const std::vector<std::string>&, const char*, int*, bool) {
    return std::string();
}
// /api/frame_apps payload: empty fixture on host.
std::string aatpe_buildFrameAppsJson() { return "{\"current\":\"\",\"apps\":[]}"; }
// errno->string (strerror_r wrapper used by aatped_hmac init).
const char* aatpeStrError(int err, char* buf, size_t n) {
    if (!buf || n == 0) return "";
    if (!strerror_r(err, buf, n)) {}
    buf[n-1] = '\0';
    return buf;
}
