// AATPE Game Cold-Start Preset Rules implementation (P1-7 / P1-13/P1-14)
#include "cold_start_rules.h"
#include "aatped.h"
#include "aatped_utils.h"       // g_threads / g_threadsMutex
#include "aatped_priority.h"    // aatpeSetThreadPriority
#include "aatped_config.h"      // isPackageInGameList
#include "aatped_logging.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#include <mutex>

namespace aatpe {

// ROUND-014 P3 (S14-06/P2): adaptive window. Hold at least kColdStartMinMs; release
// early once frames converge (kConvergeStreak consecutive tick windows with no jank);
// hard cap at kColdStartMaxMs.
static constexpr long long kColdStartMinMs = 15000;
static constexpr long long kColdStartMaxMs = 30000;
static constexpr int kConvergeStreak = 3;
// ROUND-014 P3: fast foreground-switch debounce. Away-and-back within this window is a
// warm/hot resume, not a cold start -> do not re-arm the full cold boost.
static constexpr long long kSwitchDebounceMs = 1000;
// F3.1 (v4.3.8): process-instance identity. A foreground edge is a TRUE cold start
// only when the game's process instance (PID + /proc/<pid>/stat start_time) differs
// from the last recorded instance for that package. A returning SAME live instance
// (process alive, pages/code already faulted in) gets only a short bounded warm boost
// instead of the full 15-30s RT window. The short warm window is intentionally small:
// it re-asserts RT priority for the first few frames after resume, then hands off.
static constexpr long long kWarmBoostMinMs = 0;      // converge as soon as frames settle
static constexpr long long kWarmBoostMaxMs = 5000;   // hard cap for a warm resume

namespace {
// P1-14: the active/coldPkg/coldStartMs triple is read by the main decision
// loop (coldStartTick) and mutated on the foreground-switch hook, and queried
// by diagnostics (coldStartActive/coldStartPkg). Previously these were plain
// statics read/written without a lock. Guard them with a dedicated mutex; do
// NOT hold it across aatpeSetThreadPriority() (syscalls) or g_threadsMutex.
std::mutex  s_coldMutex;
std::string s_coldPkg;
long long   s_coldStartMs = 0;
bool        s_active = false;
// ROUND-014 P3: thermal freeze (set by main loop from G6 thermal HAL). SEVERE+ -> skip
// all RT presets this tick; moderate -> handled by main. Convergence streak counter.
bool        s_thermalFreeze = false;
bool        s_ioPressure = false;
int         s_convergeStreak = 0;
// ROUND-014 P3: foreground switch debounce state (warm/hot resume suppression).
std::string s_lastDisarmPkg;
long long   s_lastDisarmMs = 0;
// F-8: per-package cold-start arm frequency cap. A real user opens a given game at most
// a few times within a minute; repeatedly switching foreground every >1s (to bypass the
// debounce) to farm RT boosts must be limited. Allow at most kArmsPerWindow arms per package
// within kArmWindowMs; then refuse until the window slides.
#include <unordered_map>
#include <deque>
static constexpr long long kArmWindowMs = 60000;
static constexpr int kArmsPerWindow = 3;
std::unordered_map<std::string, std::deque<long long>> s_armHistory;
// F-8 (v4.2 Batch C / spec07 §3.2): cumulative count of cold-start arm requests
// REFUSED by the per-package frequency cap above. The counter only ever grows
// (one-way farm-resistance evidence). It is mirrored to STATE_DIR/cold_start_rejected
// (uint text) at the mutation point so state.json/WebUI can display it. All access is
// under s_coldMutex; the daemon is the sole writer, readers serialize via
// readInt(path, 0) (missing file -> 0).
long long s_coldRejected = 0;

// F3.1 (v4.3.8): per-package last-recorded process instance. A return whose
// (pid,start_time) matches this entry is the SAME live process -> warm resume; a
// different entry (or first sighting) -> true cold start. start_time (field 22 of
// /proc/<pid>/stat, clock ticks since boot) never collides across PID reuse, so the
// pair uniquely identifies one process instance. Read/written only under
// s_coldMutex (the pid/start values themselves come from a lock-free /proc scan done
// on the arm edge).
struct ProcInstance { int pid = 0; unsigned long long start_time = 0; };
std::unordered_map<std::string, ProcInstance> s_lastInstance;
// True while the CURRENTLY armed window is a full cold boost; false => the short
// warm-resume boost. Set on arm, cleared on disarm/window-elapsed.
bool s_isCold = false;

#ifdef AATPE_HOST_TEST
static long long (*g_nowOverrideForTest)() = nullptr;
// F3.1 host override: when active, resolveProcIdentity() reads s_testIdentity
// instead of scanning /proc. No entry for a pkg => process not present there.
static bool g_testIdentityActive = false;
static std::unordered_map<std::string, ProcInstance> s_testIdentity;
#endif

long long nowMs() {
#ifdef AATPE_HOST_TEST
    if (g_nowOverrideForTest) return g_nowOverrideForTest();
#endif
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// F-11 host seams: drive the jank-vote and displayed-probe without frame.cpp / dumpsys.
#ifdef AATPE_HOST_TEST
static bool (*g_jankVoteForTest)() = nullptr;       // return true == jank active
static std::string (*g_displayedForTest)() = nullptr; // return stub dumpsys output
#endif
static bool coldStartJankActive() {
#ifdef AATPE_HOST_TEST
    if (g_jankVoteForTest) return g_jankVoteForTest();
#endif
    return aatpe::frameaware::jankBoostVoteActive();
}

// Match a thread's tname against the preset table. Returns the role+level to
// apply, or {-1,0} for "no preset for this thread".
struct Preset { ThreadRole role; int level; };
Preset presetForThread(const ThreadInfo& t) {
    std::string n = t.tname;
    // P3-18: cast to unsigned char before std::tolower. Passing a plain char
    // with a negative value (bytes >= 0x80) is undefined behavior.
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (n.find("renderthread") != std::string::npos) {
        return {ROLE_S2_RENDER, 3};
    }
    // G5-009: the old bare n.find("audio") matched ANY thread whose name merely
    // contained "audio" (e.g. a background "audioutils-worker"), needlessly
    // boosting it to RT. Restrict to the known real-time audio renderer threads.
    {
        static const char* kRtAudio[] = {
            "audiotrack", "audioflinger", "audiomixer", "audioout",
            "audioin", "aaudio", "audiohal",
        };
        for (const char* pat : kRtAudio) {
            if (n.find(pat) != std::string::npos) {
                return {ROLE_S1_UI, 2};
            }
        }
    }
    // Main / UI thread of the game process (tid == pid == process main thread).
    if (t.tid == t.pid) {
        return {ROLE_S3_GAME, 3};
    }
    return {ROLE_S0_SYSTEM, 0};
}

// F-11 (postfreeze): symmetric release. On window exit we must undo the RT/SCHED_FIFO
// boost the armed window applied, or level-3 priority lingers until the placement loop
// happens to revisit the tid. Mirrors the tick's Job snapshot: for the same pkg threads
// that a preset matched, call the UNGATED restore path.
//
// #1/#2 fix: previously this called aatpeSetThreadPriority(tid, role, 0), but the
// setter clamps level<1 to 1 and only ever RAISES to SCHED_FIFO/RR — so the thread
// stayed at SCHED_RR + uclamp_min=1024. Worse, the setter's first line checks
// rtBoostAllowed(); under safe mode the release was short-circuited. We now call
// aatpeRestoreThreadPriority() which reverts to SCHED_OTHER / recorded nice / recorded
// uclamp and is NOT gated by safe mode. Only live tids currently in g_threads are
// touched (dead/reused tids are skipped).
static void releaseColdBoost(const std::string& pkg) {
    struct Job { int tid; ThreadRole role; };
    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        for (const auto& kv : g_threads) {
            const ThreadInfo& t = kv.second;
            if (t.pname != pkg) continue;
            Preset p = presetForThread(t);
            if (p.level > 0) jobs.push_back({t.tid, p.role});
        }
    }
    for (const Job& j : jobs) {
        aatpe_priority::aatpeRestoreThreadPriority(j.tid);
    }
}
}  // namespace

// ROUND-014 P3: main loop gates. External linkage (called from aatped_main.cpp).
void coldStartSetThermalFreeze(bool f) {
    std::lock_guard<std::mutex> lk(s_coldMutex);
    s_thermalFreeze = f;
}
// ROUND-014 P3: main loop sets from PSI io. true = IO-bound launch -> downgrade presets.
void coldStartSetIoPressure(bool f) {
    std::lock_guard<std::mutex> lk(s_coldMutex);
    s_ioPressure = f;
}

#ifdef AATPE_HOST_TEST
// F-8 clock injection (host only). Defined here (external linkage) so the host test can
// drive the 60s arm-window rollover; production macro off -> no symbol, real clock used.
void coldStartSetNowForTest(long long (*fn)()) {
    g_nowOverrideForTest = fn;
}
// F-11 host seams. nullptr restores the real frame/dumpsys behaviour.
void coldStartSetJankForTest(bool (*fn)()) { g_jankVoteForTest = fn; }
void coldStartSetDisplayedForTest(std::string (*fn)()) { g_displayedForTest = fn; }

// F-8 (v4.2 Batch C): test seam to read the cumulative refusal counter. Locked
// read for consistency; production code reads the STATE_DIR mirror instead.
long long coldStartRejectedCountForTest() {
    std::lock_guard<std::mutex> lk(s_coldMutex);
    return s_coldRejected;
}

// F3.1 (v4.3.8): inject a fake live process identity for `pkg`. pid<=0 marks the
// process as gone. The first call flips resolveProcIdentity off /proc onto this map.
void coldStartSetProcIdentityForTest(const std::string& pkg, int pid,
                                     unsigned long long start_time) {
    g_testIdentityActive = true;
    if (pid <= 0) s_testIdentity.erase(pkg);
    else          s_testIdentity[pkg] = {pid, start_time};
}

// F3.1 (v4.3.8): observability — true while the armed window is a full cold boost.
bool coldStartIsColdForTest() {
    std::lock_guard<std::mutex> lk(s_coldMutex);
    return s_isCold;
}
#endif

// ROUND-014 P3 (SPEC 2.7): WebUI/doctor observability state key. stage in
// armed/displayed_wait/converged/skipped_thermal/skipped_io. Writes the current window
// phase + package + remaining ms. Empty stage clears the key (inactive/disarmed).
static void writeColdStartWindow(const char* stage, const std::string& pkg, long long remMs) {
    std::string v = std::string(stage) + " pkg=" + (pkg.empty() ? std::string("-") : pkg) +
                    " remain_ms=" + std::to_string(remMs);
    writeFile((std::string(STATE_DIR) + "/cold_start_window").c_str(), v);
}

// ROUND-014 P3: pure parse of `dumpsys activity activities` output. Returns true when a
// Displayed line (reportLaunchTimeLocked "Displayed <pkg>/.Activity: +...") or a non-zero
// mDisplayed timestamp is present -> the app has rendered its first frame, safe to hand
// off. Empty/unrelated output -> false (caller falls back to the fixed window).
bool coldStartDisplayedPresent(const std::string& out) {
    if (out.empty()) return false;
    if (out.find("Displayed ") != std::string::npos) return true;
    // mDisplayed=0x0 / "mDisplayed=0" means not yet reported.
    size_t pos = out.find("mDisplayed=");
    if (pos != std::string::npos) {
        pos += 11;
        // skip whitespace, then optional 0x/0X hex prefix, then zero digits.
        while (pos < out.size() && out[pos]==' ') pos++;
        if (pos+1 < out.size() && (out[pos]=='0') &&
            (out[pos+1]=='x' || out[pos+1]=='X')) pos += 2;
        while (pos < out.size() && out[pos]=='0') pos++;
        if (pos < out.size() && out[pos] >= '1' && out[pos] <= '9') return true;
        // also accept nonzero hex digits a-f/A-F after stripping prefix.
        if (pos < out.size() && ((out[pos]>='a'&&out[pos]<='f')||(out[pos]>='A'&&out[pos]<='F'))) return true;
    }
    return false;
}

// Best-effort root fetch of `dumpsys activity activities`.
// #8 fix: the old implementation was a bare fork + blocking read() + waitpid(...,0)
// with NO timeout/kill/reap — a hung system_server could freeze the main tick. We now
// delegate to the shared hardened aatpeExecArgv() (3000ms wall-clock deadline +
// setpgid + killpg + bounded WNOHANG reap + dumpsys cooldown). No shell: the argv is
// passed verbatim, and the Displayed/mDisplayed filtering is done in C++ by
// coldStartDisplayedPresent() (never via grep/head). Returns "" on any failure/timeout;
// the caller falls back to the fixed window.
static std::string coldStartFetchDisplayed() {
    return aatpeExecArgv({"/system/bin/dumpsys", "activity", "activities"},
                         "coldstart-displayed");
}

// F3.1 (v4.3.8): resolve the live process instance for package `pkg`. Returns true
// and fills (pid, start_time) when the process is alive. start_time is field 22 of
// /proc/<pid>/stat (clock ticks since boot). The (pid, start_time) pair uniquely
// identifies one process instance across PID reuse.
//
// Fully generic on every Android/Linux device: the main app process has cmdline
// equal to its package name; named subprocesses are "pkg:...". We match cmdline
// argv[0] against the package — there is NO package-name hardcoding here. Runs
// only on a foreground-switch edge (not per tick), so the one-shot /proc scan cost
// is negligible.
static bool resolveProcIdentity(const std::string& pkg, int& pidOut,
                                unsigned long long& startOut) {
    if (pkg.empty()) return false;
#ifdef AATPE_HOST_TEST
    if (g_testIdentityActive) {
        auto it = s_testIdentity.find(pkg);
        if (it == s_testIdentity.end() || it->second.pid <= 0) return false;
        pidOut = it->second.pid;
        startOut = it->second.start_time;
        return true;
    }
#endif
    // Production: scan /proc for the process whose cmdline is this package.
    DIR* d = opendir("/proc");
    if (!d) return false;
    struct dirent* de = nullptr;
    int exactPid = 0, prefixPid = 0;
    const std::string namedPrefix = pkg + ":";
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int pid = atoi(de->d_name);
        if (pid <= 0) continue;
        std::string cmdline =
            readFile(("/proc/" + std::to_string(pid) + "/cmdline").c_str());
        if (cmdline.empty()) continue;
        size_t nul = cmdline.find('\0');  // cmdline is NUL-separated argv
        if (nul != std::string::npos) cmdline.resize(nul);
        size_t a = cmdline.find_first_not_of(" \t\r\n");
        size_t b = cmdline.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        cmdline = cmdline.substr(a, b - a + 1);
        if (cmdline == pkg) { exactPid = pid; break; }       // main process wins
        if (prefixPid == 0 && cmdline.size() > namedPrefix.size() &&
            cmdline.compare(0, namedPrefix.size(), namedPrefix) == 0) {
            prefixPid = pid;
        }
    }
    closedir(d);
    int foundPid = (exactPid > 0) ? exactPid : prefixPid;
    if (foundPid <= 0) return false;

