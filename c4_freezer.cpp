// c4_freezer.cpp — see c4_freezer.h for the contract.
#include "c4_freezer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>

namespace aatpe {
namespace c4freezer {

// ---- PURE decision layer (always compiled; host-tested) -----------------------
C4Decision decideC4Freeze(const C4DecisionInputs& in) {
    C4Decision d;

    // Gate off: byte-identical to v4.4.3 — never touch any cgroup.
    if (!in.featureEnabled) {
        d.action = C4Action::NO_CHANGE;
        d.reason = "gate-off";
        return d;
    }

    // Safe-first: any foreground release edge wins. We never leave an app frozen
    // after it returns to foreground, even if every other signal is ambiguous.
    if (in.isForeground) {
        if (in.alreadyFrozen) {
            d.action = C4Action::UNFREEZE;
            d.reason = "foreground-release";
        } else {
            d.action = C4Action::NO_CHANGE;
            d.reason = "foreground-idle";
        }
        return d;
    }

    // Background:
    // Audio whitelist — never freeze an app that may be playing audio (music /
    // voice chat). audioActive=true covers BOTH "positively playing" and
    // "unknown (could not verify)" so we never freeze on an ambiguous verdict.
    // MJ-2: if audio STARTS while we are already frozen (mid-freeze music), we must
    // UNFREEZE rather than hold the freeze.
    if (in.audioActive) {
        if (in.alreadyFrozen) {
            d.action = C4Action::UNFREEZE;
            d.reason = "audio-started-mid-freeze";
        } else {
            d.action = C4Action::NO_CHANGE;
            d.reason = "audio-whitelist";
        }
        return d;
    }

    // No freezer backend available (v2 unified freezer absent AND v1 freezer
    // hierarchy absent). v6.1.0: v1 now counts as available, so this branch only
    // fires when BOTH backends are missing.
    if (!in.freezerAvailable) {
        d.action = C4Action::NO_CHANGE;
        d.degraded = true;
        d.reason = "no-freezer";
        return d;
    }

    // Already frozen -> idempotent, no redundant write.
    if (in.alreadyFrozen) {
        d.action = C4Action::NO_CHANGE;
        d.reason = "already-frozen";
        return d;
    }

    d.action = C4Action::FREEZE;
    d.reason = "background-freeze";
    return d;
}

// ---- v4.4.4 rewire: pure helpers (always compiled; host-tested) --------------
std::string parseV2CgroupPath(const std::string& content) {
    std::string path;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(pos, eol - pos);
        // trim trailing CR/space
        while (!line.empty() && (line.back()=='\r' || line.back()==' ' || line.back()=='\t'))
            line.pop_back();
        if (line.rfind("0::", 0) == 0) {
            std::string p = line.substr(3);
            while (!p.empty() && p.front() == '/') p.erase(p.begin());
            if (!p.empty()) { path = p; break; }
        }
        pos = eol + 1;
    }
    return path; // "" when none found
}

std::string buildFreezeDir(const std::string& v2Path, const std::string& mountRoot) {
    if (v2Path.empty()) return "";
    std::string root = mountRoot;
    while (!root.empty() && root.back() == '/') root.pop_back();
    return root + "/" + v2Path;
}

bool mediaSessionMentionsPlaying(const std::string& output, const std::string& pkg) {
    if (output.empty() || pkg.empty()) return false;
    // Walk block-by-block: a session block header contains the package name; the
    // playback state line "playback state=3" (STATE_PLAYING) appears in that
    // block. We track pkg presence within the current block and a state=3 seen
    // since the block started. Simple, conservative, host-testable.
    bool inPkgBlock = false;
    bool pkgInBlock = false;
    bool state3InBlock = false;
    size_t pos = 0;
    auto flushBlock = [&]() -> bool {
        if (pkgInBlock && state3InBlock) return true;
        pkgInBlock = false; state3InBlock = false; inPkgBlock = false;
        return false;
    };
    while (pos < output.size()) {
        size_t eol = output.find('\n', pos);
        if (eol == std::string::npos) eol = output.size();
        std::string line = output.substr(pos, eol - pos);
        // A new top-level block line (indented less) resets state.
        bool blockStart = !line.empty() && (line.front()==' ' || line.front()=='\t');
        if (!blockStart) {
            if (flushBlock()) return true;
        }
        // Package reference (e.g. "package=com.foo" or "com.foo/...").
        if (line.find(pkg) != std::string::npos) pkgInBlock = true;
        // Playback state 3.
        if (line.find("state=3") != std::string::npos ||
            line.find("playback state=3") != std::string::npos) state3InBlock = true;
        inPkgBlock = pkgInBlock;
        pos = eol + 1;
    }
    return (pkgInBlock && state3InBlock);
}

bool cgroupPathBelongsToUid(const std::string& v2Path, int appUid) {
    if (appUid <= 0 || v2Path.empty()) return false;
    std::string want = "uid_" + std::to_string(appUid);
    // Walk each '/' segment exactly.
    size_t pos = 0;
    while (pos <= v2Path.size()) {
        size_t slash = v2Path.find('/', pos);
        if (slash == std::string::npos) slash = v2Path.size();
        std::string seg = v2Path.substr(pos, slash - pos);
        if (seg == want) return true;
        pos = slash + 1;
    }
    return false;
}

// ---- v6.1.0: cgroup v1 freezer pure helpers ----------------------------------
std::string parseV1FreezerPath(const std::string& content) {
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(pos, eol - pos);
        // v1 line format: <id>:<ctrl1,ctrl2,...>:<path>
        size_t c1 = line.find(':');
        if (c1 == std::string::npos) { pos = eol + 1; continue; }
        size_t c2 = line.find(':', c1 + 1);
        if (c2 == std::string::npos) { pos = eol + 1; continue; }
        std::string ctrls = line.substr(c1 + 1, c2 - c1 - 1);
        // Split controller list by comma; look for exact "freezer".
        bool hasFreezer = false;
        size_t p = 0;
        while (p <= ctrls.size()) {
            size_t comma = ctrls.find(',', p);
            if (comma == std::string::npos) comma = ctrls.size();
            std::string ctrl = ctrls.substr(p, comma - p);
            if (ctrl == "freezer") { hasFreezer = true; break; }
            p = comma + 1;
        }
        if (!hasFreezer) { pos = eol + 1; continue; }
        std::string pth = line.substr(c2 + 1);
        while (!pth.empty() && pth.front() == '/') pth.erase(pth.begin());
        if (!pth.empty()) return pth;
        pos = eol + 1;
    }
    return "";
}

