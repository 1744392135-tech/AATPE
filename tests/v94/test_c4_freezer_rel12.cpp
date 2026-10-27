// test_c4_freezer_rel12.cpp — rel12 Bug6 regression.
//
// On the rel14 device the C4 panel read "freezer: 不可用" even though the
// diagnostic (c4_rootcause.txt) proved the v1 freezer hierarchy IS present and
// mounted (/sys/fs/cgroup/freezer). The conflation: "freezer backend available"
// was derived from "we currently have a freezable BACKGROUND target". With the
// foreground being the SukiSU WebUI itself (non_game), there is no target, so the
// UI read as "不可用" -- which the user reasonably interpreted as "the freezer
// driver is missing", when it actually means "nothing to freeze right now".
//
// This test proves the production c4FreezerStateJson reports the v1 backend as
// AVAILABLE (freezerAvailable=true, freezerBackend="v1") even when NO game has
// ever been foregrounded (target empty). The only degraded reason may then be
// "no-game-target", never "no-freezer-controller".
//
// Compiled in production mode (-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM) so the
// real c4FreezerTick / c4FreezerStateJson bodies run.

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

const char* STATE_DIR = "/tmp/aatpe_c4rel12_state";

namespace aatpe { namespace c4freezer {
extern std::atomic<int> g_c4freezerLockDepth;
void c4FreezerTestSetMountRoot(const std::string& root);
}}

static std::string g_fg = "com.sukisu.ultra";  // the WebUI itself, NOT a game
static bool        g_fgIsGame = false;

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

// v6.0.1: C4 now also consults the config game list (global namespace).
bool isPackageInGameList(const std::string&) { return false; }

std::string aatpeExec(const std::string& cmd, const char*) {
    (void)cmd;  // no game -> no uid resolution needed
    // media_session shows nothing playing for our target.
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
    const char* p = content.data(); size_t left = content.size();
    while (left > 0) { ssize_t w = write(fd, p, left); if (w <= 0) { close(fd); return false; } p += w; left -= (size_t)w; }
    close(fd); return true;
}

static int s_pass = 0, s_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

static void writeFileStd(const std::string& path, const std::string& content) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, content.data(), content.size()); close(fd); }
}
static void rmrf(const std::string& path) { std::string cmd = "rm -rf " + path; system(cmd.c_str()); }

int main() {
    printf("=== C4 rel12 Bug6: v1 freezer present, NO game target ===\n");

    // Fake v1 freezer hierarchy (v2 absent). Proves v1FreezerPresent().
    std::string root = "/tmp/aatpe_c4rel12_fakecg";
    rmrf(root);
    mkdir(root.c_str(), 0755);
    mkdir((root + "/freezer").c_str(), 0755);
    writeFileStd(root + "/freezer/freezer.state", "THAWED\n");
    // NO v2 nodes -> v2 unavailable, v1 is the backend.

    c4FreezerTestSetMountRoot(root);

    rmrf(STATE_DIR);
    mkdir(STATE_DIR, 0700);
    setC4FreezerEnabled(true);
    CHECK(c4FreezerEnabled(), "freezer enabled");

    // Foreground = WebUI itself (non_game). Tick. There is NO game target.
    g_fg = "com.sukisu.ultra";
    g_fgIsGame = false;
    c4FreezerTick();

    std::string j = c4FreezerStateJson();
    printf("  snapshot: %s\n", j.c_str());

    // The v1 backend must be reported as AVAILABLE despite the empty target.
    CHECK(j.find("\"freezerAvailable\":true") != std::string::npos,
          "Bug6: freezerAvailable=true even with no freezable target");
    CHECK(j.find("\"freezerBackend\":\"v1\"") != std::string::npos,
          "Bug6: freezerBackend=v1 (independent of current target)");
    // No target -> degraded may be "no-game-target", but must NOT claim the
    // freezer controller itself is missing.
    CHECK(j.find("no-freezer-controller") == std::string::npos,
          "Bug6: degraded is not 'no-freezer-controller' (v1 really is present)");
    CHECK(j.find("\"target\":\"\"") != std::string::npos,
          "Bug6: target empty (nothing to freeze right now)");

    printf("\n=== rel12 c4 result: pass=%d fail=%d ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
