
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "c4_freezer.h"
#include "aatpe_managers.h"

using namespace aatpe;
using namespace aatpe::c4freezer;

const char* STATE_DIR = "/tmp/aatpe_c4freezer_glue_state";

namespace aatpe { namespace c4freezer {
extern std::atomic<int> g_c4freezerLockDepth;
}}

static std::string g_fg = "com.android.launcher";
static bool        g_fgIsGame = false;

struct IOEntry { std::string path; std::string content; };
static std::vector<IOEntry> g_writes;
static std::vector<std::string> g_reads;
static int g_slowCallsWhileLocked = 0;

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

std::string aatpeExec(const std::string&, const char*) {
    if (g_c4freezerLockDepth.load(std::memory_order_relaxed) > 0)
        g_slowCallsWhileLocked++;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    return "Session #1:\n  package=com.music.app\n  playback state=3\n";
}

std::string readFile(const char* path) {
    if (g_c4freezerLockDepth.load(std::memory_order_relaxed) > 0)
        g_slowCallsWhileLocked++;
    g_reads.push_back(path ? path : "");
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

bool writeFileAtomic(const char* path, const std::string& content) {
    if (g_c4freezerLockDepth.load(std::memory_order_relaxed) > 0)
        g_slowCallsWhileLocked++;
    g_writes.push_back({path ? path : "", content});
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

static bool containsPath(const std::string& suffix) {
    for (const auto& w : g_writes)
        if (w.path.size() >= suffix.size() &&
            w.path.compare(w.path.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
    return false;
}
static bool barePathUsed(const char* name) {
    for (const auto& r : g_reads) if (r == name) return true;
    for (const auto& w : g_writes) if (w.path == name) return true;
    return false;
}
static bool anyWriteHasContent(const std::string& suffix, const std::string& content) {
    for (const auto& w : g_writes) {
        if (w.path.size() >= suffix.size() &&
            w.path.compare(w.path.size() - suffix.size(), suffix.size(), suffix) == 0 &&
            w.content == content) return true;
    }
    return false;
}

int main() {
    printf("=== C4 freezer glue (production mode): path/lock/NN-restart ===\n");

    mkdir(STATE_DIR, 0700);
    std::string cwdRoot = "/tmp/aatpe_c4freezer_glue_cwd";
    mkdir(cwdRoot.c_str(), 0700);
    chdir(cwdRoot.c_str());
    setC4FreezerEnabled(true);
    CHECK(c4FreezerEnabled(), "freezer enabled");

    const char* seededRecord =
        "cg=/sys/fs/cgroup/uid_10123/cg_42\npkg=com.game\nuid=10123\nlg=com.game\n";
    {
        int fd = open(std::string(STATE_DIR).append("/c4_frozen_state").c_str(),
                      O_WRONLY | O_CREAT | O_TRUNC, 0600);
        write(fd, seededRecord, strlen(seededRecord));
        close(fd);
    }

    g_fg = "com.android.launcher";
    g_fgIsGame = false;

    g_reads.clear(); g_writes.clear();
    c4FreezerTick();

    printf("--- T-A: state files live under STATE_DIR, not CWD ---\n");
    CHECK(!g_reads.empty(), "first tick performed a state-file read");
    bool readUnderStateDir = !g_reads.empty() &&
        g_reads[0].rfind(std::string(STATE_DIR) + "/c4_frozen_state", 0) == 0;
    CHECK(readUnderStateDir,
          "T-A: readFile called as STATE_DIR/c4_frozen_state (not bare basename)");
    CHECK(!barePathUsed("c4_frozen_state"),
          "T-A: no bare-relative \"c4_frozen_state\" read/write");
    CHECK(containsPath(std::string(STATE_DIR) + "/c4_last_action"),
          "T-A: c4_last_action written under STATE_DIR");
    CHECK(!barePathUsed("c4_last_action"),
          "T-A: no bare-relative \"c4_last_action\"");
    CHECK(access("c4_frozen_state", F_OK) != 0,
          "T-A: no c4_frozen_state created in bare CWD");
    CHECK(access("c4_last_action", F_OK) != 0,
          "T-A: no c4_last_action created in bare CWD");
    CHECK(access(std::string(STATE_DIR).append("/c4_last_action").c_str(), F_OK) == 0,
          "T-A: STATE_DIR/c4_last_action exists on disk");

    printf("--- T-D: NN restart recovery restores frozen bookkeeping ---\n");
    std::string j = c4FreezerStateJson();
    printf("  snapshot: %s\n", j.c_str());
    CHECK(j.find("\"frozen\":true") != std::string::npos,
          "T-D: restored record -> JSON frozen:true");
    CHECK(j.find("/sys/fs/cgroup/uid_10123/cg_42") != std::string::npos,
          "T-D: restored cgDir visible in JSON");

    printf("--- T-B: no slow I/O inside the locked section ---\n");
    CHECK(g_slowCallsWhileLocked == 0,
          "T-B: exec/read/write all observed with lock depth == 0");

    printf("--- T-C: c4FreezerStateJson latency while tick thread runs 30ms execs ---\n");
    {
        std::atomic<bool> stop{false};
        std::thread ticker([&]() {
            while (!stop.load(std::memory_order_relaxed)) c4FreezerTick();
        });
        std::string sink;
        double maxMs = 0.0;
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t0).count() < 0.6) {
            auto a = std::chrono::steady_clock::now();
            sink = c4FreezerStateJson();
            auto b = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(b - a).count();
            if (ms > maxMs) maxMs = ms;
        }
        stop.store(true, std::memory_order_relaxed);
        ticker.join();
        (void)sink;
        printf("  max c4FreezerStateJson() latency: %.2f ms (exec stub sleeps 30 ms)\n", maxMs);
        CHECK(maxMs < 20.0,
              "T-C: snapshot call never blocks on the tick's slow exec (<20ms)");
    }
    CHECK(g_slowCallsWhileLocked == 0,
          "T-B: after contention run, still zero I/O calls under the lock");

    printf("--- T-D: frozen game returns to foreground -> unfreeze + clear record ---\n");
    g_fg = "com.game";
    g_fgIsGame = true;
    c4FreezerTick();

    std::string j2 = c4FreezerStateJson();
    printf("  snapshot: %s\n", j2.c_str());
    CHECK(j2.find("\"frozen\":false") != std::string::npos,
          "T-D: game foregrounded -> JSON frozen:false");
    CHECK(anyWriteHasContent("/c4_frozen_state", ""),
          "T-D: persisted record cleared (empty write) on release");
    {
        std::string lastAction;
        for (auto it = g_writes.rbegin(); it != g_writes.rend(); ++it) {
            if (it->path.rfind(std::string(STATE_DIR) + "/c4_last_action") !=
                std::string::npos) { lastAction = it->content; break; }
        }
        CHECK(lastAction.find("UNFREEZE") != std::string::npos,
              "T-D: c4_last_action records the UNFREEZE decision");
    }

    printf("\n=== glue result: pass=%d fail=%d ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