std::string parseV1FreezerState(const std::string& content) {
    std::string s = content;
    while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||
                           s.back()==' '||s.back()=='\t'))
        s.pop_back();
    if (s == "FROZEN" || s == "THAWED" || s == "FREEZING" || s == "THAWING")
        return s;
    return "";
}

bool isV1FreezerDir(const std::string& dir) {
    // v1 freezer cgroup dirs contain "/freezer/" in their path
    // (e.g. /sys/fs/cgroup/freezer/uid_10123). v2 dirs do not.
    return dir.find("/freezer/") != std::string::npos;
}

bool audioReevalDue(int bgTicks, int period) {
    if (period <= 0) period = kAudioReevalPeriod;
    if (bgTicks < 0) return false;
    if (bgTicks == 0) return true;           // background edge
    return (bgTicks % period) == 0;
}

// ---- v4.4.5 Part A: NN persistence + MN retarget (pure) ---------------------
std::string serializeC4Frozen(const std::string& cgDir, const std::string& pkg,
                              int uid, const std::string& lastGame) {
    std::string s = "cg=" + cgDir + "\npkg=" + pkg + "\nuid=" + std::to_string(uid) + "\n";
    if (!lastGame.empty()) s += "lg=" + lastGame + "\n";
    return s;
}

C4FrozenState parseC4Frozen(const std::string& content) {
    C4FrozenState s;
    std::string cg, pkg, lg; int uid = 0;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(pos, eol - pos);
        while (!line.empty() && (line.back()=='\r'||line.back()==' '||line.back()=='\t'))
            line.pop_back();
        if (line.rfind("cg=", 0) == 0)       cg  = line.substr(3);
        else if (line.rfind("pkg=", 0)== 0)  pkg = line.substr(4);
        else if (line.rfind("lg=", 0) == 0)  lg  = line.substr(3); // v4.4.6 optional
        else if (line.rfind("uid=", 0)== 0) {
            std::string v = line.substr(4);
            if (!v.empty() && std::all_of(v.begin(), v.end(),
                                          [](char c){return c>='0'&&c<='9';})) {
                uid = std::atoi(v.c_str());
            }
        }
        pos = eol + 1;
    }
    if (cg.empty() || pkg.empty() || uid <= 0) return s; // valid stays false
    s.cgDir = cg; s.pkg = pkg; s.uid = uid; s.lastGame = lg; s.valid = true;
    return s;
}