    // Read /proc/<pid>/stat field 22 (start_time). comm is parenthesized and may
    // contain spaces/parens -> anchor on the LAST ')' and tokenize after it.
    std::string stat =
        readFile(("/proc/" + std::to_string(foundPid) + "/stat").c_str());
    size_t rp = stat.rfind(')');
    if (rp == std::string::npos) return false;
    std::istringstream iss(stat.substr(rp + 1));
    std::string field; int fn = 0;
    unsigned long long start = 0; bool got = false;
    while (iss >> field) {
        if (fn == 19) {  // field 22 (1-indexed) = start_time; fn 0-indexed after ')'
            try { start = std::stoull(field); got = true; } catch (...) { got = false; }
            break;
        }
        fn++;
    }
    if (!got || start == 0) return false;
    pidOut = foundPid;
    startOut = start;
    return true;
}

void coldStartOnForegroundSwitch(const std::string& pkg) {
    if (isPackageInGameList(pkg)) {
        // F3.1: resolve the live process instance OUTSIDE s_coldMutex (the /proc
        // scan does blocking file IO; the lock must stay short). Values are then
        // snapshotted under the lock for the cold/warm decision.
        int curPid = 0;
        unsigned long long curStart = 0;
        bool haveId = resolveProcIdentity(pkg, curPid, curStart);

        std::lock_guard<std::mutex> lk(s_coldMutex);
        if (s_active && s_coldPkg == pkg) return;  // already armed
        // ROUND-014 P3: 1s switch debounce. A brief away-and-back (warm/hot resume)
        // within kSwitchDebounceMs must not re-arm a boost at all.
        long long now = nowMs();
        if (!s_active && pkg == s_lastDisarmPkg && (now - s_lastDisarmMs) < kSwitchDebounceMs) {
            s_coldStartMs = now;  // nudge timer, stay disarmed
            return;
        }
        // F3.1: classify cold vs warm by process-instance identity.
        //  - SAME live instance (pid+start_time both match the last recorded one)
        //    => the process is alive and already loaded -> warm/hot resume.
        //  - DIFFERENT instance, or first sighting => true cold start.
        //  - Identity unresolvable (/proc miss) => conservative cold (no regression:
        //    the daemon runs as root and /proc is universally readable; this is a
        //    safety net for exotic devices, not the production path).
        bool isCold = true;
        if (haveId) {
            auto it = s_lastInstance.find(pkg);
            if (it != s_lastInstance.end() &&
                it->second.pid == curPid && it->second.start_time == curStart) {
                isCold = false;
            }
        }
        // F-8: per-package arm frequency cap. Prune history older than the window; if this
        // package already armed kArmsPerWindow times within it, refuse (farm-resistant).
        // Fix-A: erase empty deque keys so the map doesn't accumulate N empty entries
        // after the user has opened N different apps over time.
        auto itHist = s_armHistory.find(pkg);
        if (itHist != s_armHistory.end()) {
            auto& hist = itHist->second;
            while (!hist.empty() && (now - hist.front()) > kArmWindowMs) hist.pop_front();
            if (hist.empty()) { s_armHistory.erase(itHist); itHist = s_armHistory.end(); }
        }
        // Rate-limit check: only if we still have entries after pruning. Applies to
        // both cold and warm arms — farming a short warm boost is still farming.
        int histSize = (itHist != s_armHistory.end()) ? (int)itHist->second.size() : 0;
        if (histSize >= kArmsPerWindow) {
            auto& hist = itHist->second;
            LOG_INFO("[P1-7] cold-start arm rate-limited for %s (%d/%d in %lldms)",
                     pkg.c_str(), (int)hist.size(), kArmsPerWindow, kArmWindowMs);
            s_coldRejected++;
            writeFile((std::string(STATE_DIR) + "/cold_start_rejected").c_str(),
                      (long long)s_coldRejected);
            return;
        }
        s_armHistory[pkg].push_back(now);
        if (haveId) s_lastInstance[pkg] = {curPid, curStart};
        s_coldPkg = pkg;
        s_coldStartMs = now;
        s_active = true;
        s_isCold = isCold;
        s_convergeStreak = 0;   // fresh window: convergence streak starts at zero
        const long long winMin = isCold ? kColdStartMinMs : kWarmBoostMinMs;
        const long long winMax = isCold ? kColdStartMaxMs : kWarmBoostMaxMs;
        writeColdStartWindow(isCold ? "armed" : "armed_warm", pkg, winMax);
        LOG_FORCE("[P1-7] game cold-start preset armed for %s (%s; window=%lld-%lldms)",
                  pkg.c_str(), isCold ? "cold" : "warm-resume",
                  (long long)winMin, (long long)winMax);
    } else {
        std::lock_guard<std::mutex> lk(s_coldMutex);
        if (s_active) {
            LOG_INFO("[P1-7] leaving game foreground (%s), disarming preset",
                     s_coldPkg.c_str());
            releaseColdBoost(s_coldPkg);   // F-11: symmetric RT release before clearing
            s_lastDisarmPkg = s_coldPkg;
            s_lastDisarmMs = nowMs();
            writeColdStartWindow("disarmed", s_coldPkg, 0);
            s_active = false;
            s_isCold = false;
            s_coldPkg.clear();
        }
    }
}

