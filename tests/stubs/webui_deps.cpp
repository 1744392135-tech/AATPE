#include "aatped.h"
#include "aatped_control.h"
#include <string>
#include <fstream>
#include <sys/stat.h>
const char* WEBUI_DIR = "/tmp/webui_test_dir";
const char* DECISION_DIR = "/tmp/webui_test_dir";
bool writeFile(const char* path, const std::string& content){FILE*f=fopen(path,"w"); if(!f)return false; fwrite(content.data(),1,content.size(),f); fclose(f); return true;}

// round-4: webui_http now routes /control through the command queue
// (aatpe_command_queue.cpp). Provide the small set of file/exec helpers it needs
// so this hermetic webui_http test does not have to pull in the whole control TU.
bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f << content; }
    return ::rename(tmp.c_str(), path) == 0;
}
std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return std::string();
    std::getline(f, s, '\0'); return s;
}
void mkdirIfNotExist(const char* path, mode_t mode) { ::mkdir(path, mode); }
ControlResult executeControlLine(const std::string& line) {
    ControlResult r; r.success = true; r.message = "stub:" + line; return r;
}

// added for webui_http.cpp standalone test.
// R-W3: per-process unique RUN_DIR. The old fixed /tmp/aatpe_v94_run was shared
// across webui_http/webui_authz runs; leftover commands in RUN_DIR/commands would
// accumulate across runs (no drainer on host), and once the bounded queue filled,
// /control writes hit the product's correct back-pressure (ERR_BUSY -> HTTP 429,
// webui_http.cpp) instead of being applied. mkdtemp gives each webui test
// process a fresh empty mailbox so cross-run residue can never collide.
#include <unistd.h>
#include <cstring>
static const char* aatpeInitWebuiRunDir() {
    char tmpl[] = "/tmp/aatpe_v94_run.XXXXXX";
    if (::mkdtemp(tmpl) != nullptr) return ::strdup(tmpl);
    return "/tmp";
}
const char* RUN_DIR = aatpeInitWebuiRunDir();
char g_hmacKey[128] = {0};
std::mutex g_hmacMutex;
bool g_hmacActive = false;
bool g_hmacReadonlyFlag = true;  // default read-only (back-compat for webui_http ROUND-003)
bool isHmacReadonly() { return g_hmacReadonlyFlag; }
bool verifyCommandHmac(const char*, const char*) { return true; }

// live SSE connection counter (real one lives in aatped_main.cpp,
// not linked into this hermetic webui_http test).
std::atomic<int> g_sseClientCount{0};

std::string aatpeExec(const std::string&, const char*) { return std::string(); }
// P0 /api/diag_bundle uses the hardened argv exec; host test returns empty.
// v4.2.9 T5: record the argv it was handed so the hermetic test can assert the
// diag_bundle whitelist contents (the real tar/exec never runs on host). This is
// a test-only observation hook; return value is unchanged (empty string).
std::vector<std::string> g_lastExecArgv;
std::string aatpeExecArgv(const std::vector<std::string>& argv, const char*, int*, bool) {
    g_lastExecArgv = argv;
    return std::string();
}
Config g_cfg{};
std::mutex g_cfgMutex;
const char* STATE_DIR = "/tmp/aatpe_v94_state";

// opt#1 host-stub: webui_http.cpp's buildAggregateStateJson now calls these
// instead of walking STATE_DIR itself. On the host the hermetic test links
// webui_http.cpp but NOT aatped_utils.cpp, so we provide equivalent semantics here:
// always re-read from disk (the test only checks the JSON shape).
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <utility>
bool stateCacheIsSeeded() { return false; }
void stateCacheSeedFromDir(const char*) {}
// #20/#22: host-test stand-in for the state generation counter (real one lives in
// aatped_utils.cpp, not linked into this hermetic webui_http test).
#include <cstdint>
uint64_t stateCacheGeneration() { return 0; }
std::vector<std::pair<std::string,std::string>> stateCacheSnapshot() {
    std::vector<std::pair<std::string,std::string>> out;
    DIR* d = opendir(STATE_DIR);
    if (!d) return out;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0]=='.') continue;
        std::string full = std::string(STATE_DIR) + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(),&st)==0 && S_ISREG(st.st_mode)) {
            int fd = open(full.c_str(), O_RDONLY);
            if (fd>=0) {
                char buf[4096]; std::string val; ssize_t n;
                while ((n=read(fd,buf,sizeof(buf)))>0) val.append(buf,n);
                close(fd);
                while (!val.empty() && (val.back()=='\n'||val.back()=='\r'||val.back()==' '||val.back()=='\t')) val.pop_back();
                out.push_back({ent->d_name, val});
            }
        }
    }
    closedir(d);
    return out;
}

// W2: handleFrameApps() in webui_http.cpp calls aatpe_buildFrameAppsJson(), which is
// DEFINED in webui_json.cpp on device. This hermetic webui_http test does NOT link
// webui_json.o, so provide a neutral stub (empty payload). The real serialization is
// exercised by test_webui_json (which DOES link webui_json.cpp).
std::string aatpe_buildFrameAppsJson() { return "{\"current\":\"\",\"apps\":[]}"; }