std::string c4ChooseTarget(const std::string& lastGame,
                           const std::string& fgPkg,
                           bool fgIsGame) {
    if (fgIsGame) return fgPkg;            // foreground game is the (new) target
    if (!lastGame.empty()) return lastGame; // non-game fg: freeze last bg game
    return "";                             // nothing tracked
}

// PURE (v4.4.6 WebUI; v6.1.0 backend): read-only JSON snapshot.
std::string c4StateJson(bool enabled, const std::string& target, bool frozen,
                        const std::string& cgDir, const std::string& lastAction,
                        bool audioActive, bool freezerAvailable,
                        const std::string& freezerBackend,
                        const std::string& degraded) {
    auto q = [](const std::string& v) {
        std::string o = "\"";
        for (char c : v) {
            if (c == '"' || c == '\\') o += '\\';
            o += c;
        }
        o += '"';
        return o;
    };
    std::string j = "{";
    j += "\"enabled\":" + std::string(enabled ? "true" : "false");
    j += ",\"target\":" + q(target);
    j += ",\"frozen\":" + std::string(frozen ? "true" : "false");
    j += ",\"cgDir\":" + q(cgDir);
    j += ",\"lastAction\":" + q(lastAction);
    j += ",\"audioActive\":" + std::string(audioActive ? "true" : "false");
    j += ",\"freezerAvailable\":" + std::string(freezerAvailable ? "true" : "false");
    j += ",\"freezerBackend\":" + q(freezerBackend);
    j += ",\"degraded\":" + q(degraded);
    j += "}";
    return j;
}

} // namespace c4freezer
} // namespace aatpe

// ---- Production glue (compiled out under AATPE_HOST_TEST) --------------------
#ifndef AATPE_HOST_TEST

#include <unistd.h>
#include <dirent.h>
#include <cstdlib>
#include <sys/stat.h>
#include <atomic>
#include <mutex>

#include "aatpe_managers.h"      // ForegroundManager::instance().getPackage()

// Hardened exec helper (3s wall-clock timeout). Forward-declared to avoid pulling
// the heavy aatped_utils.h (which depends on ThreadRule/ThreadInfo types declared
// in other headers). Defined in aatped_utils.cpp.
std::string aatpeExec(const std::string& cmd, const char* debugTag);
// STATE helpers (defined in aatped_utils.cpp). NOTE: writeFileAtomic/readFile
// take a FULL path — they do NOT prepend STATE_DIR (unlike writeFile family in
// some other TUs). Callers must build "STATE_DIR/<name>" themselves, exactly
// like frame.cpp does (writeFile(std::string(STATE_DIR)+"/frame_source", ...)).
// Forward-declared to keep this TU self-contained.
std::string readFile(const char* path);
bool writeFileAtomic(const char* path, const std::string& content);
// STATE_DIR root (defined in aatped_main.cpp; "/dev/aatpe/state" on Android).
// Forward-declared here rather than including aatped.h (same pattern as
// aatpe_caps.cpp). c4 state files MUST live under it (v6.0.0 review P1).
extern const char* STATE_DIR;
// Manual game list (defined in frame.cpp: aatpe::gamelist::isManualGame).
// Forward-declared in the aatpe namespace to keep this TU self-contained
// (avoids pulling aatped.h's heavy include graph).
namespace aatpe { namespace gamelist { bool isManualGame(const std::string& pkg); } }

