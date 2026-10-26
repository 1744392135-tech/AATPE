// test_c4_freezer_glue.cpp — v6.0.0 review regression tests for the C4 freezer
// PRODUCTION glue (c4FreezerTick / c4FreezerStateJson), compiled in production
// mode (WITHOUT -DAATPE_HOST_TEST, with -DC4_FREEZER_TEST_SEAM) so the real tick
// body, the mutex and the statics are linked. All external deps of the glue are
// stubbed here (test-only symbols; release nm never sees them).
//
// Covers the four review findings:
//   T-A: the persisted state file and c4_last_action land under STATE_DIR, NOT
//        relative to the daemon CWD (P1: bare-relative kStateFile).
//   T-B: slow I/O (aatpeExec / readFile / writeFileAtomic) NEVER runs while
//        s_stateMutex is held (P1: lock granularity). Probed directly via the
//        C4_FREEZER_TEST_SEAM lock-depth counter.
//   T-C: behavioral proof of T-B — the WebUI snapshot call must not block on a
//        deliberately-slow (30 ms) exec call made concurrently by the tick
//        thread. Under the old code the mutex was held across exec and every
//        colliding c4FreezerStateJson() call stalled ~30 ms.
//   T-D: NN restart recovery — a seeded STATE_DIR/c4_frozen_state is read on the
//        first tick, restored bookkeeping is visible through c4FreezerStateJson(),
//        and when the frozen game returns to foreground the tick releases it and
//        clears the persisted record.

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

// ---- test-only state ----------------------------------------------------------
const char* STATE_DIR = "/tmp/aatpe_c4freezer_glue_state";

// The seam exported by c4_freezer.cpp under C4_FREEZER_TEST_SEAM: how many
// critical sections the tick/reader are inside right now.
namespace aatpe { namespace c4freezer {
extern std::atomic<int> g_c4freezerLockDepth;
}}

static std::string g_fg = "com.android.launcher";
static bool        g_fgIsGame = false;

struct IOEntry { std::string path; std::string content; };
static std::vector<IOEntry> g_writes;          // every writeFileAtomic call
static std::vector<std::string> g_reads;       // every readFile call
static int g_slowCallsWhileLocked = 0;          // I/O observed with lock held

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

// Slow exec stub: 30 ms. Returns content that (a) contains no "uid:" so
// resolveAppUid() yields 0, and (b) mentions a DIFFERENT package's playback
// state=3 so mediaSessionMentionsPlaying() -> false (audio NOT active for our
// target). The delay is what makes T-C meaningful.
std::string aatpeExec(const std::string&, const char*) {
    if (g_c4freezerLockDepth.load(std::memory_order_relaxed) > 0)
        g_slowCallsWhileLocked++;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    return "Session #1:\n  package=com.music.app\n  playback state=3\n";
}

// Real file reads, but recorded. The path must already be absolute (STATE_DIR/...).
std::string readFile(const char* path) {
    if (g_c4freezerLockDepth.load(std::memory_order_relaxed) > 0)
        g_slowCallsWhileLocked++;
    g_reads.push_back(path ? path : "");
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// Real atomic-ish write, but recorded.
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

// ---- tiny CHECK harness ---------------------------------------------------------
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

    // ---- fixture: fresh STATE_DIR + an empty CWD we chdir away from -------------
    mkdir(STATE_DIR, 0700);
    std::string cwdRoot = "/tmp/aatpe_c4freezer_glue_cwd";
    mkdir(cwdRoot.c_str(), 0700);
    chdir(cwdRoot.c_str());
    // wipe previous contents (seed below uses O_TRUNC; this is just hygiene)
    setC4FreezerEnabled(true);
    CHECK(c4FreezerEnabled(), "freezer enabled");

    // ---- T-D setup + T-A: seed a persisted frozen record BEFORE the first tick -
    const char* seededRecord =
        "cg=/sys/fs/cgroup/uid_10123/cg_42\npkg=com.game\nuid=10123\nlg=com.game\n";
    {
        int fd = open(std::string(STATE_DIR).append("/c4_frozen_state").c_str(),
                      O_WRONLY | O_CREAT | O_TRUNC, 0600);
        write(fd, seededRecord, strlen(seededRecord));
        close(fd);
    }

    // Foreground = launcher (non-game): restored game stays "frozen".
    g_fg = "com.android.launcher";
    g_fgIsGame = false;

    g_reads.clear(); g_writes.clear();
    c4FreezerTick();   // first tick: NN restore loads the seeded record

    // ---- T-A assertions ---------------------------------------------------------
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
    // behavioral: CWD (the empty chdir'd dir) must contain nothing
    CHECK(access("c4_frozen_state", F_OK) != 0,
          "T-A: no c4_frozen_state created in bare CWD");
    CHECK(access("c4_last_action", F_OK) != 0,
          "T-A: no c4_last_action created in bare CWD");
    CHECK(access(std::string(STATE_DIR).append("/c4_last_action").c_str(), F_OK) == 0,
          "T-A: STATE_DIR/c4_last_action exists on disk");

    // ---- T-D part 1: restored bookkeeping visible through the snapshot -----------
    printf("--- T-D: NN restart recovery restores frozen bookkeeping ---\n");
    std::string j = c4FreezerStateJson();
    printf("  snapshot: %s\n", j.c_str());
    CHECK(j.find("\"frozen\":true") != std::string::npos,
          "T-D: restored record -> JSON frozen:true");
    CHECK(j.find("/sys/fs/cgroup/uid_10123/cg_42") != std::string::npos,
          "T-D: restored cgDir visible in JSON");

    // ---- T-B: slow I/O never observed with the mutex held ------------------------
    printf("--- T-B: no slow I/O inside the locked section ---\n");
    CHECK(g_slowCallsWhileLocked == 0,
          "T-B: exec/read/write all observed with lock depth == 0");

    // ---- T-C: the reader must not block on the tick's slow exec -------------------
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
        // Old code: mutex held across the 30 ms exec -> json collides and stalls
        // ~30 ms. New code: critical sections are microsecond copies -> well under.
        CHECK(maxMs < 20.0,
              "T-C: snapshot call never blocks on the tick's slow exec (<20ms)");
    }
    CHECK(g_slowCallsWhileLocked == 0,
          "T-B: after contention run, still zero I/O calls under the lock");

    // ---- T-D part 2: frozen game returns to foreground -> release + clear --------
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
    // c4_last_action must now record the unfreeze decision
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
