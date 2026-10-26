// test_c4_freezer_v1.cpp — v6.1.0 regression: cgroup v1 freezer fallback.
//
// Compiled in production mode (WITHOUT -DAATPE_HOST_TEST, with
// -DC4_FREEZER_TEST_SEAM) so the real c4FreezerTick / c4FreezerStateJson bodies
// run. A fake v1 freezer filesystem is created under /tmp and the production
// mount root is redirected via the C4_FREEZER_TEST_SEAM hook
// c4FreezerTestSetMountRoot().
//
// Covers:
//   T-V1-A: when v2 freezer is absent (no cgroup.controllers, no cgroup.freeze)
//           but v1 freezer hierarchy exists, the glue auto-selects v1 backend.
//   T-V1-B: a backgrounded game is frozen via v1: freezer.state = FROZEN and
//           the kernel read-back confirms FROZEN.
//   T-V1-C: when the game returns to foreground, the v1 freezer.state is set
//           to THAWED and the read-back confirms THAWED.
//   T-V1-D: the state JSON reports "freezerBackend":"v1".

#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include "c4_freezer.h"
#include "aatpe_managers.h"

using namespace aatpe;
using namespace aatpe::c4freezer;

// ---- test-only state ----------------------------------------------------------
const char* STATE_DIR = "/tmp/aatpe_c4v1_state";

namespace aatpe { namespace c4freezer {
extern std::atomic<int> g_c4freezerLockDepth;
// Test seam (defined in c4_freezer.cpp under C4_FREEZER_TEST_SEAM).
void c4FreezerTestSetMountRoot(const std::string& root);
}}

static std::string g_fg = "com.android.launcher";
static bool        g_fgIsGame = false;

// ---- stubs for the production glue's external deps -----------------------------
ForegroundManager& ForegroundManager::instance() {
    static ForegroundManager inst;
    return inst;
}
std::string ForegroundManager::getPackage() const { return g_fg; }
void ForegroundManager::setPackage(const std::string&) {}
time_t ForegroundManager::startTime() const { return 0; }

namespace aatpe { namespace gamelist {
bool isManualGame(const std::string&) { return g_fgIsGame; }
}}

// Smart exec stub:
//   "pm list packages -U com.game" -> "package:com.game uid:10123\n"
//   "dumpsys media_session"       -> non-playing session for a DIFFERENT pkg
std::string aatpeExec(const std::string& cmd, const char*) {
    if (cmd.find("pm list packages -U com.game") != std::string::npos) {
        return "package:com.game uid:10123\n";
    }
    // media_session: com.music.app paused (state=2), our target not playing.
    return "Session #1:\n  package=com.music.app\n  playback state=2\n";
}

std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

bool writeFileAtomic(const char* path, const std::string& content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const char* p = content.data();
    size_t left = content.size();
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) { close(fd); return false; }
        p += w; left -= (size_t)w;
    }
    close(fd);
    return true;
}

// ---- helpers -------------------------------------------------------------------
static int s_pass = 0, s_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

static std::string readFileStd(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

static void writeFileStd(const std::string& path, const std::string& content) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, content.data(), content.size()); close(fd); }
}

static void rmrf(const std::string& path) {
    std::string cmd = "rm -rf " + path;
    system(cmd.c_str());
}

int main() {
    printf("=== C4 freezer v1 fallback: fake v1 fs, v2 absent ===\n");

    // ---- Build the fake v1 freezer filesystem ---------------------------------
    std::string root = "/tmp/aatpe_c4v1_fakecg";
    rmrf(root);
    mkdir(root.c_str(), 0755);
    mkdir((root + "/freezer").c_str(), 0755);
    mkdir((root + "/freezer/uid_10123").c_str(), 0755);
    // Top-level freezer.state (proves v1FreezerPresent()).
    writeFileStd(root + "/freezer/freezer.state", "THAWED\n");
    // Target app cgroup freezer.state (initially THAWED).
    writeFileStd(root + "/freezer/uid_10123/freezer.state", "THAWED\n");
    // NO v2 nodes: no cgroup.controllers, no cgroup.freeze -> v2 unavailable.

    // ---- Redirect production mount root to the fake tree ----------------------
    c4FreezerTestSetMountRoot(root);

    // ---- Fresh STATE_DIR ------------------------------------------------------
    rmrf(STATE_DIR);
    mkdir(STATE_DIR, 0700);
    setC4FreezerEnabled(true);
    CHECK(c4FreezerEnabled(), "freezer enabled");

    // ---- Tick 1: game in foreground (priming lastGame) ------------------------
    printf("--- T-V1 setup: game foreground primes target ---\n");
    g_fg = "com.game";
    g_fgIsGame = true;
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        printf("  t1 snapshot: %s\n", j.c_str());
        // Game in foreground: must NOT be frozen.
        CHECK(j.find("\"frozen\":false") != std::string::npos,
              "T-V1-1: game foreground -> not frozen");
    }

    // ---- Tick 2: game backgrounds (launcher foreground) -> v1 FREEZE -----------
    printf("--- T-V1-A/B: launcher foreground -> game backgrounds -> v1 freeze ---\n");
    g_fg = "com.android.launcher";
    g_fgIsGame = false;
    c4FreezerTick();

    // Check the v1 freezer.state was set to FROZEN by the production glue.
    std::string state = readFileStd(root + "/freezer/uid_10123/freezer.state");
    printf("  freezer.state after bg tick: '%s'\n", state.c_str());
    CHECK(state.find("FROZEN") != std::string::npos,
          "T-V1-B: v1 freezer.state contains FROZEN (write + read-back confirmed)");

    {
        std::string j = c4FreezerStateJson();
        printf("  t2 snapshot: %s\n", j.c_str());
        CHECK(j.find("\"frozen\":true") != std::string::npos,
              "T-V1-B: JSON reports frozen:true");
        CHECK(j.find("\"freezerBackend\":\"v1\"") != std::string::npos,
              "T-V1-A: JSON reports freezerBackend=v1");
        CHECK(j.find("/freezer/uid_10123") != std::string::npos,
              "T-V1-A: cgDir is the v1 freezer path");
    }

    // ---- Tick 3: game returns to foreground -> v1 THAWED ----------------------
    printf("--- T-V1-C: game returns to foreground -> v1 thaw ---\n");
    g_fg = "com.game";
    g_fgIsGame = true;
    c4FreezerTick();

    state = readFileStd(root + "/freezer/uid_10123/freezer.state");
    printf("  freezer.state after fg tick: '%s'\n", state.c_str());
    CHECK(state.find("THAWED") != std::string::npos,
          "T-V1-C: v1 freezer.state contains THAWED (write + read-back confirmed)");

    {
        std::string j = c4FreezerStateJson();
        printf("  t3 snapshot: %s\n", j.c_str());
        CHECK(j.find("\"frozen\":false") != std::string::npos,
              "T-V1-C: JSON reports frozen:false after foreground release");
    }

    printf("\n=== v1 glue result: pass=%d fail=%d ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