namespace aatpe {
namespace c4freezer {

// On-disk paths (v6.0.0 review P1): resolve STATE_DIR ONCE, lazily on first use.
// The old code used the bare basename "c4_frozen_state" and passed it straight to
// writeFileAtomic/readFile, which open paths relative to the DAEMON's CWD — the
// file landed wherever the service happened to be running from, not under
// STATE_DIR, so WebUI/root bridge consumers (which all read STATE_DIR/) never saw
// it. Function-local statics avoid static-init order issues with test-provided
// STATE_DIR (the first tick happens long after main()).
static const std::string& c4StateFilePath() {
    static const std::string path =
        std::string(STATE_DIR ? STATE_DIR : "") + "/c4_frozen_state";
    return path;
}
static const std::string& c4LastActionPath() {
    static const std::string path =
        std::string(STATE_DIR ? STATE_DIR : "") + "/c4_last_action";
    return path;
}

// v6.0.0 review P1 test seam: proves no slow I/O (exec / file read-write) happens
// while s_stateMutex is held. Compiled ONLY into host glue tests — release builds
// never define C4_FREEZER_TEST_SEAM (zero symbols, zero overhead in release).
#ifdef C4_FREEZER_TEST_SEAM
std::atomic<int> g_c4freezerLockDepth{0};
struct C4LockProbe {
    C4LockProbe()  { g_c4freezerLockDepth.fetch_add(1, std::memory_order_relaxed); }
    ~C4LockProbe() { g_c4freezerLockDepth.fetch_sub(1, std::memory_order_relaxed); }
};
#else
struct C4LockProbe { C4LockProbe() {} };
#endif

// Slow-thread bookkeeping.
//
// R2-M1: these statics are MUTATED only by c4FreezerTick() (one slow thread) but
// READ by c4FreezerStateJson() on the WebUI/control thread. The exported strings
// (s_frozenCgDir / s_targetPkg / s_lastAction) commonly exceed the libc++ SSO
// cap, so a concurrent read during assignment can tear the (pointer,size) pair
// or read a dangling heap buffer.
//
// v6.0.0 review P1 (lock granularity): the tick previously held this mutex for
// its ENTIRE body — including exec("pm list packages"), exec("dumpsys
// media_session"), opendir("/proc") + per-task /proc/<pid>/cgroup reads, sysfs
// probes, cgroup.freeze writes and STATE_DIR file writes. Any of those can take
// tens of ms (dumpsys especially), and every one of them blocked the WebUI
// snapshot thread for the whole duration. The tick now runs in three phases:
//   1. read the persisted state file + sample managers (lock-free);
//   2. SHORT critical section: copy shared bookkeeping into locals;
//   3. ALL slow work (exec / proc scan / sysfs / state-file writes) lock-free;
//   4. SHORT critical section: publish updated locals back to the statics.
// The WebUI reader therefore only ever waits on two microsecond-length windows.
static std::mutex  s_stateMutex;
static bool        s_alreadyFrozen = false;
static std::string s_frozenCgDir;     // cgroup dir we actually froze
static std::string s_targetPkg;       // current foreground game we may freeze
static int         s_targetUid = 0;   // resolved uid of s_targetPkg (0 = unknown)
static std::string s_lastGame;        // MN: last foreground game (kept as target under launcher)
static bool        s_audioActive = false; // persisted across background episode
static int         s_bgTicks = -1;    // ticks since entering bg (-1 = foreground)
static std::string s_lastAction;      // v4.4.6: last decision action+reason (WebUI)

// Cgroup mount root. On Android this is /sys/fs/cgroup. Overridable under
// C4_FREEZER_TEST_SEAM so host glue tests can point it at a fake tree.
static std::string& cgroupMountRootRef() {
    static std::string root = "/sys/fs/cgroup";
    return root;
}
static const std::string& cgroupMountRoot() { return cgroupMountRootRef(); }

#ifdef C4_FREEZER_TEST_SEAM
void c4FreezerTestSetMountRoot(const std::string& root) {
    cgroupMountRootRef() = root;
}
#endif

static bool freezerControllerPresent() {
    FILE* f = std::fopen((cgroupMountRoot() + "/cgroup.controllers").c_str(), "r");
    if (!f) return false;
    char buf[256] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf)-1, f);
    std::fclose(f);
    return n > 0 && std::strstr(buf, "freezer") != nullptr;
}