void coldStartTick() {
    // Snapshot state under the lock, then release BEFORE touching g_threadsMutex
    // or calling aatpeSetThreadPriority (syscalls).
    bool active;
    std::string pkg;
    long long startMs;
    bool freeze;
    bool ioPress;
    bool isCold;
    {
        std::lock_guard<std::mutex> lk(s_coldMutex);
        active = s_active;
        pkg = s_coldPkg;
        startMs = s_coldStartMs;
        freeze = s_thermalFreeze;
        ioPress = s_ioPressure;
        isCold = s_isCold;
    }
    if (!active) return;

    // F3.1: window bounds depend on whether this is a true cold start (15-30s) or
    // a warm resume (0-5s). The convergence/thermal/IO semantics are unchanged.
    const long long winMinMs = isCold ? kColdStartMinMs : kWarmBoostMinMs;
    const long long winMaxMs = isCold ? kColdStartMaxMs : kWarmBoostMaxMs;

    long long age = nowMs() - startMs;
    // ROUND-014 P3: convergence early-exit — after the min window, if frames have
    // been jank-free for kConvergeStreak consecutive ticks, hand off early.
    bool noJank = !coldStartJankActive();
    if (noJank) s_convergeStreak++; else s_convergeStreak = 0;

    // ROUND-014 P3: Displayed-based handoff. After the min window, probe dumpsys at most
    // once per ~2s (execvp is costly); a reported Displayed -> first frame done -> handoff.
    // F3.1: only probe for a TRUE cold start. A warm-resume process has already rendered
    // its UI, so forking dumpsys would buy nothing.
    bool displayed = false;
    static long long s_lastProbeMs = 0;
    if (isCold && age > winMinMs && (nowMs() - s_lastProbeMs) > 2000) {
        s_lastProbeMs = nowMs();
#ifdef AATPE_HOST_TEST
        if (g_displayedForTest) displayed = coldStartDisplayedPresent(g_displayedForTest());
        else
#endif
        displayed = coldStartDisplayedPresent(coldStartFetchDisplayed());
    }

    bool windowElapsed = (age > winMaxMs) || displayed ||
                          (age > winMinMs && s_convergeStreak >= kConvergeStreak);
    if (windowElapsed) {
        // Window closed; ML takes over. F-11: symmetric RT release BEFORE clearing state.
        writeColdStartWindow("converged", pkg, 0);
        releaseColdBoost(pkg);
        // P1 round7: only clear if we still own this cold pkg (a foreground switch
        // during the dumpsys fork may have started a NEW cold pkg).
        {
            std::lock_guard<std::mutex> lk(s_coldMutex);
            if (s_coldPkg != pkg) {
                LOG_WARN("[cold] window elapsed but s_coldPkg=%s != %s; skip clear",
                         s_coldPkg.c_str(), pkg.c_str());
                return;
            }
            s_active = false; s_convergeStreak = 0; s_isCold = false;
            s_coldPkg.clear();
        }
        LOG_INFO("[P1-7] cold-start window elapsed for %s (%lldms), handing off to ML",
                 pkg.c_str(), age);
        return;
    }

    // ROUND-014 P3 (SPEC 2.7): record the per-tick window phase.
    long long remain = winMaxMs - age;
    if (freeze)      writeColdStartWindow("skipped_thermal", pkg, remain);
    else if (ioPress) writeColdStartWindow("skipped_io", pkg, remain);
    else             writeColdStartWindow("displayed_wait", pkg, remain);

    // ROUND-014 P3 (S14-07): thermal SEVERE+ freeze — skip RT presets this tick, do NOT
    // disarm (resume after thermal clears).
    if (freeze) return;
    // ROUND-014 P3: IO-bound launch — CPU uclamp cannot help; downgrade (skip) this tick.
    if (ioPress) return;

    // Snapshot (tid, role, level) under g_threadsMutex; apply priority AFTER
    // releasing the lock (aatpeSetThreadPriority does /proc + syscalls — must
    // not hold g_threadsMutex).
    struct Job { int tid; ThreadRole role; int level; };
    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        for (const auto& kv : g_threads) {
            const ThreadInfo& t = kv.second;
            if (t.pname != pkg) continue;
            Preset p = presetForThread(t);
            if (p.level > 0) jobs.push_back({t.tid, p.role, p.level});
        }
    }
    for (const Job& j : jobs) {
        aatpe_priority::aatpeSetThreadPriority(j.tid, j.role, j.level);
    }
}

bool coldStartActive() {
    std::lock_guard<std::mutex> lk(s_coldMutex);
    return s_active;
}

std::string coldStartPkg() {
    std::lock_guard<std::mutex> lk(s_coldMutex);
    return s_coldPkg;  // by value (P1-13)
}

}  // namespace aatpe
