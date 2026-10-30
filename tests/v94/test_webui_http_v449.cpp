#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "webui_http.h"
#include "aatped.h"
#include "integrity.h"
#include "aatped_config.h"

std::string aatpeHostHandleControl(const std::string& cmd);
std::string aatpeHostHandleDiagBundle();

extern bool g_hmacReadonlyFlag;
extern const char* STATE_DIR;
extern std::vector<std::string> g_lastExecArgv;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while (0)

static int statusOf(const std::string& resp) {
    if (resp.size() < 12 || resp.compare(0,5,"HTTP/") != 0) return -1;
    size_t sp = resp.find(' ');
    if (sp == std::string::npos) return -1;
    return atoi(resp.c_str() + sp + 1);
}

int main() {
    printf("=== F-A6-01: downgrade-mode replay protection ===\n");
    aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
    g_hmacReadonlyFlag = false;
    g_cfg.hmacRequired = false;

    long long now = (long long)time(nullptr);

    std::string w1 = "set_mode=active.nonce=n1.ts=" + std::to_string(now) + ".sig=abc";
    int s1 = statusOf(aatpeHostHandleControl(w1));
    CHECK(s1 == 200, "downgrade signed in-window write accepted (200)");

    int s2 = statusOf(aatpeHostHandleControl(w1));
    CHECK(s2 == 403, "replayed signed write rejected (403)");

    std::string oldTs = "set_mode=active.nonce=n2.ts=" + std::to_string(now - 3600) + ".sig=abc";
    int s3 = statusOf(aatpeHostHandleControl(oldTs));
    CHECK(s3 == 403, "out-of-window timestamp rejected (403)");

    std::string bare = "set_mode=active.sig=abc";
    int s4 = statusOf(aatpeHostHandleControl(bare));
    CHECK(s4 == 403, "bare-signature write rejected (403)");

    int s5 = statusOf(aatpeHostHandleControl("set_mode=active"));
    CHECK(s5 == 403, "unsigned write rejected in downgrade mode (403)");

    printf("=== F-A6-02: diag_bundle private path / symlink safety ===\n");
    mkdir(STATE_DIR, 0755);
    std::string sentinel = std::string(STATE_DIR) + "/sentinel.txt";
    { std::ofstream(sentinel) << "DO_NOT_TOUCH"; }
    std::string diagPath = std::string(STATE_DIR) + "/aatpe-diag.tgz";
    unlink(diagPath.c_str());
    symlink(sentinel.c_str(), diagPath.c_str());

    std::string diagResp = aatpeHostHandleDiagBundle();
    bool inTmp = false, inState = false;
    for (auto& a : g_lastExecArgv) {
        if (a.find("/data/local/tmp/aatpe-diag") != std::string::npos) inTmp = true;
        if (a == diagPath) inState = true;
    }
    CHECK(!inTmp, "diag tar output NOT in /data/local/tmp");
    CHECK(inState, "diag tar output in private STATE_DIR");

    std::ifstream sf(sentinel); std::string sc; sf >> sc;
    CHECK(sc == "DO_NOT_TOUCH", "planted symlink not followed (sentinel intact)");
    unlink(diagPath.c_str());

    printf("=== F-A6-08: shared jsonEscape sanity ===\n");
    std::string e = jsonEscape("a\"b\\c\n");
    CHECK(e.find("\\\"") != std::string::npos && e.find("\\\\") != std::string::npos,
          "shared jsonEscape escapes quote/backslash/newline");

    printf("=== RESULT: pass=%d fail=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