// Is <dir>/cgroup.freeze present AND writable by us?
static bool freezeNodeWritable(const std::string& dir) {
    if (dir.empty()) return false;
    std::string p = dir + "/cgroup.freeze";
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    return ::access(p.c_str(), W_OK) == 0;
}

// ---- v6.1.0: cgroup v1 freezer backend --------------------------------------

// v1 freezer hierarchy mounted at <root>/freezer/. Present if the directory
// exists and contains a freezer.state node.
static bool v1FreezerPresent() {
    std::string dir = cgroupMountRoot() + "/freezer";
    struct stat st;
    if (::stat(dir.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    std::string fs = dir + "/freezer.state";
    return ::access(fs.c_str(), F_OK) == 0;
}

// Is <dir>/freezer.state present AND writable?
static bool v1FreezeNodeWritable(const std::string& dir) {
    if (dir.empty()) return false;
    std::string p = dir + "/freezer.state";
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    return ::access(p.c_str(), W_OK) == 0;
}

// Locate the v1 freezer cgroup for appUid: <root>/freezer/uid_<uid>/.
// Direct path construction (no /proc scan needed on v1): the uid gate is
// built into the directory name, so system/root processes (which have no
// uid_<appUid> directory under freezer) are never touched.
static std::string findTargetV1CgroupDir(int appUid) {
    if (appUid <= 0) return "";
    std::string dir = cgroupMountRoot() + "/freezer/uid_" + std::to_string(appUid);
    struct stat st;
    if (::stat(dir.c_str(), &st) != 0) return "";
    if (!S_ISDIR(st.st_mode)) return "";
    return dir;
}

// Read a small text file into a string (for v1 freezer.state read-back).
static std::string readSmallFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return "";
    char buf[256] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf)-1, f);
    std::fclose(f);
    return std::string(buf, n);
}

// Write FROZEN/THAWED to <dir>/freezer.state, then read back to confirm the
// kernel actually transitioned. Brief retry for FREEZING/THAWING transient states.
static bool v1WriteFreeze(const std::string& dir, bool freeze) {
    std::string p = dir + "/freezer.state";
    FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return false;
    const char* val = freeze ? "FROZEN" : "THAWED";
    int rc = std::fprintf(f, "%s\n", val);
    int ce = std::fclose(f);
    if (rc < 1 || ce != 0) return false;

    std::string want = freeze ? "FROZEN" : "THAWED";
    for (int i = 0; i < 10; i++) {
        std::string state = parseV1FreezerState(readSmallFile(p));
        if (state == want) return true;
        if (state == "FREEZING" || state == "THAWING") {
            usleep(5000);  // 5 ms, wait for transition
            continue;
        }
        return false;  // wrong state or unknown
    }
    return false;  // timed out
}

// v2 freeze writer (original).
static bool writeFreezeV2(const std::string& dir, int val) {
    std::string p = dir + "/cgroup.freeze";
    FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return false;
    int rc = std::fprintf(f, "%d", val);
    int ce = std::fclose(f);
    return (rc >= 1 && ce == 0);
}

// Dispatch: choose v1 or v2 writer based on the cgroup dir path.
static bool applyFreezeState(const std::string& dir, bool freeze) {
    if (isV1FreezerDir(dir)) return v1WriteFreeze(dir, freeze);
    return writeFreezeV2(dir, freeze ? 1 : 0);
}

// Resolve an Android app uid from its package name. Returns 0 on failure.
// Uses `pm list packages -U <pkg>` (output "package:com.x uid:10123").
static int resolveAppUid(const std::string& pkg) {
    if (pkg.empty()) return 0;
    std::string out = aatpeExec(("pm list packages -U " + pkg).c_str(), "c4freezer-uid");
    size_t u = out.find("uid:");
    if (u == std::string::npos) return 0;
    u += 4;
    int uid = 0; size_t i = u;
    while (i < out.size() && out[i] >= '0' && out[i] <= '9') { uid = uid*10 + (out[i]-'0'); i++; }
    return uid;
}

