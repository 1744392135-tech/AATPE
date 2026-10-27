// test_c4_freezer_mounts.cpp — #96 regression.
//
// On the audit device the v1 freezer hierarchy IS mounted
// (/proc/mounts: "none /sys/fs/cgroup/freezer cgroup rw,relatime,freezer",
// cgroup_mounts.txt PASS) yet c4.json reported freezerAvailable=false,
// freezerBackend="". The root cause: the old v1FreezerPresent() REQUIRED a
// readable <root>/freezer/freezer.state node; on this device that root node was
// not observable through the probe, so a perfectly healthy v1 backend was judged
// missing.
//
// This test reproduces the device shape: a v1 freezer mount point exists on disk
// (created as a directory) but has NO root freezer.state file. We inject the
// /proc/mounts content via the test seam c4FreezerTestSetMountsContent() and
// prove c4FreezerStateJson() reports the v1 backend as AVAILABLE
// (freezerAvailable=true, freezerBackend="v1") even with no game target.
//
// Compiled in production mode (-UAATPE_HOST_TEST -DC4_FREEZER_TEST_SEAM).

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

const char* STATE_DIR = "/tmp/aatpe_c4mounts_state";

namespace aatpe { namespace c4freezer {
extern std::atomic<int> g_c4freezerLockDepth;
void c4FreezerTestSetMountRoot(const std::string& root);
void c4FreezerTestSetMountsContent(const std::string& content);
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
    (void)cmd;
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

static void rmrf(const std::string& path) { std::string cmd = "rm -rf " + path; system(cmd.c_str()); }

int main() {
    printf("=== C4 #96: v1 freezer mounted, NO root freezer.state -> backend available ===\n");

    // Fake v1 freezer mount point. Deliberately NO freezer.state at the root:
    // this is exactly what the audit device exposes to the old probe.
    std::string root = "/tmp/aatpe_c4mounts_fakecg";
    rmrf(root);
    mkdir(root.c_str(), 0755);
    mkdir((root + "/freezer").c_str(), 0755);
    // NOTE: no <root>/freezer/freezer.state here. The old v1FreezerPresent() would
    // return false. The new /proc/mounts probe must still detect the backend.

    c4FreezerTestSetMountRoot(root);
    // Inject /proc/mounts content that names this exact freezer mount.
    std::string mounts =
        "none /dev/cg2_bpf cgroup2 rw,seclabel,nosuid,nodev,noexec,relatime 0 0\n"
        "none " + root + " tmpfs rw,seclabel,relatime,mode=750,gid=1000 0 0\n"
        "none " + root + "/freezer cgroup rw,relatime,freezer 0 0\n";
    c4FreezerTestSetMountsContent(mounts);

    rmrf(STATE_DIR);
    mkdir(STATE_DIR, 0700);
    setC4FreezerEnabled(true);
    CHECK(c4FreezerEnabled(), "freezer enabled");

    // Foreground = WebUI itself (non_game). No game target.
    g_fg = "com.sukisu.ultra";
    g_fgIsGame = false;
    c4FreezerTick();

    std::string j = c4FreezerStateJson();
    printf("  snapshot: %s\n", j.c_str());

    // The v1 backend must be reported AVAILABLE even though the root freezer.state
    // node is absent and there is no freezable target.
    CHECK(j.find("\"freezerAvailable\":true") != std::string::npos,
          "#96: freezerAvailable=true from /proc/mounts probe (no root freezer.state needed)");
    CHECK(j.find("\"freezerBackend\":\"v1\"") != std::string::npos,
          "#96: freezerBackend=v1");
    CHECK(j.find("no-freezer-controller") == std::string::npos,
          "#96: degraded is NOT 'no-freezer-controller' (backend really is present)");
    CHECK(j.find("\"target\":\"\"") != std::string::npos,
          "#96: target empty (nothing to freeze right now)");

    printf("\n=== mounts c4 result: pass=%d fail=%d ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
