
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
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

const char* STATE_DIR = "/tmp/aatpe_c4bucket_state";

namespace aatpe { namespace c4freezer {
extern std::atomic<int> g_c4freezerLockDepth;
void c4FreezerTestSetMountRoot(const std::string& root);
void c4FreezerTestSetPidsForUid(const std::vector<int>& pids);
void c4FreezerTestSetProcCgroupContent(const std::string& cg);
void c4FreezerTestSetReadbackFailAfter(int n);
void c4FreezerTestFailThawedWrites(bool f);
void c4FreezerTestFlushCaches();
}}

static std::string g_fg = "com.android.launcher";
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
bool isPackageInGameList(const std::string&) { return false; }

std::string aatpeExec(const std::string& cmd, const char*) {
    if (cmd.find("pm list packages -U com.game") != std::string::npos) {
        return "package:com.game uid:10123\n";
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
static void mkdirp(const std::string& p) { ::mkdir(p.c_str(), 0755); }

static void buildBucketTree(const std::string& root) {
    rmrf(root);
    mkdirp(root);
    mkdirp(root + "/freezer");
    mkdirp(root + "/freezer/perf");
    mkdirp(root + "/freezer/perf/frozen");
    mkdirp(root + "/freezer/perf/thawed");
    writeFileStd(root + "/freezer/perf/frozen/freezer.state", "FROZEN\n");
    writeFileStd(root + "/freezer/perf/frozen/cgroup.procs", "");
    writeFileStd(root + "/freezer/perf/thawed/freezer.state", "THAWED\n");
    writeFileStd(root + "/freezer/perf/thawed/cgroup.procs", "");
}

static void buildUidAndBucketTree(const std::string& root) {
    buildBucketTree(root);
    mkdirp(root + "/freezer/uid_10123");
    writeFileStd(root + "/freezer/uid_10123/freezer.state", "THAWED\n");
}

static void buildBareFreezerTree(const std::string& root) {
    rmrf(root);
    mkdirp(root);
    mkdirp(root + "/freezer");
    writeFileStd(root + "/freezer/freezer.state", "THAWED\n");
}

int main() {
    printf("=== C4 freezer dual-bucket: fake MIUI-style tree ===\n");
    std::string root = "/tmp/aatpe_c4bucket_fakecg";

    rmrf(STATE_DIR);
    mkdir(STATE_DIR, 0700);
    setC4FreezerEnabled(true);
    CHECK(c4FreezerEnabled(), "freezer enabled");

    printf("--- T-BK-A: bucket-only tree, game backgrounds -> v1-bucket freeze ---\n");
    buildBucketTree(root);
    c4FreezerTestSetMountRoot(root);
    c4FreezerTestFlushCaches();
    c4FreezerTestSetPidsForUid({4001, 4002});
    c4FreezerTestSetProcCgroupContent("5:freezer:/perf/frozen\n2:cpu:/perf\n");

    g_fg = "com.game"; g_fgIsGame = true;
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        CHECK(j.find("\"frozen\":false") != std::string::npos,
              "T-BK-A: game foreground -> not frozen");
    }

    g_fg = "com.android.launcher"; g_fgIsGame = false;
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        printf("  snapshot: %s\n", j.c_str());
        CHECK(j.find("\"frozen\":true") != std::string::npos,
              "T-BK-A: JSON frozen:true after background");
        CHECK(j.find("\"freezerBackend\":\"v1-bucket\"") != std::string::npos,
              "T-BK-A: JSON reports freezerBackend=v1-bucket");
        CHECK(j.find("/freezer/perf/frozen") != std::string::npos,
              "T-BK-A: cgDir is the frozen bucket path");
    }
    std::string fp = readFileStd(root + "/freezer/perf/frozen/cgroup.procs");
    CHECK(!fp.empty(), "T-BK-A: frozen/cgroup.procs written (pid move)");
    std::string rec = readFileStd(std::string(STATE_DIR) + "/c4_frozen_state");
    printf("  persisted: %s", rec.c_str());
    CHECK(rec.find("pids=4001,4002") != std::string::npos,
          "T-BK-A: state file persists pids=4001,4002");
    CHECK(rec.find("/perf/frozen") != std::string::npos,
          "T-BK-A: state file records frozen bucket cgDir");
    {
        std::string la = readFileStd(std::string(STATE_DIR) + "/c4_last_action");
        printf("  last_action: %s", la.c_str());
        CHECK(la.find("v1-bucket") != std::string::npos,
              "M36: c4_last_action records FREEZE(v1-bucket)");
    }

    printf("--- T-BK-B: game foreground -> v1-bucket thaw (move back to thawed) ---\n");
    c4FreezerTestSetProcCgroupContent("5:freezer:/perf/thawed\n");
    g_fg = "com.game"; g_fgIsGame = true;
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        CHECK(j.find("\"frozen\":false") != std::string::npos,
              "T-BK-B: game foreground -> frozen:false");
    }
    std::string tp = readFileStd(root + "/freezer/perf/thawed/cgroup.procs");
    CHECK(!tp.empty(), "T-BK-B: thawed/cgroup.procs written (pid move back)");

    printf("--- M33: per-pid read-back fails -> rollback + write-fail degrade ---\n");
    buildBucketTree(root);
    c4FreezerTestFlushCaches();
    c4FreezerTestSetPidsForUid({4001, 4002});
    c4FreezerTestSetProcCgroupContent("5:freezer:/perf/thawed\n");
    g_fg = "com.game"; g_fgIsGame = true;
    c4FreezerTick();
    g_fg = "com.android.launcher"; g_fgIsGame = false;
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        printf("  snapshot: %s\n", j.c_str());
        CHECK(j.find("\"frozen\":false") != std::string::npos,
              "M33: read-back failure -> not frozen (rolled back, no orphan)");
        std::string la = readFileStd(std::string(STATE_DIR) + "/c4_last_action");
        CHECK(la.find("write-fail-degraded") != std::string::npos,
              "M33: c4_last_action records write-fail-degraded");
    }
    std::string rb = readFileStd(root + "/freezer/perf/thawed/cgroup.procs");
    CHECK(!rb.empty(), "N6: rollback wrote thawed/cgroup.procs (pid rollback observable)");

    printf("--- T-BK-C: uid_10123 + buckets both present -> v1 (uid) wins ---\n");
    buildUidAndBucketTree(root);
    c4FreezerTestFlushCaches();
    g_fg = "com.game"; g_fgIsGame = true;
    c4FreezerTick();
    g_fg = "com.android.launcher"; g_fgIsGame = false;
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        printf("  snapshot: %s\n", j.c_str());
        CHECK(j.find("\"frozen\":true") != std::string::npos,
              "T-BK-C: game backgrounded -> frozen:true");
        CHECK(j.find("\"freezerBackend\":\"v1\"") != std::string::npos,
              "T-BK-C: legacy uid_<uid> path wins (backend=v1, NOT v1-bucket)");
        CHECK(j.find("/freezer/uid_10123") != std::string::npos,
              "T-BK-C: cgDir is the per-uid v1 dir");
    }
    g_fg = "com.game"; g_fgIsGame = true;
    c4FreezerTick();

    printf("--- T-BK-D: bare freezer root (no buckets, no uid) -> degrade ---\n");
    buildBareFreezerTree(root);
    c4FreezerTestFlushCaches();
    g_fg = "com.game"; g_fgIsGame = true;
    c4FreezerTick();
    g_fg = "com.android.launcher"; g_fgIsGame = false;
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        printf("  snapshot: %s\n", j.c_str());
        CHECK(j.find("\"frozen\":false") != std::string::npos,
              "T-BK-D: no usable cgroup -> not frozen (safe)");
        CHECK(j.find("\"cgDir\":\"\"") != std::string::npos,
              "T-BK-D: cgDir empty (no path guessed)");
    }

    printf("--- N-CHILD: child forked during freeze is re-enumerated and thawed ---\n");
    buildBucketTree(root);
    c4FreezerTestFlushCaches();
    c4FreezerTestSetReadbackFailAfter(0);
    c4FreezerTestSetPidsForUid({4001, 4002});
    c4FreezerTestSetProcCgroupContent("5:freezer:/perf/frozen\n");
    g_fg = "com.game"; g_fgIsGame = true;
    c4FreezerTick();
    g_fg = "com.android.launcher"; g_fgIsGame = false;
    c4FreezerTick();
    {
        std::string j = c4FreezerStateJson();
        CHECK(j.find("\"frozen\":true") != std::string::npos,
              "N-CHILD: game backgrounded -> frozen");
    }
    c4FreezerTestSetPidsForUid({4001, 4002, 4003});
    c4FreezerTestSetProcCgroupContent("5:freezer:/perf/thawed\n");
    g_fg = "com.game"; g_fgIsGame = true;
    c4FreezerTick();
    {
        std::string tp = readFileStd(root + "/freezer/perf/thawed/cgroup.procs");
        CHECK(tp.find("4003") != std::string::npos,
              "N-CHILD: derived child 4003 re-enumerated and moved to thawed");
    }

    printf("--- N-PROV: missing buckets -> probe provisions them and freeze works ---\n");
    {
        rmrf(root);
        mkdirp(root);
        mkdirp(root + "/freezer");
        mkdirp(root + "/freezer/perf");
        c4FreezerTestFlushCaches();
        c4FreezerTestSetPidsForUid({4001, 4002});
        c4FreezerTestSetProcCgroupContent("5:freezer:/perf/frozen\n");
        g_fg = "com.game"; g_fgIsGame = true;
        c4FreezerTick();
        g_fg = "com.android.launcher"; g_fgIsGame = false;
        c4FreezerTick();
        std::string j = c4FreezerStateJson();
        printf("  snapshot: %s\n", j.c_str());
        CHECK(j.find("\"frozen\":true") != std::string::npos,
              "N-PROV: provisioned buckets -> frozen:true");
        CHECK(j.find("\"freezerBackend\":\"v1-bucket\"") != std::string::npos,
              "N-PROV: backend v1-bucket after provisioning");
        struct stat st;
        CHECK(::stat((root + "/freezer/perf/frozen/freezer.state").c_str(), &st) == 0,
              "N-PROV: frozen/freezer.state created by provision");
        g_fg = "com.game"; g_fgIsGame = true;
        c4FreezerTick();
    }

    printf("--- N-PROV2: bucket dirs exist but state/procs missing -> EEXIST backfill ---\n");
    {
        rmrf(root);
        mkdirp(root);
        mkdirp(root + "/freezer");
        mkdirp(root + "/freezer/perf");
        mkdirp(root + "/freezer/perf/frozen");
        mkdirp(root + "/freezer/perf/thawed");
        c4FreezerTestFlushCaches();
        c4FreezerTestSetPidsForUid({4001, 4002});
        c4FreezerTestSetProcCgroupContent("5:freezer:/perf/frozen\n");
        g_fg = "com.game"; g_fgIsGame = true;
        c4FreezerTick();
        g_fg = "com.android.launcher"; g_fgIsGame = false;
        c4FreezerTick();
        std::string j = c4FreezerStateJson();
        printf("  snapshot: %s\n", j.c_str());
        CHECK(j.find("\"frozen\":true") != std::string::npos,
              "N-PROV2: existing dirs, missing state -> backfilled and frozen");
        struct stat st;
        CHECK(::stat((root + "/freezer/perf/frozen/freezer.state").c_str(), &st) == 0,
              "N-PROV2: freezer.state backfilled on EEXIST dir");
        g_fg = "com.game"; g_fgIsGame = true;
        c4FreezerTick();
    }

    printf("--- N-SYMLINK: bucket path is a symlink -> provision refuses, safe degrade ---\n");
    {
        buildBucketTree(root);
        std::string fz = root + "/freezer/perf/frozen";
        rmrf(fz);
        symlink("/etc", fz.c_str());
        c4FreezerTestFlushCaches();
        c4FreezerTestSetPidsForUid({4001, 4002});
        c4FreezerTestSetProcCgroupContent("5:freezer:/perf/frozen\n");
        g_fg = "com.game"; g_fgIsGame = true;
        c4FreezerTick();
        g_fg = "com.android.launcher"; g_fgIsGame = false;
        c4FreezerTick();
        std::string j = c4FreezerStateJson();
        printf("  snapshot: %s\n", j.c_str());
        CHECK(j.find("\"frozen\":false") != std::string::npos,
              "N-SYMLINK: symlinked bucket not used -> not frozen");
        struct stat st;
        CHECK(::lstat((fz + "/freezer.state").c_str(), &st) != 0,
              "N-SYMLINK: no freezer.state created outside expected tree");
        g_fg = "com.game"; g_fgIsGame = true;
        c4FreezerTick();
    }

    printf("--- N-COMMIT: rollback write fails -> verified subset committed, later thawed ---\n");
    {
        buildBucketTree(root);
        c4FreezerTestFlushCaches();
        c4FreezerTestSetPidsForUid({4001, 4002});
        c4FreezerTestSetProcCgroupContent("5:freezer:/perf/frozen\n");
        c4FreezerTestSetReadbackFailAfter(1);
        c4FreezerTestFailThawedWrites(true);
        g_fg = "com.game"; g_fgIsGame = true;
        c4FreezerTick();
        g_fg = "com.android.launcher"; g_fgIsGame = false;
        c4FreezerTick();
        std::string j = c4FreezerStateJson();
        printf("  snapshot: %s\n", j.c_str());
        CHECK(j.find("\"frozen\":true") != std::string::npos,
              "N-COMMIT: verified subset committed frozen (no stateless orphan)");
        std::string rec = readFileStd(std::string(STATE_DIR) + "/c4_frozen_state");
        CHECK(rec.find("pids=4001") != std::string::npos,
              "N-COMMIT: persisted stuck pid 4001");
        c4FreezerTestFailThawedWrites(false);
        c4FreezerTestSetReadbackFailAfter(0);
        c4FreezerTestSetProcCgroupContent("5:freezer:/perf/thawed\n");
        g_fg = "com.game"; g_fgIsGame = true;
        c4FreezerTick();
        {
            std::string tj = c4FreezerStateJson();
            CHECK(tj.find("\"frozen\":false") != std::string::npos,
                  "N-COMMIT: committed subset later thawed on foreground release");
            std::string tp = readFileStd(root + "/freezer/perf/thawed/cgroup.procs");
            CHECK(tp.find("4002") != std::string::npos,
                  "N-COMMIT: committed pid moved to thawed on release");
        }
    }

    printf("\n=== bucket glue result: pass=%d fail=%d ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