// Read /proc/<pid>/status and return the real (first Uid:) value, or -1.
static int procUid(int pid) {
    std::string p = "/proc/" + std::to_string(pid) + "/status";
    FILE* f = std::fopen(p.c_str(), "r");
    if (!f) return -1;
    char line[256]; int uid = -1;
    while (fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "Uid:", 4) == 0) {
            // "Uid:\t10123\t10123\t..."
            unsigned ruid=0, euid=0, suid=0, fsuid=0;
            if (std::sscanf(line, "Uid:\t%u\t%u\t%u\t%u", &ruid,&euid,&suid,&fsuid) == 4) {
                uid = (int)ruid;
            }
            break;
        }
    }
    std::fclose(f);
    return uid;
}

// MJ-1: enumerate /proc, filter by target uid, and return the cgroup dir of a
// thread whose v2 path contains "uid_<appUid>". System/root processes have their
// own cgroups (no uid_ segment) and are NEVER returned -> we can't freeze them.
static std::string findTargetV2CgroupDir(int appUid) {
    if (appUid <= 0) return "";
    DIR* proc = ::opendir("/proc");
    if (!proc) return "";
    std::string found;
    struct dirent* de;
    while ((de = ::readdir(proc)) != nullptr) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int pid = std::atoi(de->d_name);
        if (pid <= 0) continue;
        if (procUid(pid) != appUid) continue;   // <-- uid gate: never touch others
        std::string cf = std::string("/proc/") + de->d_name + "/cgroup";
        FILE* f = std::fopen(cf.c_str(), "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            std::string l = line;
            if (l.rfind("0::", 0) == 0) {
                std::string p = l.substr(3);
                while (!p.empty() && p.front()=='/') p.erase(p.begin());
                // Pure membership check: only accept a path under this uid.
                if (!p.empty() && cgroupPathBelongsToUid(p, appUid)) {
                    found = buildFreezeDir(p, cgroupMountRoot());
                    break;
                }
            }
        }
        std::fclose(f);
        if (!found.empty()) break;
    }
    ::closedir(proc);
    return found;
}

// Run dumpsys media_session; returns true iff target pkg is actively playing.
// exec failure / timeout / empty -> true (conservative: unknown = do not freeze).
static bool audioActiveNow(const std::string& pkg) {
    std::string out = aatpeExec("dumpsys media_session", "c4freezer-media");
    if (out.empty()) return true; // unknown -> conservative, do not freeze
    return mediaSessionMentionsPlaying(out, pkg);
}

