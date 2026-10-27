// test_c4_freezer_autogame.cpp — v6.0.1 regression for the 8h "no-game-target"
// field bug.
//
// On a real device the user played a game for 8 hours but the C4 panel always
// read "后端可用 · 当前无游戏目标（后台暂无应用可冻）" with degraded code
// "no-game-target". Root cause: c4FreezerTick() set
//   fgIsGame = !fg.empty() && gamelist::isManualGame(fg)
// and isManualGame() only consults /data/adb/aatpe/game_overrides.list, which
// does not exist by default. There is no auto game detection wired in. So with
// zero manual entries, fgIsGame was permanently false, lastGame was never set,
// c4ChooseTarget() always returned "", and the panel sat at no-game-target
// forever.
//
// This test reproduces that world (manual list empty, config list empty) and
// proves the fix: when the foreground app is AUTO-detected as a game (via the
// game-engine-thread signal), it becomes the freeze target after it backgrounds.
// It also proves the negative: with no auto-detection signal, target stays ""
// and the panel reports no-game-target (the safe historical behaviour).
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

const char* STATE_DIR = "/tmp/aatpe_c4autogame_state";

namespace aatpe { namespace c4freezer {
extern std::atomic<int> g_c4freezerLockDepth;
void c4FreezerTestSetMountRoot(const std::string& root);
// v6.0.1 test seam: mark `pkg` as auto-detected (engine threads present).
// Empty string disables the seam (production/host returns false).
void c4FreezerTestSetAutoGamePkg(const std::string& pkg);
}}

static std::string g_fg = "com.android.launcher";

ForegroundManager& ForegroundManager::instance() {
    static ForegroundManager inst;
    return inst;
}
std::string ForegroundManager::getPackage() const { return g_fg; }
void ForegroundManager::setPackage(const std::string&) {}
time_t ForegroundManager::startTime() const { return 0; }

// Reproduce the bug world: manual game list EMPTY.
namespace aatpe { namespace gamelist {
bool isManualGame(const std::string&) { return false; }
}}
// Reproduce the bug world: config game list EMPTY.
bool isPackageInGameList(const std::string&) { return false; }

// Smart exec stub.
//   "pm list packages -U com.auto.game" -> uid 10123
//   anything else (dumpsys media_session) -> a paused session for another pkg.
std::string aatpeExec(const std::string& cmd, const char*) {
    if (cmd.find("pm list packages -U com.auto.game") != std::string::npos) {
        return "package:com.auto.game uid:10123\n";
    }
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
    else { s_pass--; s_fail++; printf("  [FAIL] %s\n", msg); } \
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
    printf("=== C4 v6.0.1: auto engine-thread game detection primes the freeze target ===\n");

    // Fake v1 freezer hierarchy for uid 10123 (the auto-detected game).
    std::string root = "/tmp/aatpe_c4autogame_fakecg";
    rmrf(root);
    mkdir(root.c_str(), 0755);
    mkdir((root + "/freezer").c_str(), 0755);
    mkdir((root + "/freezer/uid_10123").c_str(), 0755);
    writeFileStd(root + "/freezer/freezer.state", "THAWED\n");
    writeFileStd(root + "/freezer/uid_10123/freezer.state", "THAWED\n");
    c4FreezerTestSetMountRoot(root);

    rmrf(STATE_DIR);
    mkdir(STATE_DIR, 0700);
    setC4FreezerEnabled(true);

    // ------------------------------------------------------------------
    // PHASE 0: NEGATIVE — no manual entry, no config entry, no auto signal.
    // This is the 8-hour-bug world. target must stay "" and degraded must
    // be "no-game-target". (If the fix broke this guard, a launcher/app
    // would be wrongly frozen.)
    // ------------------------------------------------------------------
    printf("--- P0: no signal at all -> safe no-target (the old bug world) ---\n");
    c4FreezerTestSetAutoGamePkg("");   // auto-detect seam OFF
    g_fg = "com.auto.game";            // user's game, but we don't recognize it yet
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        printf("  p0 snapshot: %s\n", j.c_str());
        CHECK(j.find("\"target\":\"\"") != std::string::npos,
              "P0: with zero signals, target stays empty (safe)");
        CHECK(j.find("no-game-target") != std::string::npos,
              "P0: degraded=no-game-target (the bug we are fixing)");
    }

    // ------------------------------------------------------------------
    // PHASE 1: AUTO-DETECT the game (engine threads seen). fg=game ->
    // lastGame should now be primed even though NO manual/config entry exists.
    // ------------------------------------------------------------------
    printf("--- P1: engine-thread auto-detection primes lastGame ---\n");
    c4FreezerTestSetAutoGamePkg("com.auto.game");  // engine threads present
    g_fg = "com.auto.game";
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        printf("  p1 snapshot: %s\n", j.c_str());
        // Foreground game: target == game (it will freeze when it backgrounds).
        CHECK(j.find("\"target\":\"com.auto.game\"") != std::string::npos,
              "P1: auto-detected fg game becomes the target");
        CHECK(j.find("no-game-target") == std::string::npos,
              "P1: degraded is NOT no-game-target anymore");
        CHECK(j.find("\"frozen\":false") != std::string::npos,
              "P1: foreground game is NOT frozen");
    }

    // ------------------------------------------------------------------
    // PHASE 2: game backgrounds (launcher comes to foreground). The game
    // must now become a freezable background target and actually get FROZEN.
    // ------------------------------------------------------------------
    printf("--- P2: game backgrounds -> v1 FROZEN ---\n");
    c4FreezerTestSetAutoGamePkg("");   // launcher has no engine threads
    g_fg = "com.android.launcher";
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        printf("  p2 snapshot: %s\n", j.c_str());
        CHECK(j.find("\"target\":\"com.auto.game\"") != std::string::npos,
              "P2: backgrounded auto-game stays the freeze target");
        CHECK(j.find("\"frozen\":true") != std::string::npos,
              "P2: backgrounded game is now frozen");
        CHECK(j.find("\"freezerBackend\":\"v1\"") != std::string::npos,
              "P2: v1 backend used");
        CHECK(j.find("no-game-target") == std::string::npos,
              "P2: no more no-game-target degraded");
    }
    std::string st = readFileStd(root + "/freezer/uid_10123/freezer.state");
    printf("  freezer.state: '%s'\n", st.c_str());
    CHECK(st.find("FROZEN") != std::string::npos,
          "P2: on-disk freezer.state is FROZEN");

    // ------------------------------------------------------------------
    // PHASE 3: game returns to foreground -> thawed (safety guard).
    // ------------------------------------------------------------------
    printf("--- P3: game returns to foreground -> THAWED ---\n");
    c4FreezerTestSetAutoGamePkg("com.auto.game");
    g_fg = "com.auto.game";
    c4FreezerTick();
    st = readFileStd(root + "/freezer/uid_10123/freezer.state");
    printf("  freezer.state: '%s'\n", st.c_str());
    CHECK(st.find("THAWED") != std::string::npos,
          "P3: foreground return thaws the game");
    {
        std::string j = c4FreezerStateJson();
        CHECK(j.find("\"frozen\":false") != std::string::npos,
              "P3: JSON reports frozen:false after fg return");
    }

    printf("\n=== autogame result: pass=%d fail=%d ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