void c4FreezerTick() {
    if (!c4FreezerEnabled()) return;

    // ======================================================================
    // PHASE 1 (lock-free): read the persisted state file once + sample managers.
    // NN: once per process, reconcile a persisted frozen state across daemon
    // restart. The state-file READ happens here, OUTSIDE the mutex (the old code
    // held s_stateMutex across this read, and across every exec/sysfs call below).
    // ======================================================================
    static bool s_loaded = false;      // mutated only by the single slow thread
    C4FrozenState restored;
    bool haveRestore = false;
    if (!s_loaded) {
        s_loaded = true;
        restored = parseC4Frozen(readFile(c4StateFilePath().c_str()));
        haveRestore = restored.valid;
    }

    // ForegroundManager is internally synchronized; no need for our mutex here.
    std::string fg = ForegroundManager::instance().getPackage();
    bool fgIsGame = !fg.empty() && gamelist::isManualGame(fg);

    // ======================================================================
    // PHASE 2 (SHORT critical section): snapshot the shared bookkeeping into
    // locals. No I/O, no exec, no sysfs in here — pure copies.
    // ======================================================================
    bool alreadyFrozen, audioActive;
    int  targetUid, bgTicks;
    std::string frozenCgDir, targetPkg, lastGame;
    {
        std::lock_guard<std::mutex> stateLk(s_stateMutex);
        C4LockProbe probe;
        alreadyFrozen = s_alreadyFrozen;
        audioActive   = s_audioActive;
        targetUid     = s_targetUid;
        bgTicks       = s_bgTicks;
        frozenCgDir   = s_frozenCgDir;
        targetPkg     = s_targetPkg;
        lastGame      = s_lastGame;
    }

    // Apply the NN restore to the local working copy. If the persisted game is
    // back in foreground, release it symmetrically (sysfs write + state-file
    // clear, both OUTSIDE the lock) — identical to the old inline restore-release.
    if (haveRestore) {
        if (fg == restored.pkg) {
            applyFreezeState(restored.cgDir, false);
            writeFileAtomic(c4StateFilePath().c_str(), "");
        } else {
            alreadyFrozen = true;
            frozenCgDir   = restored.cgDir;
            targetPkg     = restored.pkg;
            targetUid     = restored.uid;
            // NN-NIT(v4.4.6): restore lastGame too (optional field; old files
            // omit it -> empty, safe).
            if (!restored.lastGame.empty()) lastGame = restored.lastGame;
        }
    }

    // ======================================================================
    // PHASE 3 (lock-free): ALL slow work — exec, /proc scan, sysfs probes and
    // writes, STATE_DIR file writes — runs here on the locals only.
    // ======================================================================

    // MN: refresh the last foreground game. fgIsGame drives c4ChooseTarget.
    if (fgIsGame) lastGame = fg;

    // Choose the freeze target: foreground game -> that game; launcher/non-game ->
    // the previously-backgrounded lastGame; nothing tracked -> "".
    std::string target = c4ChooseTarget(lastGame, fg, fgIsGame);

    // Release a held freeze only when the TARGET actually changes (old bug: any
    // non-game foreground released the backgrounded game). Switching games / losing
    // the target releases; launcher appearing does NOT.
    if (alreadyFrozen && target != targetPkg && !frozenCgDir.empty()) {
        applyFreezeState(frozenCgDir, false);
        alreadyFrozen = false;
        frozenCgDir.clear();
        writeFileAtomic(c4StateFilePath().c_str(), "");   // NN: clear persisted record
    }
    if (target.empty()) {
        targetUid = 0;
        lastGame.clear();          // target gone (process exited / no game)
    } else if (targetUid == 0) {
        targetUid = resolveAppUid(target);              // exec (slow) — lock-free
    }

    // isForeground = the TARGET game is the one currently on screen. Under a
    // launcher this is false -> the backgrounded game stays freezable.
    bool isFg = !target.empty() && fg == target;

    if (isFg) {
        // Foreground edge: reset background episode bookkeeping.
        bgTicks = -1;
        audioActive = false;
    } else if (!target.empty()) {
        // Background: advance tick counter (0 = first background tick this episode).
        if (bgTicks < 0) bgTicks = 0; else bgTicks++;

        // MJ-2: persisted audio + periodic re-eval (edge + every kAudioReevalPeriod).
        if (audioReevalDue(bgTicks, kAudioReevalPeriod)) {
            audioActive = audioActiveNow(target);        // exec (slow) — lock-free
        }
    } else {
        bgTicks = -1;
        audioActive = false;
    }

    // Resolve cgroup dir (only when we hold no frozen dir).
    // v6.1.0: try v2 unified first; fall back to v1 freezer hierarchy.
    std::string cgDir;
    std::string backend;       // "v2" / "v1" / ""
    if (alreadyFrozen) {
        cgDir = frozenCgDir;
        backend = isV1FreezerDir(cgDir) ? "v1" : "v2";
    } else {
        // v2 priority
        cgDir = findTargetV2CgroupDir(targetUid);
        if (!cgDir.empty() && freezerControllerPresent() && freezeNodeWritable(cgDir)) {
            backend = "v2";
        } else {
            cgDir.clear();
        }
        // v1 auto-fallback
        if (cgDir.empty()) {
            std::string v1Dir = findTargetV1CgroupDir(targetUid);
            if (!v1Dir.empty() && v1FreezerPresent() && v1FreezeNodeWritable(v1Dir)) {
                cgDir = v1Dir;
                backend = "v1";
            }
        }
    }
    // If the target no longer maps to a real (background) cgroup, drop target.
    if (cgDir.empty() && !alreadyFrozen && !target.empty()) {
        lastGame.clear();
        target.clear();
        targetUid = 0;
    }

    C4DecisionInputs in;
    in.featureEnabled   = true;
    in.isForeground     = isFg;
    in.alreadyFrozen    = alreadyFrozen;
    in.freezerAvailable = !cgDir.empty() && !backend.empty();
    in.audioActive      = audioActive;

    // lastAction is recomputed from the decision every tick (the snapshot value
    // is only history); it is published back to the statics in phase 4.
    std::string lastAction;
    C4Decision d = decideC4Freeze(in);
    if (d.action == C4Action::FREEZE) {
        lastAction = alreadyFrozen ? "FREEZE(already)" : ("FREEZE(" + backend + ")");
        if (applyFreezeState(cgDir, true)) {
            alreadyFrozen = true;
            frozenCgDir   = cgDir;
            writeFileAtomic(c4StateFilePath().c_str(),
                            serializeC4Frozen(cgDir, target, targetUid, lastGame));
        } else {
            lastAction = "FREEZE(write-fail-degraded)";
        }
    } else if (d.action == C4Action::UNFREEZE) {
        lastAction = audioActive ? "UNFREEZE(audio-started-mid-freeze)"
                                 : "UNFREEZE(foreground)";
        if (!frozenCgDir.empty()) applyFreezeState(frozenCgDir, false);
        alreadyFrozen = false;
        frozenCgDir.clear();
        writeFileAtomic(c4StateFilePath().c_str(), "");   // NN: clear persisted record
    } else {
        lastAction = "NO_CHANGE";
    }

    // P2 (v6.0.0 review): publish the last decision as a diagnostic state file so
    // the root bridge / WebUI can `cat STATE_DIR/c4_last_action` — same convention
    // as frame.cpp's frame_unavailable_reason / vm_inner_detect_reason. Lock-free.
    writeFileAtomic(c4LastActionPath().c_str(), lastAction);

    // ======================================================================
    // PHASE 4 (SHORT critical section): publish the updated locals back to the
    // exported bookkeeping the WebUI reader snapshots. Again, pure assignments.
    // ======================================================================
    {
        std::lock_guard<std::mutex> stateLk(s_stateMutex);
        C4LockProbe probe;
        s_alreadyFrozen = alreadyFrozen;
        s_frozenCgDir   = frozenCgDir;
        s_targetPkg     = target;
        s_targetUid     = targetUid;
        s_lastGame       = lastGame;
        s_audioActive    = audioActive;
        s_bgTicks        = bgTicks;
        s_lastAction     = lastAction;
    }
}

// Production glue: read-only snapshot for WebUI.
//
// R2-M1: this is called off the WebUI/control thread while c4FreezerTick() runs
// on the slow thread. We take s_stateMutex ONLY to copy the exported scalars and
// strings into locals (short critical section, no I/O), then release it before
// probing sysfs / building JSON. The copied locals are immune to concurrent
// mutation, so no string can tear mid-export.
std::string c4FreezerStateJson() {
    bool en = c4FreezerEnabled();

    // --- short critical section: snapshot the exported bookkeeping -----------
    bool alreadyFrozen, audioActive;
    std::string targetPkg, frozenCgDir, lastAction;
    {
        std::lock_guard<std::mutex> lk(s_stateMutex);
        alreadyFrozen = s_alreadyFrozen;
        audioActive   = s_audioActive;
        targetPkg     = s_targetPkg;
        frozenCgDir   = s_frozenCgDir;
        lastAction    = s_lastAction;
    }
    // --- lock released; I/O + JSON building run lock-free --------------------

    // v6.1.0: determine which freezer backend is actually available.
    bool hasV2 = freezerControllerPresent();
    bool hasV1 = v1FreezerPresent();
    std::string backend = hasV2 ? "v2" : (hasV1 ? "v1" : "");
    bool freezerAvail = !backend.empty();

    std::string degraded;
    if (!en) degraded = "disabled";
    else if (targetPkg.empty()) degraded = "no-game-target";
    else if (!frozenCgDir.empty()) {
        // Check the frozen dir's node is writable via its own backend.
        bool writable = isV1FreezerDir(frozenCgDir)
                            ? v1FreezeNodeWritable(frozenCgDir)
                            : freezeNodeWritable(frozenCgDir);
        if (!writable) degraded = "freeze-node-not-writable";
    }
    if (degraded.empty() && !freezerAvail) degraded = "no-freezer-controller";
    return c4StateJson(en, targetPkg, alreadyFrozen, frozenCgDir,
                       lastAction, audioActive,
                       freezerAvail, backend, degraded);
}

} // namespace c4freezer
} // namespace aatpe

#endif // AATPE_HOST_TEST
