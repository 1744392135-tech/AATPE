// AATPE Watchdog - process monitoring, emergency recovery, and daemon restart
// P0-2 fix: Restored full daemon restart logic from old version,
//   combined with new-version improvements (runtime CPU detection,
//   critical-thread whitelist, cpu_capacity-based big-core detection).
//
// Compile: NDK clang++ -O2 -std=c++17 -o aatpe-watchdog aatpe-watchdog.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <cerrno>
#include <climits>
#include <cctype>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <spawn.h>
#include <sched.h>
#include <dirent.h>
#include <map>
#include <set>
// Shared CPU thermal-zone selector — the SAME pure header used by thermal.cpp,
// so the watchdog's independent thermal wall agrees with the daemon on which zones
// are trusted CPU dies (no rogue board-sensor disagreement between the two
// binaries). Makefile adds -I shared.
#include "cpu_zone_selector.h"
#include "aatpe-constants.h"
#include "aatpe-version.h"
#include "aatpe_pid_identity.h"

using namespace aatpe;
extern char** environ;

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
static const char* LOG_FILE     = "/dev/aatpe/aatpe-watchdog.log";
static const char* WD_PID_FILE  = "/dev/aatpe/watchdog.pid";
static const char* WD_LOCK_FILE = "/dev/aatpe/watchdog.lock";
static const char* DAEMON_PATH  = "/system/bin/aatped";
// G9-003: explicit-stop request flag written by `aatpe-cli daemon stop`. While
// this file exists the watchdog must NOT auto-restart the daemon (distinguish a
// deliberate user stop from a crash, which leaves no such file). The CLI start/
// restart and service.sh clear it. Root-only 0600 in the state dir.
static const char* STOP_REQUEST_FILE = "/dev/aatpe/state/stop_request";

// passive-mode cooldown uses watchdog::kPassiveCooldownSec (300s)
// defined in aatpe-constants.h; no local alias needed (avoids -Wunused-const-variable).

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------
static std::atomic<sig_atomic_t> g_shouldExit{0};
static void signalHandler(int sig) {
    if (sig == SIGTERM || sig == SIGINT)
        g_shouldExit.store(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------
static std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static int readInt(const char* path, int def) {
    std::string s = readFile(path);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

// P0 (crash_loop false-positive fix): distinguish a REAL fatal-signal crash from
// a benign restart. The daemon's crashSignalHandler writes the fatal signal name
// into STATE_DIR/crash_signal before re-raising. Only when that file names one of
// the fatal signals do we treat the death as a hard crash. The watchdog's own
// heartbeat-stale SIGKILL, clean SIGTERM, and service.sh/upgrade restarts leave NO
// crash_signal and must NOT latch the crash_loop PASSIVE.
static bool readHardCrashSignal(std::string& sigOut) {
    std::string v = readFile((std::string(aatpe::paths::kStateDir) + "/crash_signal").c_str());
    sigOut = v;
    return v == "SIGABRT" || v == "SIGSEGV" || v == "SIGBUS" ||
           v == "SIGILL" || v == "SIGFPE";
}
static void clearCrashSignal() {
    ::unlink((std::string(aatpe::paths::kStateDir) + "/crash_signal").c_str());
}
// Consecutive heartbeat-stale cycles required before we SIGKILL a possibly-slow
// daemon. A single stale reading under boot storm / bpftool exec must not kill.
static constexpr int kStaleStrikeBeforeKill = 3;

// W-004: monotonic seconds that INCLUDE time spent suspended. The daemon stamps
// its heartbeat with CLOCK_BOOTTIME; we MUST compare against the same clock here.
// Using wall-clock time(nullptr) would, after a screen-off suspend, see the
// heartbeat "frozen" (wall clock does not advance while suspended) and SIGKILL a
// perfectly healthy daemon on wake. CLOCK_BOOTTIME keeps counting across suspend.
static time_t bootTimeSec() {
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) {
        // Defensive fallback: should never happen on a 4.0+ kernel. If it does,
        // fall back to wall clock rather than crashing the watchdog.
        return time(nullptr);
    }
    return (time_t)ts.tv_sec;
}

static bool writeFile(const char* path, const std::string& content) {
    // P3-12: state files contain internal liveness/crash-loop flags; 0600 keeps
    // them root-only on a world-tmpfs /dev. Was 0644 (world-readable).
    // v4.4.9 R3-F08: add O_NOFOLLOW so a symlink planted at the final path
    // component is rejected instead of followed. The watchdog lock (WD_LOCK_FILE)
    // and pid-file writes already use O_NOFOLLOW; this state-file writer was the
    // lone inconsistent open(). Path lives under a root-0700 state dir, so a
    // non-root cannot plant a link — this is hardening/consistency, not a live
    // privilege-escalation fix.
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    // P2-#8: loop write() until all bytes are written (partial write retry).
    size_t total = content.size();
    size_t off = 0;
    while (off < total) {
        ssize_t n = write(fd, content.c_str() + off, total - off);
        if (n <= 0) { close(fd); return false; }
        off += (size_t)n;
    }
    close(fd);
    return true;
}

static void trim(std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) { s.clear(); return; }
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static FILE* g_wdLogFp = nullptr;
static void wdLogInit() {
    g_wdLogFp = fopen(LOG_FILE, "a");
    if (!g_wdLogFp) {
        // Failed to open log file — logWatchdog() will retry on each write,
        // so we don't abort here. Record to stderr for diagnosability.
        fprintf(stderr, "watchdog: failed to open log file %s: %s\n", LOG_FILE, strerror(errno));
    }
}

// Log rotation. Called from logWatchdog() before each write.
// If the log file exceeds kMaxLogSizeBytes (1 MB), truncate it by closing the
// stream and reopening with "w". /dev is tmpfs so we clear instead of renaming
// to .1 (no extra headroom on tmpfs).
static void rotateLogIfNeeded(FILE** ppFp) {
    if (!ppFp || !*ppFp) return;
    struct stat st;
    if (fstat(fileno(*ppFp), &st) != 0) return;
    if (st.st_size <= aatpe::logging::kMaxLogSizeBytes) return;
    fclose(*ppFp);
    *ppFp = fopen(LOG_FILE, "w");
    if (!*ppFp) return;
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(*ppFp, "[%s] [WATCHDOG] log rotated (size limit %lld bytes)\n",
            ts, (long long)aatpe::logging::kMaxLogSizeBytes);
    fflush(*ppFp);
}

static void logWatchdog(const char* fmt, ...) {
    bool tempOpened = false;
    FILE* fp = g_wdLogFp;
    if (!fp) {
        fp = fopen(LOG_FILE, "a");
        if (!fp) return;
        tempOpened = true;
    }
    // rotate before writing (only on the persistent handle)
    if (!tempOpened) rotateLogIfNeeded(&g_wdLogFp);
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(fp, "[%s] [WATCHDOG] ", ts);
    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fprintf(fp, "\n");
    fflush(fp);
    if (tempOpened) fclose(fp);
}

static void writeState(const char* key, const char* val) {
    std::string path = std::string(aatpe::paths::kStateDir) + "/" + key;
    // R8-011 (LED-118): std::ofstream creates with the default umask (typically
    // 0644, world-readable). State files carry internal liveness/crash-loop flags;
    // route through writeFile() which opens them 0600 root-only.
    writeFile(path.c_str(), val ? val : "");
}

// P1-1: send a control command to the daemon via the root control pipe.
// The daemon reads /dev/aatpe/control each loop and dispatches through
// CommandDispatcher -> executeControlLine. This replaces the old direct
// writeState("control_mode", ...) which only touched the state file without
// updating g_cfg.controlMode inside the daemon.
static void sendControlCommand(const std::string& cmd) {
    std::string path = std::string(aatpe::paths::kRunDir) + "/control";
    std::ofstream f(path, std::ios::app);
    if (f) {
        f << cmd << "\n";
    } else {
        // Daemon not running or pipe not ready — the command is dropped.
        // This is safe: the watchdog's other restart logic will bring the
        // daemon back up, and the next health-check cycle will re-send.
        logWatchdog("WARN: cannot write control pipe %s (daemon down?)", path.c_str());
    }
}

// ---------------------------------------------------------------------------
// P3-53/P3-54/P3-55: explicit watchdog runtime state machine.
//
// Before this change the watchdog kept scattered booleans (passiveMode,
// degradedMode, tempPassiveUntil) and issued sendControlCommand() calls from
// many places, with no single state owner and no recovery hysteresis — so a
// transient fault could ping-pong abnormal -> passive -> active -> abnormal.
//
// The watchdog now owns ONE explicit runtime state. EVERY transition goes
// through wdTransition(), which:
//   1. logs the from->to edge with its reason (P3-54),
//   2. publishes watchdog_runtime_state / watchdog_state_reason to state files,
//   3. routes the daemon control-mode change through the control pipe ->
//      daemon setRuntimeControlMode(mode, reason) (the daemon stays the single
//      executor of g_cfg.controlMode / SystemState).
//
// States:
//   NORMAL    healthy, full scheduling        -> control_mode active
//   DEGRADED  rapid crash loop, reduced       -> control_mode assist
//   ASSIST    light-touch assist              -> control_mode assist
//   PASSIVE   migrations blocked              -> control_mode passive
//   EMERGENCY thermal emergency               -> control_mode emergency
//   RECOVERY  observing after passive/degraded; needs kRecoveryStableSec clean
//             before promoting to NORMAL (P3-55 hysteresis)
// ---------------------------------------------------------------------------
enum class WdRuntimeState {
    NORMAL, DEGRADED, PASSIVE, EMERGENCY, RECOVERY
};
static WdRuntimeState g_wdState = WdRuntimeState::NORMAL;
// wall-clock time we entered RECOVERY; 0 when not recovering.
static time_t g_recoverySince = 0;

// ---------------------------------------------------------------------------
// WD-02: watchdog_degraded is a SHARED state file written by several independent
// subsystems (rapid-crash classifier, binary_broken detector, thermal-thread-stale
// observer). Previously each one overwrote the others' label (e.g. thermal recovery
// spuriously writing "none" wiped a live "rapid_crash_loop" latch). Now the active
// degraded reasons live in an in-memory set and are published as ONE aggregated
// string joined by '+' (e.g. "rapid_crash_loop+thermal_thread_stale") through a
// single choke point, so no reason clobbers another. WD-05 also falls out of this:
// publish only fires when the set actually changes (no per-tick rewrite).
// ---------------------------------------------------------------------------
static std::set<std::string> g_degradedSet;
// Pure: join the active reasons into the on-disk value ("r1+r2"), "normal" when empty.
// Split out so host tests can assert the aggregated string without touching /dev.
static std::string degradedReasonsString() {
    std::string v;
    for (const auto& r : g_degradedSet) {
        if (!v.empty()) v += "+";
        v += r;
    }
    return v.empty() ? "normal" : v;
}
static void publishDegraded() {
    writeState("watchdog_degraded", degradedReasonsString().c_str());
}
static void addDegradedReason(const std::string& r) {
    if (g_degradedSet.insert(r).second) publishDegraded();
}
static void removeDegradedReason(const std::string& r) {
    if (g_degradedSet.erase(r) > 0) publishDegraded();
}
static void clearAllDegradedReasons() {
    if (!g_degradedSet.empty()) { g_degradedSet.clear(); publishDegraded(); }
}

static const char* wdStateName(WdRuntimeState s) {
    switch (s) {
        case WdRuntimeState::NORMAL:    return "NORMAL";
        case WdRuntimeState::DEGRADED:  return "DEGRADED";
        case WdRuntimeState::PASSIVE:   return "PASSIVE";
        case WdRuntimeState::EMERGENCY: return "EMERGENCY";
        case WdRuntimeState::RECOVERY: return "RECOVERY";
    }
    return "UNKNOWN";
}

// Map the watchdog runtime state onto the daemon control modes that
// setRuntimeControlMode() accepts (active/assist/passive/emergency).
static const char* wdControlModeFor(WdRuntimeState s) {
    switch (s) {
        case WdRuntimeState::NORMAL:    return "active";
        case WdRuntimeState::DEGRADED:  return "assist";
        case WdRuntimeState::RECOVERY: return "assist";
        case WdRuntimeState::PASSIVE:   return "passive";
        case WdRuntimeState::EMERGENCY: return "emergency";
    }
    return "assist";
}

// Single choke point for every watchdog state transition. `reason` MUST be a
// short [a-z0-9_] token (it rides the control pipe) so the daemon records it
// in control_mode_reason. Same-state calls are no-ops (no control churn).
// W-016: the reason rides the control pipe and is later echoed into state files
// and the audit log. Sanitize it locally to the [a-z0-9_] whitelist before it
// leaves the watchdog, replacing any other byte with '_', so a stray newline /
// '&' / '=' cannot smuggle a second control command or break the key=value line.
static std::string sanitizeReason(const std::string& reason) {
    std::string out;
    out.reserve(reason.size());
    for (char c : reason) {
        // R8-021 (LED-128): uppercase letters must be folded to lowercase (so e.g.
        // "RapidCrashLoop" -> "rapidcrashloop") instead of being replaced with '_'
        // (which produced "_apid_rash_loop" and made reasons unreadable/instable).
        if (c >= 'A' && c <= 'Z') out.push_back((char)(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') out.push_back(c);
        else out.push_back('_');
    }
    if (out.empty()) out = "unspecified";
    return out;
}

static void wdTransition(WdRuntimeState target, const std::string& reason) {
    WdRuntimeState prev = g_wdState;
    if (prev == target) return;
    const char* mode = wdControlModeFor(target);
    std::string safeReason = sanitizeReason(reason);
    logWatchdog("WSTATE: %s -> %s (control_mode=%s, reason=%s)",
                wdStateName(prev), wdStateName(target), mode, safeReason.c_str());
    writeState("watchdog_runtime_state", wdStateName(target));
    writeState("watchdog_state_reason", safeReason.c_str());
    sendControlCommand("set_control_mode=" + std::string(mode) + "&reason=" + safeReason);
    g_wdState = target;
    if (target == WdRuntimeState::RECOVERY) {
        g_recoverySince = time(nullptr);
    } else {
        g_recoverySince = 0;
    }
}

// ---------------------------------------------------------------------------
// G9-001 / G9-002: PURE steady-state transition function (no globals, no I/O).
//
// This is the single table-driven decision the unit tests drive. It encodes the
// previously-missing recovery edges:
//   * EMERGENCY  -> RECOVERY once thermal has been clear continuously for
//                   emergencyClearSec (was: EMERGENCY had NO exit edge).
//   * RECOVERY    -> NORMAL once the window has stayed clean for recoveryStableSec.
//   * PASSIVE     (temp) -> RECOVERY when no fault fires this cycle (handled by
//                   the temp-passive expiry branch; here a fault re-asserts PASSIVE).
// DEGRADED is promoted by its own cooldown/restart block (it needs to actually
// restart the daemon before observing health); once it does it enters RECOVERY,
// which this function then carries on to NORMAL.
//
// Priority of fault inputs (highest wins): explicitStop (freeze) > thermalEmergency
// > (migOver|loadOver). When no fault fires this cycle the recovery hysteresis
// below applies. 'now' is CLOCK_BOOTTIME seconds.
// ---------------------------------------------------------------------------
struct WdEvidence {
    bool daemonAlive;
    bool thermalEmergency;   // current thermal_emergency == 1
    bool migOver;            // migration rate over limit this sample
    bool loadOver;           // system load over limit this sample
    bool explicitStop;       // stop_request flag present
};

static WdRuntimeState wdSteadyTransition(WdRuntimeState cur, const WdEvidence& ev,
                                         long long now,
                                         long long emergClearSince,
                                         long long recoverySince,
                                         int recoveryStableSec,
                                         int emergencyClearSec) {
    // During an explicit user stop the watchdog freezes: no promotion, no new
    // fault action. (Restart suppression is applied separately in the loop.)
    if (ev.explicitStop) return cur;

    // Highest-priority active fault re-asserts the faulted state.
    if (ev.thermalEmergency) return WdRuntimeState::EMERGENCY;
    if (ev.migOver || ev.loadOver) return WdRuntimeState::PASSIVE;

    // No active fault this cycle: apply recovery hysteresis.
    switch (cur) {
        case WdRuntimeState::EMERGENCY:
            // G9-001 exit edge: thermal has been continuously clear for long
            // enough -> step down to RECOVERY (never straight to NORMAL).
            if (emergClearSince > 0 && (now - emergClearSince) >= emergencyClearSec)
                return WdRuntimeState::RECOVERY;
            return WdRuntimeState::EMERGENCY;
        case WdRuntimeState::RECOVERY:
            if (recoverySince > 0 && (now - recoverySince) >= recoveryStableSec)
                return WdRuntimeState::NORMAL;
            return WdRuntimeState::RECOVERY;
        default:
            // NORMAL / PASSIVE / DEGRADED: fault absent; PASSIVE temp expiry and
            // DEGRADED cooldown are driven by their own timed branches.
            return cur;
    }
}

// True when the explicit-stop flag file exists (root-only state dir).
static bool explicitStopRequested() {
    struct stat st;
    return ::stat(STOP_REQUEST_FILE, &st) == 0;
}

// ---------------------------------------------------------------------------
// ROUND-003: independent thermal backup authority.
//
// EMERGENCY used to exit ONLY when the daemon wrote thermal_emergency=0. If the
// daemon's thermal subsystem ever stalls and never clears the flag (real heat
// already long gone), the watchdog would sit in EMERGENCY forever (conservative,
// migrations off) with no second opinion. Here the watchdog reads the thermal
// sensors itself as a backup authority. The classify function is pure so unit
// tests can drive it; the sysfs scan is the host-device side.
// ---------------------------------------------------------------------------
enum class WdTempRead { HOT, SAFE, UNKNOWN };

// Pure decision on a single independent reading.
//   maxC     : watchdog's own max temp across thermal zones, in °C; -1 if no zone readable.
//   wallC    : the emergency temperature wall in °C (at/above = really hot).
//   marginC  : extra safety margin; only maxC <= wallC-marginC counts as SAFE.
// Grey band (wallC-marginC < maxC < wallC) and "no reading at all" both -> UNKNOWN
// so we never use "couldn't read = cool" as an excuse to drop protection.
static WdTempRead wdClassifyIndTemp(long long maxC, int wallC, int marginC) {
    if (maxC < 0) return WdTempRead::UNKNOWN;
    if (maxC >= wallC) return WdTempRead::HOT;
    if (maxC <= wallC - marginC) return WdTempRead::SAFE;
    return WdTempRead::UNKNOWN;
}

// Pure streak tracker. Returns true once SAFE samples have reached needSamples.
// Any non-SAFE sample resets the streak (anti-flap).
static bool wdIndTempSample(WdTempRead r, int& safeStreak, int needSamples) {
    if (r == WdTempRead::SAFE) {
        if (++safeStreak >= needSamples) return true;
        return false;
    }
    safeStreak = 0;
    return false;
}

// Read every /sys/class/thermal/thermal_zone*, classify them with the SHARED CPU
// zone selector (shared/cpu_zone_selector.h, the same header thermal.cpp uses), and
// return the TRUSTED CPU die temperature in °C.
//
// Root cause: the old version took the untyped GLOBAL max across ALL zones. On
// Dimensity 1200 an intermittent/rogue mtktscpu zone (reads -1, then spikes to
// ~87C) made the watchdog's independent 90C wall disagree with the daemon's
// rogue-filtered WARNING wall -- the two binaries fought over thermal state. Now the
// watchdog derives its temperature from the SAME trusted high side (max over
// non-rogue valid CPU zones) as thermal.cpp, so they never conflict.
//
// Returns -1 if NO CPU-typed zone could be read. The caller treats -1 as UNKNOWN,
// i.e. NEVER drops protection ("no read = cool" is forbidden). Cross-scan history
// (badStreak/spikeStreak) persists in the static map across watchdog loops.
//
// root: injectable sysfs root (production passes "/sys/class/thermal"; hermetic
// tests pass a fake tree). Kept as a parameter so the exact production reader loop
// is what the fixture exercises, without a separate divergent copy.
static long long wdReadMaxTempCelsiusUnder(const char* root) {
    DIR* base = opendir(root);
    if (!base) return -1;

    std::vector<thermzone::ZoneReading> tz;
    struct dirent* de;
    while ((de = readdir(base)) != nullptr) {
        std::string name = de->d_name;
        if (name.rfind("thermal_zone", 0) != 0) continue;
        std::string basePath = std::string(root) + "/" + name;

        // zone type (for the allow/deny classifier). Unreadable type => skip zone.
        std::string typeLower;
        {
            FILE* tf = fopen((basePath + "/type").c_str(), "r");
            if (!tf) continue;
            char buf[128] = {0};
            if (!fgets(buf, sizeof(buf), tf)) { fclose(tf); continue; }
            fclose(tf);
            typeLower = buf;
            while (!typeLower.empty() &&
                   (typeLower.back() == '\n' || typeLower.back() == '\r' ||
                    typeLower.back() == ' '  || typeLower.back() == '\t'))
                typeLower.pop_back();
            for (auto& c : typeLower) c = (char)std::tolower((unsigned char)c);
        }

        long long raw = 0;
        bool haveRaw = false;
        {
            FILE* f = fopen((basePath + "/temp").c_str(), "r");
            if (f) {
                if (fscanf(f, "%lld", &raw) == 1) haveRaw = true;
                fclose(f);
            }
        }

        thermzone::ZoneReading r;
        r.name = name;
        r.typeLower = typeLower;
        // Unit normalization (same three-tier as the daemon): mC / 0.1C / C.
        if (haveRaw && raw > -50 && raw < 200000) {
            int c;
            if (raw > 1500)      c = (int)(raw / 1000);   // millicelsius
            else if (raw > 150)  c = (int)(raw / 10);     // 0.1°C
            else                 c = (int)raw;            // Celsius
            // A live CPU die is always >0C; the driver's "uninitialized/unreadable"
            // sentinel reads -1 (or 0 at boot, same as the daemon's not-ready floor).
            // Treat c<=0 or c>=150 as INVALID this scan so it feeds badStreak, never a
            // bogus -1C sample into the consensus median.
            r.valid = (c > 0 && c < 150);
            r.tempC = r.valid ? c : INT_MIN;
        } else {
            r.valid = false;
            r.tempC = INT_MIN;
        }
        tz.push_back(r);
    }
    closedir(base);

    static std::map<std::string, thermzone::ZoneHist> s_wdTzHist;
    thermzone::SelectResult sel = thermzone::select(tz, s_wdTzHist);
    // Trusted high side. sel.trustedCpuTempC already fail-safes to anyCpu when all
    // zones are rogue; when there is no CPU zone at all it is 0 -> report -1 (UNKNOWN).
    if (sel.trustedCpuTempC > 0) return (long long)sel.trustedCpuTempC;
    return -1;
}

// Production entry point: read the real sysfs tree.
static long long wdReadMaxTempCelsius() {
    return wdReadMaxTempCelsiusUnder("/sys/class/thermal");
}

// ---------------------------------------------------------------------------
// Config reading (uppercase keys, matching aatpe.conf)
// ---------------------------------------------------------------------------
static int readConfigInt(const char* key, int def) {
    // cover all manager layouts. The watchdog is a standalone binary
    // that does not link aatped_utils.cpp, so it probes the canonical module roots
    // inline (Magisk / KernelSU / APatch) instead of hardcoding Magisk only.
    static const char* confPaths[] = {
        "/system/etc/aatpe/aatpe.conf",
        "/data/adb/modules/aatpe/aatpe.conf",
        "/data/adb/ksu/modules/aatpe/aatpe.conf",
        "/data/adb/ap/modules/aatpe/aatpe.conf",
        "/data/adb/modules_update/aatpe/aatpe.conf",
        "/data/aatpe/aatpe.conf",
        "/dev/aatpe/aatpe.conf",
        nullptr
    };
    for (int i = 0; confPaths[i]; i++) {
        std::ifstream f(confPaths[i]);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            size_t hashPos = line.find('#');
            if (hashPos != std::string::npos) line = line.substr(0, hashPos);
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            trim(k);
            trim(v);
            if (k == key) {
                try { return std::stoi(v); } catch (...) { return def; }
            }
        }
        // P2-#9: file opened but key not found — continue to next config path,
        // do NOT return def prematurely (an earlier file may shadow a later one
        // that actually has the key).
    }
    return def;
}

// ---------------------------------------------------------------------------
// Runtime CPU detection (no hardcoded g_cpuCount)
// ---------------------------------------------------------------------------
// WD-13: sysconf(_SC_NPROCESSORS_ONLN) is the primary source. If it returns
// <=0 (rare: a sandboxed host without a proper /proc/stat, or a kernel bug),
// enumerate /sys/devices/system/cpu/cpu[0-9]+ directories instead of falling
// straight back to a hardcoded 8. The hardcoded 8 is now the LAST resort and
// is annotated in a log so a field report can tell "real sysfs count" from
// "blind fallback".
static int countCpuDirs(const char* sysBase) {
    std::string base = std::string(sysBase) + "/devices/system/cpu/";
    DIR* d = ::opendir(base.c_str());
    if (!d) return 0;
    int n = 0;
    struct dirent* de;
    while ((de = ::readdir(d)) != nullptr) {
        // cpu0, cpu1, ... — must start with "cpu" followed by a digit.
        const char* p = de->d_name;
        if (p[0] != 'c' || p[1] != 'p' || p[2] != 'u') continue;
        if (p[3] < '0' || p[3] > '9') continue;
        n++;
    }
    ::closedir(d);
    return n;
}

static int getCpuCount() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int)n;
    // WD-13: sysconf unreliable — probe /sys/devices/system/cpu.
    int sysN = countCpuDirs("/sys");
    if (sysN > 0) return sysN;
    // Truly blind fallback. Log once so a field log shows why we used 8.
    logWatchdog("WARN: getCpuCount sysconf and /sys enumeration both failed; "
                "using conservative fallback=8 (may under/over-count on >8 cores)");
    return 8;
}

// ---------------------------------------------------------------------------
// topoLOGY SOURCE OBSERVABILITY.
//   detectBigCorePool() has two data sources:
//     1) <state_dir>/big_pool written BY THE DAEMON (single source of truth), OR
//     2) a local inference from /sys cpu_capacity (emergency fallback only).
//   We must NOT change which rules win (the fallback already mirrors the daemon's
//   classifyCpuPools() exactly). Problem 38 only asks to *observe* which source
//   actually answered, so on-device field reports can distinguish:
//     - "daemon_state_file"  : watchdog consumed the daemon's classification.
//     - "local_fallback"     : the daemon state file was missing/unreadable and
//                             watchdog used its mirrored local inference.
//   The fallback path is the EMERGENCY path only (watchdog starting before the
//   daemon has run detectCpuTopology, or the daemon being dead). Different
//   failure modes (dead daemon vs. slow boot vs. permission-denied state file)
//   cannot be told apart by static code alone and MUST be validated on the real
//   target device. This string is published to
//     <state_dir>/watchdog_topology_source
//   and a WARN is emitted at watchdog start when local_fallback is in effect.
// ---------------------------------------------------------------------------
static std::string g_topologySource = "unknown";

// ROUND-011: D-state unkillable escalation state. Tracked across loop cycles so
// a daemon wedged in uninterruptible sleep (SIGKILL can't reap it) escalates to a
// visible "UnkillableDstate" condition instead of silently giving up. Reset to
// zero once the old process finally dies and a fresh daemon is launched.
static int g_unkillableStreak = 0;
static int g_staleStreak = 0;   // consecutive heartbeat-stale cycles (P0 false-positive fix)
static pid_t g_unkillablePid = -1;
static time_t g_unkillableSinceH = 0;   // CLOCK_BOOTTIME start of the wedged period

// ROUND-014 P3: independent observation of a wedged thermal sampling thread. The
// main loop may still write heartbeats while the thermal subsystem thread is
// stuck reading a node. We only RAISE a degraded state key (never restart the
// daemon — it has self-healing degradation; a restart would be a false kill).
static int g_thermalStaleStreak = 0;
static bool thermalStaleTriggered(int streak) { return streak >= 5; }

// Pure decision: a wedged D-state daemon escalates to the visible
// UnkillableDstate condition only after this many consecutive kill-failure
// cycles (3). Extracted for host unit testing.
static bool unkillableStreakTriggered(int streak) { return streak >= 3; }

// ---------------------------------------------------------------------------
// ROUND-005c T1: pure, testable core-selection from a (capacity, cpu) list.
// Emergency migration must pull critical threads OFF the hottest/strongest
// PRIME core(s) and pin them to the reliable second-highest (BIG) cluster --
// never onto the fire. This mirrors the daemon's classifyCpuPools() decision
// (aatped_topology.cpp:535-538: when the highest cluster is PRIME, g_bigPool =
// the cluster BELOW prime). The old local rule (ratio>90% of maxCap) selected
// ONLY the PRIME core on D1100-style layouts (1024 vs 800 => 78% < 90%), which
// was the divergent bug.
//
// PRIME top-cluster detection (consistent with the daemon's gap clustering):
//   - the top cluster = all cores at maxCap;
//   - it is a PRIME outlier iff it has <=2 cores AND maxCap/secondHighestCap
//     >= 1.20 (watchdog only has cpu_capacity locally; the daemon's 1.15 freq
//     branch is unavailable here).
// If a PRIME outlier exists: exclude it, then select cores whose capacity is
// >= 90% of the second-highest cluster (= the BIG cluster). Otherwise keep the
// legacy "ratio>90% of maxCap => BIG" behavior (homogeneous / normal 2-tier).
static std::vector<int> selectBigPoolFromCapacities(std::vector<std::pair<int,int>> cpuCapacities) {
    std::vector<int> pool;
    if (cpuCapacities.empty()) return pool;
    std::sort(cpuCapacities.begin(), cpuCapacities.end(),
              [](const std::pair<int,int>& a, const std::pair<int,int>& b) { return a.first > b.first; });
    int maxCap = cpuCapacities.front().first;

    // Size of the top cluster (all cores at maxCap) and the next distinct tier.
    size_t topCount = 0;
    for (const auto& cc : cpuCapacities) if (cc.first == maxCap) topCount++;
    int secondCap = 0;
    for (const auto& cc : cpuCapacities) { if (cc.first < maxCap) { secondCap = cc.first; break; } }

    bool hasPrimeTop = false;
    if (secondCap > 0 && topCount <= 2 && maxCap * 100 / secondCap >= 120) {
        hasPrimeTop = true;
    }

    if (hasPrimeTop) {
        // Exclude the PRIME outlier; pick the BIG cluster = cores within 90% of
        // the second-highest tier.
        int bigFloor = secondCap * topology::kMidMaxRatio / 100;
        for (const auto& cc : cpuCapacities) {
            if (cc.first == maxCap) continue;            // drop PRIME
            if (cc.first >= bigFloor) pool.push_back(cc.second);
        }
        if (!pool.empty()) return pool;
        // Degenerate: nothing at the second tier -- take every non-max core.
        for (const auto& cc : cpuCapacities) if (cc.first < maxCap) pool.push_back(cc.second);
        if (!pool.empty()) return pool;
    }

    // No PRIME outlier (homogeneous or ordinary 2-tier): legacy rule.
    for (const auto& cc : cpuCapacities) {
        int ratio = cc.first * 100 / maxCap;
        if (ratio > topology::kMidMaxRatio) pool.push_back(cc.second);
    }
    if (!pool.empty()) return pool;
    // All cores within 90% of max with no clear tier: keep the strongest core.
    pool.push_back(cpuCapacities.front().second);
    return pool;
}

// Detect big-core pool from cpu_capacity at runtime.
// ---------------------------------------------------------------------------
static std::vector<int> detectBigCorePool() {
    // SINGLE STATE SOURCE. The daemon is the only component
    // that classifies CPU topology (it applies the reasonableness checks, pool
    // ratios, and retry logic). It publishes the result to
    //   <state_dir>/big_pool   (comma-separated cpu list, e.g. "6,7")
    // The watchdog must CONSUME that file instead of re-inferring pools from
    // cpu_capacity (which was a duplicate, divergent source of truth). Only if
    // the file is missing/unreadable (e.g. watchdog started before the daemon
    // has run detectCpuTopology once) do we fall back to a local inference, so
    // emergency migration still has a working mask.
    {
        std::string sp = std::string(aatpe::paths::kStateDir) + "/big_pool";
        std::string s = readFile(sp.c_str());
        if (!s.empty()) {
            std::vector<int> pool;
            std::stringstream ss(s);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                try { pool.push_back(std::stoi(tok)); } catch (const std::exception&) { /* malformed token: skip */ }
            }
            if (!pool.empty()) {
                // consumed the daemon's authoritative pool.
                g_topologySource = "daemon_state_file";
                return pool;
            }
        }
    }

    // P2 Problem11: local fallback only (the daemon-written big_pool above is the
    // single source of truth). UNIFY the fallback rule with the daemon's
    // classifyCpuPools() in aatped.cpp so the two never disagree:
    //   - cpu_capacity available: a core is BIG iff its capacity ratio
    //     (cap*100/maxCap) > topology::kMidMaxRatio (90%) — identical to the
    //     daemon's strategy-1 "else -> g_bigPool" branch. (Previously this took
    //     the TOP HALF by capacity, which wrongly promoted MID cores to BIG on
    //     e.g. 4L+3M+1B layouts.)
    //   - no cpu_capacity: ascending-ID split matching daemon strategy 3
    //     (n>=7 -> last core is BIG; n<=6 -> upper half is BIG).
    //
    // we reached here ONLY because the daemon state file was
    // missing/unreadable. Mark the source. (The three return points below are all
    // part of this single local-inference path; setting the flag once here covers
    // all of them.) This fallback is emergency-only and needs on-device validation
    // of WHICH failure triggered it (slow boot / dead daemon / permissions).
    g_topologySource = "local_fallback";
    std::vector<std::pair<int, int>> cpuCapacities;  // (capacity, cpu)
    int maxCap = 0;
    // P3-15: do NOT hardcode "cpu < 32". The watchdog has no in-process g_allCpus
    // (it is a standalone binary), so the closest equivalent is the CPU list the
    // daemon publishes to <state_dir>/online_cpus (comma-separated, the watchdog's
    // single source of truth, same as big_pool). If that file is missing (watchdog
    // started before the daemon's first topology scan), enumerate
    // /sys/devices/system/cpu/cpuN directories directly. A hardcoded upper bound
    // of 32 would silently miss cores on larger-id CPUs.
    std::vector<int> cpusToProbe;
    {
        std::string oc = readFile((std::string(aatpe::paths::kStateDir) + "/online_cpus").c_str());
        if (!oc.empty()) {
            std::stringstream ss(oc);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                try { cpusToProbe.push_back(std::stoi(tok)); } catch (const std::exception&) { /* malformed token: skip */ }
            }
        }
    }
    if (cpusToProbe.empty()) {
        DIR* sysCpuDir = opendir("/sys/devices/system/cpu");
        if (sysCpuDir) {
            struct dirent* de;
            while ((de = readdir(sysCpuDir)) != nullptr) {
                if (strncmp(de->d_name, "cpu", 3) != 0) continue;
                char* end = nullptr;
                errno = 0;
                long id = strtol(de->d_name + 3, &end, 10);
                if (*end != '\0' || errno == ERANGE || id < 0 || id > 1023) continue;
                cpusToProbe.push_back((int)id);
            }
            closedir(sysCpuDir);
        }
    }
    for (int cpu : cpusToProbe) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity";
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        std::ifstream f(path);
        if (!f.good()) continue;
        int cap = 0;
        f >> cap;
        if (cap > 0) {
            cpuCapacities.push_back({cap, cpu});
            if (cap > maxCap) maxCap = cap;
        }
    }
    if (!cpuCapacities.empty() && maxCap > 0) {
        // ROUND-005c T1: delegate to the pure helper so the PRIME-outlier
        // exclusion matches the daemon's big_pool semantics.
        return selectBigPoolFromCapacities(cpuCapacities);
    }
    // No cpu_capacity available: ascending-ID fallback matching daemon strategy 3.
    int total = getCpuCount();
    std::vector<int> pool;
    if (total >= 7) {
        pool.push_back(total - 1);  // highest ID core is the prime/BIG
    } else {
        for (int i = total / 2; i < total; i++) pool.push_back(i);
    }
    return pool;
}

// ---------------------------------------------------------------------------
// Critical-thread whitelist for emergency migration.
// ---------------------------------------------------------------------------
static bool isCriticalThreadComm(const std::string& comm) {
    // RC6 (C3): whitelist matches ONLY generic render/scheduling thread signatures
    // (system render, HWUI, game-engine render threads, GPU-driver worker threads,
    // and the ".game" process marker). The previous hardcoded vendor substrings
    // (mihoyo/tencent/pubg/codm/genshin) are removed - zero vendor-name coupling.
    static const char* kCriticalComms[] = {
        "surfaceflinger", "RenderThread", "hwuiTask0", "hwuiTask1", "hwuiTask2",
        "UnityMain", "UnityGfxDevice", "mali-utility", "mali-csf", "kgsl_worker",
        ".game",
        nullptr
    };
    for (int i = 0; kCriticalComms[i]; i++) {
        if (comm.find(kCriticalComms[i]) != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Emergency migration — only whitelisted threads to big-core pool.
// Called before daemon restart to avoid 1-3s scheduling gap.
//
// Thread-level migration instead of process-level. The old code
// matched /proc/<pid>/comm and sched_setaffinity'd the whole pid (all threads).
// That over-migrated every helper thread of a matching game process. Now we
// walk /proc/<pid>/task/<tid>/ and only pin individual threads whose own comm
// hits isCriticalThreadComm(). Emergency migration is a rare event (only when
// the daemon is stuck/dead), so the extra /proc walk cost is acceptable.
// ---------------------------------------------------------------------------
static void watchdogEmergencyMigrate() {
    std::vector<int> bigPool = detectBigCorePool();
    // refresh the published topology-source metric each time we
    // actually need the pool (emergency moment), so the state file reflects the
    // source that really answered at decision time, not just the boot snapshot.
    writeState("watchdog_topology_source", g_topologySource.c_str());
    if (bigPool.empty()) return;

    cpu_set_t bigMask;
    CPU_ZERO(&bigMask);
    for (int cpu : bigPool) {
        if (cpu < CPU_SETSIZE) CPU_SET(cpu, &bigMask);
    }

    DIR* procDir = opendir("/proc");
    if (!procDir) return;
    struct dirent* entry;
    int migratedThreads = 0;
    while ((entry = readdir(procDir)) != nullptr) {
        int pid = parseIntSafe(entry->d_name);
        if (pid <= 0) continue;

        // Walk every thread of this process: /proc/<pid>/task/<tid>/
        std::string taskDirPath = "/proc/" + std::to_string(pid) + "/task";
        DIR* taskDir = opendir(taskDirPath.c_str());
        if (!taskDir) continue;
        struct dirent* taskEntry;
        while ((taskEntry = readdir(taskDir)) != nullptr) {
            int tid = parseIntSafe(taskEntry->d_name);
            if (tid <= 0) continue;
            std::string commPath = taskDirPath + "/" + std::to_string(tid) + "/comm";
            std::ifstream cf(commPath);
            if (!cf.good()) continue;
            std::string comm;
            std::getline(cf, comm);
            cf.close();
            if (!isCriticalThreadComm(comm)) continue;
            // Thread-level affinity: pin only this tid, not the whole process.
            if (sched_setaffinity(tid, sizeof(bigMask), &bigMask) == 0) {
                migratedThreads++;
            }
        }
        closedir(taskDir);
    }
    closedir(procDir);
    if (migratedThreads > 0) {
        logWatchdog("Emergency migration: pinned %d critical threads to big-core pool",
                    migratedThreads);
    }
}

// ---------------------------------------------------------------------------
// Kill daemon (SIGKILL) and wait for death.
// P1-20: Enhanced zombie reaping (real original aatpe-watchdog.cpp:193-209)
// After SIGKILL, actively waitpid to prevent zombie accumulation.
// ---------------------------------------------------------------------------

// PID-reuse-safe identity check (P1-2 hardening).
// P1-2 (加强版): the old implementation only did readlink(/proc/<pid>/exe) and a
// substring find("aatped"), which matched "aatped_fake", "my_aatped_test", etc.
// and had NO starttime check, so a recycled PID could pass. We now delegate to
// the unified aatpe::pid_identity::verifyAatpedPid() which enforces triple
// identity: kill(pid,0), exact exe realpath == "/system/bin/aatped", and
// /proc/<pid>/stat starttime matching the daemon-published record.
static bool isAatpedIdentity(int pid) {
    return aatpe::pid_identity::verifyAatpedPid((pid_t)pid);
}

// P1-3: returns true if the process actually died (kill(pid,0) != 0 after SIGKILL),
// false if it is still running (e.g. uninterruptible D-state). The caller MUST
// check the return value and NOT spawn a new instance if this returns false —
// otherwise two aatped instances run concurrently.
static bool killAndWaitDaemon(int dpid) {
    if (dpid <= 0) return true;  // nothing to kill = already dead
    // P1-2: never signal a PID unless it passes triple identity verification
    // (kill(pid,0) + exact exe realpath + starttime match).
    if (!isAatpedIdentity(dpid)) {
        logWatchdog("refusing to SIGKILL pid=%d (identity check failed; stale/reused PID or wrong exe)",
                    dpid);
        return true;  // not our process — treat as "safe to proceed"
    }
    kill(dpid, SIGKILL);
    // Wait up to 3 seconds for process to actually die (extended from 2s).
    bool dead = false;
    for (int i = 0; i < 30; i++) {
        if (kill(dpid, 0) != 0) { dead = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!dead) {
        // Still alive after 3s (likely D-state / uninterruptible disk wait).
        logWatchdog("CRITICAL: SIGKILL pid=%d still alive after 3s (D-state?). "
                    "NOT spawning new daemon to avoid double-instance.", dpid);
        return false;
    }
    // P1-20: Reap zombie — try WNOHANG first, then blocking wait with timeout.
    int status;
    pid_t reaped = waitpid(dpid, &status, WNOHANG);
    if (reaped == 0) {
        for (int i = 0; i < 10; i++) {
            reaped = waitpid(dpid, &status, WNOHANG);
            if (reaped != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    while (waitpid(-1, &status, WNOHANG) > 0) {
        // drain all zombies
    }
    return true;
}

// ---------------------------------------------------------------------------
// Restart daemon via posix_spawn.
// Returns true if the new daemon process appears alive after spawn.
// ---------------------------------------------------------------------------
// R8-010 (LED-117): long fixed sleeps must not outlive a SIGTERM/SIGINT. Sleep in
// 200ms slices, re-checking g_shouldExit, so the watchdog exits promptly on signal
// instead of sleeping out an entire cooldown/restart wait.
static void interruptibleSleepSec(long long secs) {
    long long waitedMs = 0;
    const long long totalMs = secs * 1000;
    while (waitedMs < totalMs && !g_shouldExit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        waitedMs += 200;
    }
}

static bool restartDaemon() {
    // P0 (self-sustaining spawn loop fix): NEVER blindly spawn a second aatped.
    // If the supervised PIDfile already names a healthy, identity-verified daemon,
    // one is running — spawning a duplicate just makes it lockf-abort (now _exit(0))
    // and churn control_mode. Adopt the existing healthy daemon instead of exec-ing.
    {
        int existing = readInt(aatpe::paths::kPidFile, 0);
        if (existing > 0 && isAatpedIdentity(existing)) {
            logWatchdog("supervised daemon already healthy (PID=%d) — adopting, NOT spawning duplicate",
                        existing);
            return true;
        }
    }
    // G9-016: pin a sane umask (022) so files the daemon creates inherit
    // 0644/0755 defaults rather than whatever umask the watchdog was started with.
    umask(022);
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        logWatchdog("posix_spawn_file_actions_init failed: %s", strerror(errno));
        return false;
    }

    // Issue-24: log file 0600 root-only (was 0644 world-readable).
    // v6.0.0 S-02: O_NOFOLLOW refuses a trailing symlink at kLogFile (matches the
    // canonical writeFile); O_CLOEXEC keeps the parent's logFd out of exec'd
    // children. The dup2-to-stdout/stderr below is an explicit file_action and is
    // unaffected by the CLOEXEC flag on the source fd.
    int logFd = open(aatpe::paths::kLogFile, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (logFd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, logFd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, logFd, STDERR_FILENO);
        posix_spawn_file_actions_addclose(&fa, logFd);
    }

    pid_t child = 0;
    char* spawnArgv[] = {(char*)"aatped", (char*)"--daemon", nullptr};
    int spawnRet = posix_spawn(&child, DAEMON_PATH, &fa, nullptr, spawnArgv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (logFd >= 0) close(logFd);

    if (spawnRet != 0 || child <= 0) {
        logWatchdog("posix_spawn failed: ret=%d errno=%d (%s)",
                    spawnRet, errno, strerror(errno));
        return false;
    }

    // H-07: reap the intermediate posix_spawn child. aatped --daemon double-forks:
    // the first child (this 'child') execs aatped, which forks the real daemon and
    // exits. Without waitpid(), that first child becomes a zombie. Wait up to 3s
    // (aatped daemonizes quickly after eBPF load).
    {
        int st;
        for (int i = 0; i < 30; i++) {
            pid_t w = waitpid(child, &st, WNOHANG);
            if (w == child || w < 0) break;
            if (g_shouldExit.load(std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Wait for daemon to write its PID file and become alive
    interruptibleSleepSec(watchdog::kRestartWaitSec);  // R8-010
    int newPid = readInt(aatpe::paths::kPidFile, 0);
    if (newPid > 0 && kill(newPid, 0) == 0) {
        logWatchdog("Daemon restarted successfully (PID=%d)", newPid);
        return true;
    }
    logWatchdog("Daemon restart failed: new PID not found or not alive (newPid=%d)", newPid);
    return false;
}

// ---------------------------------------------------------------------------
// Check if the daemon binary is usable (exists + executable).
// Returns: 0=usable, 1=missing, 2=not-executable.
// ---------------------------------------------------------------------------
static int checkBinaryUsable() {
    struct stat st;
    if (stat(DAEMON_PATH, &st) != 0) return 1;  // missing
    if (!(st.st_mode & S_IXUSR)) return 2;     // not executable
    return 0;
}

// ---------------------------------------------------------------------------
// Classify the death reason based on observable signals.
// Returns an enum-like string for logging and decision-making.
//   "clean_exit"     — daemon was alive for a normal duration then died
//   "rapid_crash"    — died within kRapidDeathThresholdSec of start (config/binary error)
//   "stuck"          — heartbeat stale (infinite loop / deadlock)
//   "binary_broken"  — daemon binary missing or not executable
//   "oom"            — was approaching memory limit before death
// ---------------------------------------------------------------------------
static const char* classifyDeath(time_t daemonStartedAt, bool wasHeartbeatStale,
                                 bool wasMemoryPressure, time_t now) {
    if (checkBinaryUsable() != 0) return "binary_broken";
    if (wasHeartbeatStale) return "stuck";
    if (wasMemoryPressure) return "oom";
    long long aliveSec = (long long)(now - daemonStartedAt);
    if (daemonStartedAt > 0 && aliveSec < watchdog::kRapidDeathThresholdSec)
        return "rapid_crash";
    return "clean_exit";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // P2-32: --version / -v / version prints the embedded version and exits
    // BEFORE root-only setup (/dev/aatpe, state dirs, file lock). Lets
    // `aatpe-watchdog --version` return vX.Y.Z cleanly on-device.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v" || a == "version") {
            printf("%s\n", AATPE_VERSION);
            return 0;
        }
    }

    // G9-008: fail-fast when not root. The watchdog mkdir's /dev/aatpe, writes a
    // root-only lock, and posix_spawn's /system/bin/aatped. Running it as an
    // unprivileged user previously fell through into a hot loop of failed spawns
    // log-spamming; refuse up front instead.
    if (::getuid() != 0) {
        fprintf(stderr, "aatpe-watchdog: must run as root\n");
        return 1;
    }

    // Signal handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    // P2-2: create /dev/aatpe (and the state subdir) BEFORE opening the log file.
    // Previously wdLogInit() ran here and fopen(LOG_FILE) failed because the dir
    // did not exist yet, leaving g_wdLogFp==nullptr forever. logWatchdog() then
    // took its temp-open fallback on every write, and rotateLogIfNeeded() (guarded
    // by !tempOpened) was skipped permanently -> the log grew without bound. With
    // the dir created first, wdLogInit() gets a real persistent handle and rotation
    // actually runs.
    // P3-13: run dirs are root-only on tmpfs -> 0700 (was 0755).
    if (mkdir("/dev/aatpe", 0700) != 0 && errno != EEXIST)
        logWatchdog("mkdir /dev/aatpe failed: %s", strerror(errno));
    if (mkdir(aatpe::paths::kStateDir, 0700) != 0 && errno != EEXIST)
        logWatchdog("mkdir %s failed: %s", aatpe::paths::kStateDir, strerror(errno));
    wdLogInit();

    // File lock — prevent multiple watchdog instances.
    // P2-#11: if open() fails we MUST NOT proceed unlocked (double-instance).
    // Retry briefly (tmpfs mount race after boot), then hard-exit.
    int wdLockFd = -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        // R8-017 (LED-124): lock file holds the watchdog instance lock; open it
        // 0600 root-only (was 0644, world-readable) and O_NOFOLLOW to reject a
        // symlink planted at this path.
        wdLockFd = open(WD_LOCK_FILE, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
        if (wdLockFd >= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (wdLockFd < 0) {
        logWatchdog("FATAL: cannot open lock file %s: %s — exiting to avoid double-instance",
                    WD_LOCK_FILE, strerror(errno));
        return 1;
    }
    if (lockf(wdLockFd, F_TLOCK, 0) < 0) {
        logWatchdog("Another watchdog instance is already running, exiting");
        close(wdLockFd);
        return 1;
    }

    // Write watchdog PID file (ROUND-007: atomic temp+rename so a reader never
    // sees a truncated/half-written pid; O_NOFOLLOW on the target too).
    // WD-10: check the rename() return value. A failed rename left the stale
    // .tmp behind while watchdog.pid still pointed at the OLD (dead) instance,
    // so service.sh's idempotent check would think a watchdog was already alive
    // and skip spawning a new one.
    {
        std::string pidStr = std::to_string(getpid());
        std::string tmpPid = std::string(WD_PID_FILE) + ".tmp";
        int tfd = open(tmpPid.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC, 0600);
        if (tfd >= 0) {
            ssize_t w = write(tfd, pidStr.c_str(), pidStr.size());
            bool wroteAll = (w == (ssize_t)pidStr.size());
            close(tfd);
            if (wroteAll) {
                if (rename(tmpPid.c_str(), WD_PID_FILE) != 0) {
                    logWatchdog("WARN: atomic rename %s -> %s failed: %s; "
                                "leaving tmp (will be retried next start)",
                                tmpPid.c_str(), WD_PID_FILE, strerror(errno));
                    // tmp stays; next start O_TRUNC's it. Do NOT unlink under
                    // lock (another instance may be mid-start).
                }
            } else {
                logWatchdog("WARN: short write on pid tmp (%zd/%zu); unlinking",
                            w, pidStr.size());
                unlink(tmpPid.c_str());
            }
        }
    }
    logWatchdog("AATPE Watchdog %s starting (PID=%d)", AATPE_VERSION, getpid());

    // crash_loop_detected must ALWAYS be a clean boolean ("0"/"1").
    // On startup a previous build may have left a non-numeric label (e.g. an old
    // watchdog wrote the deathReason string here). Re-seed it to "0" so the
    // daemon's readInt() never sees garbage and the on-disk contract holds. The
    // latch is flipped to "1" only below when restartCount > MAX_RESTARTS.
    writeState("crash_loop_detected", "0");

    // WD-06: when the watchdog itself is restarted (service.sh supervisor respawn,
    // module toggle, OOM), g_wdState resets to NORMAL in memory but the on-disk
    // watchdog_runtime_state / watchdog_degraded files from the previous process
    // can linger at EMERGENCY/PASSIVE or "rapid_crash_loop+thermal_thread_stale".
    // The WebUI/diag would then show a scary stale latch until the first
    // wdTransition() fires. Re-publish the in-memory initial state (NORMAL /
    // "normal") at startup so disk matches memory immediately. The
    // stale-on-disk self-heal path below still covers the case where the
    // daemon is genuinely mid-crash-loop and heartbeatFresh never becomes true.
    writeState("watchdog_runtime_state", wdStateName(g_wdState));  // NORMAL
    writeState("watchdog_state_reason", "watchdog_startup");
    clearAllDegradedReasons();   // publishes "normal" if set was non-empty

    // WD-12 / INC-013: who watches the watchdog? This C++ process cannot
    // self-reap (if it deadlocks in D-state, no internal thread can respawn it).
    // The answer lives in service.sh (the ROUND-010 T1 block, ~line 486-537):
    // a detached 10s-poll supervisor loop reads watchdog.pid, verifies exe +
    // starttime (PID-reuse guard), and respawns the watchdog with a 5-in-60s
    // rapid-crash gate. The C++ side only owns the watchdog.pid + watchdog.lock
    // contract that supervisor reads. No in-process self-ping is added because
    // (a) a self-heartbeat cannot detect a full-process deadlock and (b) the
    // script-layer supervisor is already the correct separation of concerns.

    // Config (uppercase keys matching aatpe.conf)
    int INTERVAL = readConfigInt("WATCHDOG_INTERVAL", watchdog::kDefaultIntervalSec);
    if (INTERVAL < watchdog::kMinIntervalSec) INTERVAL = watchdog::kDefaultIntervalSec;
    // G9-006: clamp the poll interval to a sane [1s,60s]. An unbounded large value
    // would leave a stuck daemon undetected for an hour; a sub-second value would
    // busy-spin. Anything out of band falls back to the default.
    if (INTERVAL > 60) {
        logWatchdog("WATCHDOG_INTERVAL=%d out of range, using default %ds",
                    INTERVAL, watchdog::kDefaultIntervalSec);
        INTERVAL = watchdog::kDefaultIntervalSec;
    }
    int MAX_RESTARTS = readConfigInt("WATCHDOG_MAX_RESTARTS", watchdog::kDefaultMaxRestarts);
    int RESTART_WINDOW = readConfigInt("WATCHDOG_RESTART_WINDOW", watchdog::kDefaultRestartWindow);
    // heartbeat timeout no longer hardcoded; overridable via WATCHDOG_HEARTBEAT_TIMEOUT
    int HEARTBEAT_TIMEOUT = readConfigInt("WATCHDOG_HEARTBEAT_TIMEOUT",
                                         watchdog::kHeartbeatTimeoutSec);
    // G9-006: a zero/negative heartbeat timeout would treat every healthy daemon
    // as stuck (age>0 always true) and SIGKILL it every cycle; an unbounded large
    // one would defeat the watchdog entirely. Clamp to [10s, 600s].
    if (HEARTBEAT_TIMEOUT < 10 || HEARTBEAT_TIMEOUT > 600) {
        logWatchdog("WATCHDOG_HEARTBEAT_TIMEOUT=%d out of range, using default %ds",
                    HEARTBEAT_TIMEOUT, watchdog::kHeartbeatTimeoutSec);
        HEARTBEAT_TIMEOUT = watchdog::kHeartbeatTimeoutSec;
    }

    // Restart rate limiting
    int restartCount = 0;
    time_t lastRestart = 0;
    // P0: hard-crash-only counter drives the crash_loop PASSIVE latch. Soft
    // restarts (watchdog stuck-kill, clean SIGTERM, service.sh/upgrade) reset it
    // and never latch by themselves.
    int hardCrashCount = 0;
    time_t lastHardCrash = 0;
    bool passiveMode = false;
    // W-005: count consecutive PASSIVE entries caused by crash-loops. The cooldown
    // doubles each cycle (300s -> 600s -> 1200s, capped at 3600s). After 3 cycles the
    // watchdog stays latched in PASSIVE and only a manual unlock_watchdog clears it
    // (read via the watchdog_passive_locked=0 state file the daemon writes). Reset
    // to 0 once the daemon has run healthy for > 10 minutes.
    int crashLoopCycles = 0;
    // passive cooldown — record when we entered passive so we can
    // give up after a cooldown period and attempt recovery instead of being
    // stuck passive forever.
    time_t passiveSince = 0;
    // consecutive-sample counters to avoid single-sample jitter
    // triggering real actions.
    int consecutiveMemoryWarns = 0;
    int consecutiveMigWarns    = 0;
    int consecutiveLoadWarns  = 0;
    // temporary passive (mig-rate / load) auto-recovery timestamp
    time_t tempPassiveUntil = 0;
    // error classification state
    time_t daemonStartedAt = 0;       // when we last successfully launched a daemon
    int rapidCrashCount = 0;           // consecutive deaths within kRapidDeathThresholdSec
    bool degradedMode = false;         // rapid crash loop detected — back off longer
    time_t degradedSince = 0;
    // Hotfix v4.1.1: CLOCK_BOOTTIME timestamp when the daemon first stayed alive
    // AND heartbeated fresh while we were latched in DEGRADED; 0 while we are not
    // accumulating a self-heal streak. Any daemon death or stale heartbeat resets it.
    time_t degradedHealthySince = 0;
    bool lastMemWasOver = false;       // track memory pressure leading to death
    bool stopLogged = false;           // G9-003: one-time log latch per explicit stop
    // G9-001: CLOCK_BOOTTIME timestamp when thermal first cleared while we were in
    // EMERGENCY; 0 while thermal is still active or not in EMERGENCY. Leaving
    // EMERGENCY requires thermal to stay clear continuously for this long.
    time_t emergClearSince = 0;
    const int emergencyClearSec = thermal_runtime::kMinRecoverySec;  // 30s hysteresis
    // ROUND-003: independent thermal backup authority. Watchdog reads the sensors
    // itself while in EMERGENCY. A configurable wall (default 90C, conservative)
    // with a 5C margin: only when the independently-read max temp has been SAFE
    // for a full emergencyClearSec does the watchdog trust "it has cooled" even if
    // the daemon's thermal_emergency file is still stuck at 1.
    int indThermalWallC = readConfigInt("WATCHDOG_INDEP_THERMAL_WALL", 90);
    if (indThermalWallC < 40 || indThermalWallC > 150) {
        logWatchdog("WATCHDOG_INDEP_THERMAL_WALL=%d out of range, using 90C", indThermalWallC);
        indThermalWallC = 90;
    }
    const int indThermalMarginC = 5;
    int emergIndSafeStreak = 0;
    // D-006: sample count must CEILING-divide emergencyClearSec by INTERVAL so the
    // wall-clock covered by emergIndNeedSamples consecutive safe samples is
    // guaranteed >= emergencyClearSec. The old floor division
    //   emergencyClearSec / INTERVAL
    // under-counts whenever emergencyClearSec is not an exact multiple of INTERVAL
    // (e.g. clear=30s, interval=20s -> 30/20=1 samples, but a single 20s sample
    // only proves ~20s of safety < 30s). Ceiling: (clear + interval - 1)/interval
    // -> ceil(30/20)=2 samples = ~40s >= 30s. The >=1 floor keeps the existing
    // "at least one SAFE read" guarantee for interval >= clear.
    const int emergIndNeedSamples =
        (emergencyClearSec + INTERVAL - 1) / INTERVAL < 1
            ? 1
            : (emergencyClearSec + INTERVAL - 1) / INTERVAL;

    logWatchdog("Config: interval=%ds max_restarts=%d window=%ds hb_timeout=%ds cooldown=%ds",
                INTERVAL, MAX_RESTARTS, RESTART_WINDOW,
                HEARTBEAT_TIMEOUT, watchdog::kPassiveCooldownSec);

    // topology-source observability.
    // Warm up detectBigCorePool() ONCE at watchdog start so g_topologySource is
    // populated before we publish it. Note this is a *startup snapshot*: the
    // daemon may not have written big_pool yet (service.sh waits for daemon.pid
    // but NOT for its first detectCpuTopology), so a "local_fallback" here can
    // mean "daemon not ready yet" rather than "daemon is broken". The emergency
    // migration path re-runs detectBigCorePool() each time, so g_topologySource
    // is also refreshed at the moment it actually matters.
    {
        std::vector<int> warm = detectBigCorePool();
        (void)warm;  // only side-effect we want is g_topologySource
        writeState("watchdog_topology_source", g_topologySource.c_str());
        if (g_topologySource == "local_fallback") {
            // explicit WARN when we did NOT get the daemon's
            // authoritative classification. This is the signal field engineers
            // read during on-device bring-up; it does NOT change behaviour.
            logWatchdog("WARN: watchdog using LOCAL topology fallback (daemon state file "
                        "'big_pool' unavailable at start). Pools inferred locally and mirrored "
                        "to daemon rules; on-device validation recommended. "
                        "Published state: watchdog_topology_source=local_fallback");
        } else {
            logWatchdog("Topology source: %s (big_pool consumed from daemon state file)",
                        g_topologySource.c_str());
        }
    }

    while (!g_shouldExit.load(std::memory_order_relaxed)) {
        interruptibleSleepSec(INTERVAL);  // R8-010

        // Issue-23: drain any zombie children each iteration. The watchdog spawns
        // the daemon via posix_spawn; the intermediate child exits after double-fork.
        // killAndWaitDaemon() drains only on the kill path; this catch-all ensures
        // natural-death restarts also reap accumulated zombies.
        // D2 storm fix (v4.3.1h5): ONLY the supervised daemon pid (the one named in
        // daemon.pid) counts as a real death. Launchers / double-fork intermediates /
        // duplicate `aatped --daemon` copies that lose lockf and _exit(0) are NOISE:
        // reap them but do NOT let them drive restart counting, stale-streak resets,
        // or emergency pin migration.
        {
            int st;
            int supervisedPid = readInt(aatpe::paths::kPidFile, 0);
            int w;
            while ((w = waitpid(-1, &st, WNOHANG)) > 0) {
                if (w != supervisedPid) {
                    logWatchdog("ignoring exit of non-supervised child pid=%d (launcher/duplicate)", w);
                }
                // supervised death is handled by the daemonAlive/heartbeat path below;
                // here we only reap and never act on the noise children.
            }
        }

        // G9-003: explicit user stop (`aatpe-cli daemon stop`) drops the
        // stop_request flag. While it exists the watchdog MUST NOT revive the
        // daemon — a deliberate stop looks identical to a crash to the liveness
        // check, so the flag is the only thing that distinguishes them. We freeze
        // all recovery/restart/state-promotion work until the flag is cleared by
        // `daemon start`/`restart` or by service.sh at next boot.
        if (explicitStopRequested()) {
            if (!stopLogged) {
                logWatchdog("Explicit stop requested (%s present): not restarting daemon until cleared",
                            STOP_REQUEST_FILE);
                stopLogged = true;
            }
            continue;
        } else {
            stopLogged = false;
        }

        // Hotfix v4.1.1: boot grace. During the first kBootGraceSec after boot the
        // device is still in its boot storm (observed loadavg=41) and service.sh +
        // the watchdog can race-launch aatped while daemon.pid is not yet written.
        // That benign double-start flock-collision exits < kRapidDeathThresholdSec;
        // we must NOT read it as a broken-binary crash loop, and we must give the
        // warming-up daemon a wider heartbeat-stale margin.
        bool inBootGrace = watchdog::inBootGrace(bootTimeSec(), watchdog::kBootGraceSec);
        int hbTimeoutLimit = watchdog::effectiveHeartbeatTimeout(HEARTBEAT_TIMEOUT, inBootGrace);

        int dpid = readInt(aatpe::paths::kPidFile, 0);
        // P1-2 (hardened): liveness requires triple identity verification
        // (kill(dpid,0) + exact exe realpath + starttime match) via isAatpedIdentity()
        // -> aatpe::pid_identity::verifyAatpedPid(). This rejects a reused PID.
        bool daemonAlive = (dpid > 0 && isAatpedIdentity(dpid));

        // Heartbeat check — alive but stale heartbeat = stuck
        // STARTUP TIMING (documented; needs on-device confirm).
        // Boot sequence: post-fs-data.sh -> service.sh -> aatped --daemon
        //   (writes daemon.pid once up) -> eBPF load -> watchdog start.
        // service.sh waits up to 5s for daemon.pid to appear BEFORE launching the
        // watchdog, so when watchdog enters this loop the daemon is already alive.
        // We must NOT misjudge a still-initializing daemon as "stuck": the daemon
        // only starts writing the heartbeat file AFTER its eBPF load / first
        // decision cycle completes. The `if (hb > 0)` guard below is what makes
        // this safe: while hb==0 (no heartbeat written yet) we simply do NOT
        // run the staleness test — i.e. the heartbeat clock only starts counting
        // from the daemon's first reported heartbeat, not from watchdog start.
        bool heartbeatStale = false;
        int hb = 0;
        if (daemonAlive) {
            hb = readInt((std::string(aatpe::paths::kStateDir) + "/heartbeat").c_str(), 0);
            if (hb > 0) {
                // W-004: compare against CLOCK_BOOTTIME (same clock the daemon
                // stamps), NOT wall clock. hb is a CLOCK_BOOTTIME second count.
                time_t nowHb = bootTimeSec();
                time_t age = nowHb - (time_t)hb;
                // use configurable HEARTBEAT_TIMEOUT, widened by kBootGrace...
                // during the post-boot grace so a warm-up under the boot storm is
                // not misjudged as stuck (hotfix v4.1.1).
                if (age > hbTimeoutLimit) {
                    g_staleStreak++;
                    // P0: require N consecutive stale cycles before SIGKILL. A single
                    // stale reading (bpftool exec / /proc scan / boot storm) must not
                    // kill a slow-but-alive daemon.
                    if (g_staleStreak < kStaleStrikeBeforeKill) {
                        logWatchdog("heartbeat stale (%lds old, limit=%ds) streak=%d/%d — deferring SIGKILL",
                                    (long)age, hbTimeoutLimit, g_staleStreak, kStaleStrikeBeforeKill);
                        heartbeatStale = false;  // not yet a kill this cycle
                    } else {
                        heartbeatStale = true;
                        logWatchdog("DAEMON STUCK: alive (PID=%d) but heartbeat stale (%lds old, limit=%ds%s, streak=%d). Killing for restart.",
                                    dpid, (long)age, hbTimeoutLimit,
                                    inBootGrace ? " [boot-grace widened]" : "", g_staleStreak);
                    bool killed = killAndWaitDaemon(dpid);
                    if (!killed) {
                        // P1-3: old daemon still alive — do NOT start a new instance.
                        logWatchdog("Old daemon pid=%d survived SIGKILL, skipping restart this cycle",
                                    dpid);
                        // ROUND-011: D-state unkillable escalation. After >=3
                        // consecutive cycles the process is wedged in D-state;
                        // publish a visible condition and re-try a process-group
                        // SIGKILL every cycle instead of silently giving up.
                        if (g_unkillablePid != dpid) {
                            g_unkillablePid = dpid;
                            g_unkillableStreak = 0;
                            g_unkillableSinceH = bootTimeSec();
                        }
                        g_unkillableStreak++;
                        long waited = (long)(bootTimeSec() - g_unkillableSinceH);
                        if (unkillableStreakTriggered(g_unkillableStreak)) {
                            // WD-01: the D-state condition lives in the dedicated
                            // watchdog_unkillable key (set/cleared below), NOT in
                            // watchdog_runtime_state. Writing a non-enum token there
                            // desynced the on-disk state from g_wdState and was never
                            // reset. watchdog_unkillable=0 on recovery already restores
                            // the enum state file.
                            writeState("watchdog_unkillable", "1");
                            // Process-group SIGKILL, but NEVER our own group:
                            // the watchdog itself (and the superv shell) must
                            // not be killed. Daemon setsid()s into its own group.
                            pid_t dpgid = getpgid(dpid);
                            pid_t mypgid = getpgid(0);
                            if (dpgid > 0 && dpgid != mypgid && dpgid != getpid()) {
                                logWatchdog("UnkillableDstate: cycle %d, group SIGKILL pgid=%d (pid=%d, waited=%lds, hb age=%lds)",
                                            g_unkillableStreak, (int)dpgid, dpid, waited, (long)age);
                                kill(-dpgid, SIGKILL);
                            } else {
                                logWatchdog("UnkillableDstate: cycle %d, pid=%d waited=%lds (group-skip: pgid=%d mine=%d)",
                                            g_unkillableStreak, dpid, waited, (int)dpgid, (int)mypgid);
                            }
                        }
                        heartbeatStale = false;  // don't enter restart path
                        continue;
                    }
                    // old daemon finally died -> clear unkillable condition
                    if (g_unkillableStreak > 0) {
                        writeState("watchdog_unkillable", "0");
                        g_unkillableStreak = 0;
                        g_unkillablePid = -1;
                    }
                    unlink(aatpe::paths::kPidFile);
                    } // end else: streak reached -> killed
                }
            }
        }
        // Hotfix v4.1.1: heartbeatFresh = alive, has written at least one heartbeat
        // (hb>0), and that heartbeat is not stale. This is the "healthy enough to
        // adopt / to count toward the degraded self-heal streak" predicate.
        bool heartbeatFresh = (daemonAlive && hb > 0 && !heartbeatStale);
        // D2 storm fix (v4.3.1h5): ONLY clear the stale deferral streak when the
        // heartbeat is GENUINELY fresh (age <= timeout). Previously this ran on
        // `heartbeatFresh`, which was also true during the "deferring" cycles
        // (age>limit but streak<kill) — so g_staleStreak reset to 0 every cycle
        // and a frozen-heartbeat daemon was NEVER SIGKILLed. Compute real freshness
        // directly from age, not from the deferring predicate.
        bool heartbeatReallyFresh = daemonAlive && hb > 0 &&
            (bootTimeSec() - (time_t)hb) <= hbTimeoutLimit;
        if (heartbeatReallyFresh) g_staleStreak = 0;

        // P2-5: stale-restart telemetry self-clears. restart_count /
        // watchdog_restart_reason are point-in-time; after the daemon has stayed
        // healthy (heartbeat-fresh) for a stable window they must not linger at a
        // scary historical value (e.g. 1678 / "rapid_crash class=soft") that the
        // user misreads as current instability. We reset the telemetry (NOT the
        // hard-crash latch, which is governed by crash_loop_detected + hard unlock
        // and is untouched) once health has been continuous for the recovery window.
        if (heartbeatFresh && restartCount > 0) {
            time_t nowHb = bootTimeSec();
            long long sinceRestart = (lastRestart > 0) ? (long long)(nowHb - lastRestart) : 0;
            // P3: only self-clear when genuinely stable AND not latched. When the
            // watchdog is in PASSIVE (hard crash-loop latch, possibly crashLoopCycles>=3
            // awaiting manual unlock) the loop `continue`s before reaching here on the
            // death path, but double-guard defensively so we never zero the latch
            // accounting while a human lock is active.
            bool latched = passiveMode || (crashLoopCycles >= 3);
            if (sinceRestart >= (long long)watchdog::kRecoveryStableSec * 4 && !latched) {
                logWatchdog("Healthy for %llds — clearing stale restart telemetry "
                            "(restart_count was %d, hard_crash_count was %d); hard-crash "
                            "latch untouched",
                            sinceRestart, restartCount, hardCrashCount);
                // Reset BOTH in-memory counters AND the state files so they stay
                // consistent; the next real hard crash then accumulates cleanly from 0.
                restartCount = 0;
                hardCrashCount = 0;
                lastHardCrash = 0;
                writeState("restart_count", "0");
                writeState("hard_crash_count", "0");
                writeState("watchdog_restart_reason", "normal");
            }
        }

        // ROUND-014 P3: independent thermal-thread-stale observation. Only when the
        // main loop heartbeat is healthy (daemon not otherwise stuck) AND the daemon
        // self-reports thermal_freshness=stale for >=5 cycles do we raise a
        // degraded state key. We NEVER restart the daemon for this — it has its own
        // self-healing degradation, and killing a healthy-main-loop daemon would be
        // a false positive.
        if (daemonAlive && !heartbeatStale) {
            std::string tf = readFile((std::string(aatpe::paths::kStateDir) + "/thermal_freshness").c_str());
            // audit4_state F1: the daemon writes "stale=<age>" (e.g. "stale=15"), not
            // the bare tokens "stale"/"stale_read" this previously compared against,
            // so the independent second-eyes observation never fired. Match by the
            // "stale" PREFIX (covers "stale=N", "stale" and "stale_read"); "fresh"
            // and any other value reset the streak.
            if (tf.rfind("stale", 0) == 0) {
                g_thermalStaleStreak++;
                if (thermalStaleTriggered(g_thermalStaleStreak)) {
                    // WD-02/WD-05: addReason only publishes on the rising edge; it does
                    // not clobber a coexisting rapid_crash_loop/binary_broken reason and
                    // does not rewrite the file every tick.
                    addDegradedReason("thermal_thread_stale");
                }
            } else {
                if (g_thermalStaleStreak > 0) {
                    removeDegradedReason("thermal_thread_stale");
                }
                g_thermalStaleStreak = 0;
            }
        }

        // Unified restart path: dead daemon, missing PID, OR stuck (heartbeat stale)
        if (!daemonAlive || heartbeatStale) {
            // Hotfix v4.1.1: any death / stale heartbeat breaks the DEGRADED
            // self-heal streak — a transient healthy window is over.
            degradedHealthySince = 0;
            // classify the failure before deciding strategy
            // R8-005 (LED-112): measure "how long the daemon lived" on CLOCK_BOOTTIME,
            // the SAME monotonic-suspend-immune clock the heartbeat uses (bootTimeSec()).
            // time(nullptr) wall clock pauses across suspend and can jump on NTP, which
            // would misclassify a long-lived daemon's post-suspend death as rapid_crash.
            time_t nowC = bootTimeSec();
            const char* deathReason = classifyDeath(daemonStartedAt, heartbeatStale,
                                                     lastMemWasOver, nowC);
            logWatchdog("Daemon death classified as: %s", deathReason);
            // P1 (boot double-start false-latch fix): a sub-10s death that left NO
            // fatal crash_signal is a BENIGN soft restart (service.sh/watchdog
            // flock race, or the duplicate-instance _exit(0)). It must NOT latch
            // DEGRADED/rapid_crash_loop. Only a real fatal-signal crash may escalate.
            std::string deathSig;
            bool thisDeathIsHard = readHardCrashSignal(deathSig);

            // Strategy: binary_broken → do NOT restart (no point). Log and wait.
            if (std::string(deathReason) == "binary_broken") {
                int br = checkBinaryUsable();
                logWatchdog("BINARY BROKEN (check=%d): %s — skipping restart until file restored",
                            br, DAEMON_PATH);
                addDegradedReason("binary_broken");
                interruptibleSleepSec(double(INTERVAL) * 5.0);  // R8-010
                continue;
            }
            // Reached here only when the death reason is NOT binary_broken: the binary
            // is usable again, so drop any stale binary_broken reason (WD-02).
            removeDegradedReason("binary_broken");

            // Strategy: rapid_crash → count toward degraded escalation.
            // Hotfix v4.1.1: during the post-boot grace a sub-threshold death is the
            // expected service.sh/watchdog double-start flock race under the boot
            // storm — observe it but NEITHER count it NOR escalate to DEGRADED.
            if (std::string(deathReason) == "rapid_crash" && thisDeathIsHard) {
                if (watchdog::countRapidCrash(inBootGrace)) {
                    rapidCrashCount++;
                    logWatchdog("Rapid crash %d/%d (daemon died < %ds after start)",
                                rapidCrashCount, watchdog::kMaxRapidCrashesBeforeDegrade,
                                watchdog::kRapidDeathThresholdSec);
                    if (watchdog::shouldEscalateDegraded(inBootGrace, rapidCrashCount,
                                                         watchdog::kMaxRapidCrashesBeforeDegrade,
                                                         degradedMode)) {
                        logWatchdog("ESCALATING to DEGRADED mode: %d rapid crashes in a row. "
                                    "Likely config error or binary corruption. Cooldown %ds.",
                                    rapidCrashCount, watchdog::kDegradedCooldownSec);
                        degradedMode = true;
                        degradedSince = nowC;
                        wdTransition(WdRuntimeState::DEGRADED, "rapid_crash_loop");
                        addDegradedReason("rapid_crash_loop");
                    }
                } else {
                    logWatchdog("Rapid death (died < %ds after start) DURING boot grace "
                                "(bootNow=%lds/%ds): not counting, not escalating — almost "
                                "certainly the service.sh+watchdog double-start flock race "
                                "under the boot storm, not a broken binary.",
                                watchdog::kRapidDeathThresholdSec,
                                (long)nowC, watchdog::kBootGraceSec);
                }
            } else if (std::string(deathReason) == "rapid_crash" && !thisDeathIsHard) {
                // P1: sub-10s death with NO fatal signal = benign soft restart
                // (double-start flock / duplicate _exit(0)). Reset the streak and
                // do NOT latch DEGRADED. The health-based self-heal below clears
                // any prior on-disk latch once the daemon stays healthy.
                rapidCrashCount = 0;
                logWatchdog("Soft rapid death (signal=%s, class=soft) — benign, resetting rapid streak, NOT degrading",
                            deathSig.c_str());
            } else {
                // Normal death (not rapid) — reset rapid crash counter
                rapidCrashCount = 0;
            }

            // Strategy: degraded mode → wait out the cooldown before retry
            if (degradedMode) {
                if ((nowC - degradedSince) < watchdog::kDegradedCooldownSec) {
                    logWatchdog("Degraded mode: waiting %lds/%ds before retry",
                                (long)(nowC - degradedSince), watchdog::kDegradedCooldownSec);
                    continue;
                }
                logWatchdog("Degraded cooldown elapsed (%lds), attempting recovery restart",
                            (long)(nowC - degradedSince));
                degradedMode = false;
                degradedSince = 0;
                rapidCrashCount = 0;
                hardCrashCount = 0;
                removeDegradedReason("rapid_crash_loop");
                // G9-002: previously the state stayed DEGRADED forever after this
                // retry (control_mode latched to assist). Step up to RECOVERY on the
                // recovery restart so the existing recovery-stable hysteresis can
                // carry us back to NORMAL once the daemon stays healthy.
                wdTransition(WdRuntimeState::RECOVERY, "degraded_cooldown_retry");
                // fall through to restart
            }

            // D2 storm fix (v4.3.1h5): emergency pin migration moved OUT of this
            // unconditional pre-restart path into the restartDaemon() success block
            // below. It must run ONLY when a real supervised daemon restart actually
            // launches — never on a false-death / launcher cycle (which was pinning
            // 109~131 threads to the big core every ~4s, overheating the device).

            // Rate limiting. G9-007: use CLOCK_BOOTTIME (monotonic across suspend)
            // here too, matching daemonStartedAt/heartbeat. Wall clock pauses across
            // suspend and could otherwise burst-count restarts on resume.
            time_t nowR = bootTimeSec();

            // P0 (crash_loop false-positive fix): classify this death BEFORE counting.
            // Only a real fatal-signal crash (crash_signal = SIGABRT/SEGV/BUS/ILL/FPE)
            // advances the hard-crash latch. The watchdog's own heartbeat-stale SIGKILL
            // (deathReason=="stuck") and clean exits are soft restarts: they bump the
            // telemetry restartCount but reset hardCrashCount and NEVER latch PASSIVE.
            std::string crashSig;
            bool isStuckKill = (std::string(deathReason) == "stuck");
            bool hardCrash = !isStuckKill && readHardCrashSignal(crashSig);
            writeState("watchdog_restart_reason",
                       (std::string(deathReason) + " class=" +
                        (hardCrash ? "hard_crash" : "soft_restart") +
                        " signal=" + (crashSig.empty() ? "-" : crashSig)).c_str());
            clearCrashSignal();  // consume evidence; don't poison the next cycle

            if (lastRestart > 0 && (nowR - lastRestart) < RESTART_WINDOW)
                restartCount++;
            else
                restartCount = 1;
            lastRestart = nowR;

            if (hardCrash) {
                if (lastHardCrash > 0 && (nowR - lastHardCrash) < RESTART_WINDOW)
                    hardCrashCount++;
                else
                    hardCrashCount = 1;
                lastHardCrash = nowR;
            } else {
                hardCrashCount = 0;  // benign restart breaks the hard-crash streak
            }

            // Crash Loop Protector telemetry. The daemon reads
            // crash_loop_detected to latch its own safe-mode (monitor-only), and
            // restart_count lets the WebUI show the recent restart frequency.
            writeState("restart_count", std::to_string(restartCount).c_str());
            writeState("hard_crash_count", std::to_string(hardCrashCount).c_str());

            // P0: the PASSIVE crash_loop latch is gated on HARD crashes only.
            if (hardCrashCount > MAX_RESTARTS) {
                if (!passiveMode) {
                    crashLoopCycles++;
                    logWatchdog("FATAL: Too many HARD crashes (%d > %d). Entering passive mode (crash-loop cycle %d).",
                                hardCrashCount, MAX_RESTARTS, crashLoopCycles);
                    wdTransition(WdRuntimeState::PASSIVE, "crash_loop_detected");
                    writeState("crash_loop_detected", "1");
                    passiveMode = true;
                    // stamp the time we entered passive
                    passiveSince = nowR;
                }
                // W-005: exponential backoff. Cycle 1 = kPassiveCooldownSec (300s),
                // cycle 2 = 600s, cycle 3 = 1200s, then 2400s, capped at 3600s.
                // P3-14: the shift count must be bounded. crashLoopCycles is bounded
                // in practice (>=3 latches into manual-unlock), but a defensive cap
                // keeps `1L << (n-1)` from shifting by >=64 (undefined behavior) if
                // the counter ever escapes. 63 is the max sane shift for long.
                int shiftCycles = crashLoopCycles;
                if (shiftCycles < 1) shiftCycles = 1;
                if (shiftCycles > 63) shiftCycles = 63;
                long passiveCooldown =
                    (long)watchdog::kPassiveCooldownSec * (1L << (shiftCycles - 1));
                if (passiveCooldown > 3600L) passiveCooldown = 3600L;
                // After 3 crash-loop cycles, do NOT auto-recover on cooldown. Stay
                // latched in PASSIVE until a human sends unlock_watchdog, which the
                // daemon records by clearing STATE_DIR/watchdog_passive_locked. We
                // poll that file to observe the manual unlock.
                bool manualUnlocked = false;
                if (crashLoopCycles >= 3) {
                    std::string v = readFile(
                        (std::string(aatpe::paths::kStateDir) + "/watchdog_passive_locked").c_str());
                    if (v == "0") manualUnlocked = true;
                }
                bool exitPassive = false;
                if (passiveMode) {
                    if (crashLoopCycles >= 3 && !manualUnlocked) {
                        // Stay latched; wait for manual unlock. Periodically log so
                        // the operator knows the watchdog is intentionally holding.
                        if ((nowR - passiveSince) >= passiveCooldown) {
                            logWatchdog("crash-loop latched after %d cycles; staying PASSIVE, "
                                        "waiting for manual unlock_watchdog (watchdog_passive_locked=0)",
                                        crashLoopCycles);
                            // advance passiveSince so we only log once per cooldown window
                            passiveSince = nowR;
                        }
                        continue;
                    }
                    if (manualUnlocked && crashLoopCycles >= 3) {
                        logWatchdog("manual unlock_watchdog observed after %d crash-loop cycles; leaving PASSIVE",
                                    crashLoopCycles);
                        exitPassive = true;
                    } else if ((nowR - passiveSince) >= passiveCooldown) {
                        logWatchdog("passive cooldown elapsed (%lds >= %lds), entering RECOVERY",
                                    (long)(nowR - passiveSince), passiveCooldown);
                        exitPassive = true;
                    }
                }
                if (exitPassive) {
                    restartCount = 0;
                    hardCrashCount = 0;
                    lastHardCrash = 0;
                    // F-06: the manual-unlock path (crashLoopCycles>=3) is the ONLY
                    // place the latched cycle counter is explicitly cleared now that
                    // the 10-min healthy reset refuses to touch it. A future genuine
                    // crash then accumulates cleanly from 0.
                    crashLoopCycles = 0;
                    passiveMode = false;
                    passiveSince = 0;
                    writeState("crash_loop_detected", "0");
                    writeState("restart_count", "0");
                    // Leaving the crash-loop latch: drop any degraded reasons that were
                    // latched alongside it (rapid_crash_loop, etc.). WD-02.
                    clearAllDegradedReasons();
                    // P3-55 hysteresis: do NOT jump straight back to active after
                    // the cooldown. Enter RECOVERY (control_mode=assist) and only
                    // promote to NORMAL once health stays clean for kRecoveryStableSec.
                    wdTransition(WdRuntimeState::RECOVERY, "crash_loop_cooldown_elapsed");
                    // fall through into normal restart attempt below
                } else {
                    continue;
                }
            }

            if (!daemonAlive && dpid <= 0)
                logWatchdog("Daemon PID missing/invalid (dpid=%d). Starting.", dpid);
            else if (!daemonAlive)
                logWatchdog("Daemon dead (was PID=%d). Restarting.", dpid);

            logWatchdog("Restart attempt %d/%d", restartCount, MAX_RESTARTS);

            if (restartDaemon()) {
                passiveMode = false;
                daemonStartedAt = bootTimeSec();  // R8-005: CLOCK_BOOTTIME, same as heartbeat
                lastMemWasOver = false;           // reset memory tracking
                // D2 storm fix: pin migration ONLY on a real supervised restart.
                watchdogEmergencyMigrate();
            }
            continue;
        }

        // ---- Hotfix v4.1.1 / v4.1.2-hotfix: DEGRADED self-heal WITHOUT killing the daemon -------
        // Real-device bug: once latched DEGRADED, degradedMode could ONLY be cleared
        // in the death path, which requires the daemon to actually die. When the
        // daemon was in fact healthy (the latch was a boot double-start false
        // positive — PID alive 200s+, heartbeat fresh, crash_loop_detected=0),
        // nothing ever drove us out, so control_mode stayed "assist" forever.
        // v4.1.2-hotfix: a SECOND leak is that this self-heal was gated ONLY on the
        // in-memory `degradedMode`. If THIS watchdog child itself was restarted
        // (service.sh/supervisor), degradedMode resets to false on startup while the
        // on-disk watchdog_degraded label keeps "rapid_crash_loop" — so the WebUI
        // shows a permanent red rapid_crash_loop even though the daemon has been
        // healthy for hours (observed: crash_loop=0, main state NORMAL, wd still
        // DEGRADED for 1h37m). We now ALSO treat a stale on-disk rapid_crash_loop
        // latch as a reason to run the same grace-window self-heal. A genuine live
        // crash loop keeps heartbeatFresh false / re-escalates, so it is unaffected.
        // WD-02: the on-disk value is now an aggregated '+' joined set (e.g.
        // "rapid_crash_loop+thermal_thread_stale"). Detect a stale rapid_crash_loop
        // latch by substring containment rather than exact equality, so a coexisting
        // thermal_thread_stale reason does not mask it, and cross-watchdog-restart
        // detection still works (g_degradedSet is empty after a restart).
        bool wdDegradedOnDisk =
            (readFile((std::string(aatpe::paths::kStateDir) + "/watchdog_degraded").c_str())
             .find("rapid_crash_loop") != std::string::npos);
        if (degradedMode || wdDegradedOnDisk) {
            if (daemonAlive && heartbeatFresh) {
                time_t nowDh = bootTimeSec();
                if (degradedHealthySince == 0) degradedHealthySince = nowDh;
                long long healthyStreak = (long long)(nowDh - degradedHealthySince);
                if (watchdog::degradedSelfHeal(true, healthyStreak,
                                              watchdog::kRecoveryStableSec)) {
                    logWatchdog("DEGRADED self-heal: daemon alive + heartbeat-fresh %llds "
                                ">= %lds while latched (%s) — clearing degraded/rapid latch WITHOUT "
                                "killing, stepping to RECOVERY (control_mode assist -> active "
                                "after the recovery-stable window)",
                                healthyStreak, watchdog::kRecoveryStableSec,
                                degradedMode ? "in-memory" : "stale on-disk");
                    degradedMode = false;
                    degradedSince = 0;
                    rapidCrashCount = 0;
                    hardCrashCount = 0;
                    lastHardCrash = 0;
                    degradedHealthySince = 0;
                    removeDegradedReason("rapid_crash_loop");
                    writeState("healthy_since", "");
                    // Reuse the existing choke point (P3-53) so the control-pipe
                    // notification + state-file publish stay consistent; wdTransition()
                    // stamps g_recoverySince, and wdSteadyTransition() below promotes
                    // RECOVERY -> NORMAL once health stays clean for kRecoveryStableSec.
                    wdTransition(WdRuntimeState::RECOVERY, "degraded_healthy_self_heal");
                }
            }
        }

        // ---- Hotfix v4.1.1: adopt a live, heartbeat-fresh daemon we did not spawn --
        // If the pidfile already points to a live, heartbeat-FRESH aatped that this
        // watchdog process did not start this uptime (service.sh launched it before
        // the watchdog, or the watchdog restarted and found it already up), record
        // it as our daemon instead of posix_spawn-ing a second instance. On the boot
        // double-start race a second instance flock-collides and exits <10s, which we
        // would then misread as a rapid crash loop. We adopt its REAL start time from
        // /proc so a later death is classified against its true age, not our spawn.
        if (watchdog::shouldAdoptExistingDaemon(daemonAlive, heartbeatFresh, daemonStartedAt > 0)) {
            long long ticks = aatpe::pid_identity::readPidStarttime(dpid);
            long hz = sysconf(_SC_CLK_TCK);
            time_t adoptedAt = (ticks > 0 && hz > 0) ? (time_t)(ticks / hz) : bootTimeSec();
            logWatchdog("ADOPT: pidfile PID=%d is live + heartbeat-fresh and we did not start "
                        "it (bootNow=%lds) — adopting as our daemon (daemonStartedAt=%lds), "
                        "NOT spawning a second instance",
                        dpid, (long)bootTimeSec(), (long)adoptedAt);
            daemonStartedAt = adoptedAt;
        }

        // daemon is alive and healthy — reset rapid crash counter
        if (daemonStartedAt > 0 && !degradedMode) {
            long long aliveSec = (long long)(bootTimeSec() - daemonStartedAt);  // R8-005
            if (aliveSec > watchdog::kRapidDeathThresholdSec && rapidCrashCount > 0) {
                logWatchdog("Daemon stable for %llds — resetting rapid crash counter", aliveSec);
                rapidCrashCount = 0;
                removeDegradedReason("rapid_crash_loop");
            }
            // W-005: after a sustained healthy run (>10 min) the crash-loop backoff
            // counter resets, so the next genuine fault starts back at the 300s
            // cooldown rather than continuing from an elevated cycle count.
            // F-06: the auto reset must NEVER bypass the manual-unlock latch. When
            // crashLoopCycles >= 3 the watchdog is latched in PASSIVE and only a
            // manual unlock_watchdog (watchdog_passive_locked=0) may leave it. If the
            // daemon happens to stay alive while latched, this healthy-reset branch
            // used to run (it is NOT in the death branch) and zeroed crashLoopCycles
            // after 10 min, silently de-latching the human lock. Guard with
            // `crashLoopCycles < 3`: the <3 cooldown behaviour is unchanged, and >=3
            // keeps PASSIVE until exitPassive() explicitly resets the cycle count.
            if (aliveSec > 600 && crashLoopCycles != 0 && crashLoopCycles < 3) {
                logWatchdog("Daemon healthy for %llds — resetting crash-loop backoff cycles (%d -> 0)",
                            aliveSec, crashLoopCycles);
                crashLoopCycles = 0;
            }
        }

        // --- Health checks (only when daemon is alive and not stuck) ---
        // these used to be log-only. Now each has a graded
        // response gated by consecutive-sample counters to avoid jitter.
        // A4: tempPassiveUntil (set/compared below) must run on CLOCK_BOOTTIME,
        // the same base the daemon heartbeats and the rest of this loop uses.
        // time(nullptr) is the wall clock: across suspend/resume or NTP step the
        // temp-passive window would mis-fire. Unify to bootTimeSec().
        time_t nowH = bootTimeSec();

        // temporary passive (mig-rate / load) auto-recovery
        if (tempPassiveUntil != 0 && nowH >= tempPassiveUntil) {
            logWatchdog("temporary passive expired (%lds), entering RECOVERY",
                        (long)(nowH - (tempPassiveUntil - watchdog::kTempPassiveDurationSec)));
            // P3-55: hysteresis. Do NOT snap straight back to active. Enter
            // RECOVERY (control_mode=assist); promote to NORMAL only after a
            // sustained clean window (kRecoveryStableSec), else a fault during
            // RECOVERY drops straight back to PASSIVE below.
            wdTransition(WdRuntimeState::RECOVERY, "temp_passive_expired");
            tempPassiveUntil = 0;
        }

        // 1) Memory: VmRSS > warn threshold. 3 consecutive → graceful restart.
        int memWarnKb = readConfigInt("DAEMON_MEMORY_WARN_KB", watchdog::kDefaultMemWarnKb);
        bool memOver = false;
        std::string statusPath = "/proc/" + std::to_string(dpid) + "/status";
        std::ifstream sf(statusPath);
        if (sf) {
            std::string line;
            while (std::getline(sf, line)) {
                if (line.find("VmRSS:") == 0) {
                    size_t pos = line.find_first_of("0123456789");
                    if (pos != std::string::npos) {
                        try {
                            int memKb = std::stoi(line.substr(pos));
                            if (memKb > memWarnKb) {
                                memOver = true;
                                logWatchdog("WARNING: Daemon memory %dKB > %dKB (consecutive=%d)",
                                            memKb, memWarnKb, consecutiveMemoryWarns + 1);
                            }
                        } catch (...) {
                            // Periodic VmRSS parse; malformed line. Log at most once
                            // (static) to avoid per-tick spam, then stay quiet.
                            static int s_vmRssParseWarned = 0;
                            if (!s_vmRssParseWarned) {
                                s_vmRssParseWarned = 1;
                                logWatchdog("WARNING: could not parse VmRSS line, leaving memWarn unchanged: %s",
                                            line.c_str());
                            }
                        }
                    }
                    break;
                }
            }
        }
        if (memOver) {
            consecutiveMemoryWarns++;
            lastMemWasOver = true;  // track for death classification
            if (consecutiveMemoryWarns >= watchdog::kConsecutiveThreshold) {
                logWatchdog("ACTION: memory over limit %d consecutive times, SIGTERM graceful restart",
                            consecutiveMemoryWarns);
                // P1-2: re-verify triple identity immediately before SIGTERM.
                // daemonAlive was checked earlier in this iteration, but we must
                // not signal a PID that may have been recycled between then and now.
                if (!isAatpedIdentity(dpid)) {
                    logWatchdog("refusing to SIGTERM pid=%d (identity check failed; stale/reused PID)",
                                dpid);
                } else {
                    kill(dpid, SIGTERM);
                    // wait briefly for graceful exit
                    bool graceful = false;
                    for (int i = 0; i < 20 && kill(dpid, 0) == 0; i++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (kill(dpid, 0) != 0) graceful = true;
                    if (!graceful) {
                        // P1-3: SIGTERM ignored — escalate to SIGKILL and verify death.
                        logWatchdog("SIGTERM pid=%d ignored, escalating to SIGKILL", dpid);
                        bool killed = killAndWaitDaemon(dpid);
                        if (!killed) {
                            logWatchdog("CRITICAL: pid=%d survived SIGKILL, NOT spawning new daemon",
                                        dpid);
                            consecutiveMemoryWarns = 0;
                            lastMemWasOver = false;
                            continue;
                        }
                    }
                }
                unlink(aatpe::paths::kPidFile);
                restartCount = 0;  // fresh restart cycle
                watchdogEmergencyMigrate();
                // R8-009 (LED-116): restartDaemon() returns false on spawn failure.
                // Previously ignored — a failed OOM-restart left daemonStartedAt at
                // the old value, so the next death was misclassified against a stale
                // clock and no CRITICAL was raised. Check it; on failure, log CRITICAL
                // and reset the start timestamp so classification stays correct.
                if (restartDaemon()) {
                    daemonStartedAt = bootTimeSec();
                } else {
                    logWatchdog("CRITICAL: OOM-path restartDaemon() failed; no daemon running this cycle");
                    daemonStartedAt = 0;
                }
                consecutiveMemoryWarns = 0;
                lastMemWasOver = false;  // reset after OOM restart
            }
        } else {
            consecutiveMemoryWarns = 0;
            lastMemWasOver = false;  // memory is normal now
        }

        // 2) Migration rate: 3 consecutive → temporary passive for 10s.
        int migCount = readInt((std::string(aatpe::paths::kStateDir) + "/migration_rate_current").c_str(), 0);
        int migLimit = readInt((std::string(aatpe::paths::kStateDir) + "/safety_migration_limit").c_str(),
                                watchdog::kDefaultMigrationLimit);
        if (migLimit <= 0) migLimit = watchdog::kDefaultMigrationLimit;
        if (migCount >= migLimit) {
            consecutiveMigWarns++;
            logWatchdog("WARNING: Migration rate %d/s exceeds limit %d (consecutive=%d)",
                        migCount, migLimit, consecutiveMigWarns);
            if (consecutiveMigWarns >= watchdog::kConsecutiveThreshold && tempPassiveUntil == 0) {
                logWatchdog("ACTION: migration rate over limit, entering temporary passive for %ds",
                            watchdog::kTempPassiveDurationSec);
                wdTransition(WdRuntimeState::PASSIVE, "migration_rate");
                tempPassiveUntil = nowH + watchdog::kTempPassiveDurationSec;
            }
        } else {
            consecutiveMigWarns = 0;
        }

        // 3) System load: 3 consecutive → temporary passive for 10s.
        int loadLimit = readConfigInt("SAFETY_LOAD_LIMIT", watchdog::kDefaultLoadLimit);
        std::ifstream lf("/proc/loadavg");
        if (lf) {
            float load1 = 0.0f;
            lf >> load1;
            int loadInt = (int)(load1 * 100);
            if (loadInt >= loadLimit * 100) {
                consecutiveLoadWarns++;
                logWatchdog("WARNING: System load critical: %.2f (consecutive=%d)",
                            load1, consecutiveLoadWarns);
                if (consecutiveLoadWarns >= watchdog::kConsecutiveThreshold && tempPassiveUntil == 0) {
                    logWatchdog("ACTION: load over limit, entering temporary passive for %ds",
                                watchdog::kTempPassiveDurationSec);
                    wdTransition(WdRuntimeState::PASSIVE, "system_load");
                    tempPassiveUntil = nowH + watchdog::kTempPassiveDurationSec;
                }
            } else {
                consecutiveLoadWarns = 0;
            }
        }

        // 4) Thermal emergency: read the daemon flag, then layer the ROUND-003
        //    independent backup reading on top.
        int thermalEmergency = readInt((std::string(aatpe::paths::kStateDir) + "/thermal_emergency").c_str(), 0);

        // ROUND-003: while in EMERGENCY, read the sensors ourselves. If the daemon
        // flag is stuck at 1 but our own reads have been cool (SAFE) for a full
        // window, treat the flag as stale and allow the exit. If we read HOT we
        // stay; if we read NOTHING we also stay (fail-safe, never "no read = cool").
        bool indForcedClear = false;
        bool effThermal = (thermalEmergency == 1);  // refined below; P1 round7 active-scan may force it
        if (g_wdState == WdRuntimeState::EMERGENCY) {
            long long indMaxC = wdReadMaxTempCelsius();
            WdTempRead tr = wdClassifyIndTemp(indMaxC, indThermalWallC, indThermalMarginC);
            if (wdIndTempSample(tr, emergIndSafeStreak, emergIndNeedSamples)) {
                indForcedClear = true;
                logWatchdog("WARN: daemon thermal flag stale (==1), watchdog independent "
                            "max temp=%lldC <= %dC (wall=%dC margin=%dC) for %ds — exiting EMERGENCY",
                            indMaxC, indThermalWallC - indThermalMarginC,
                            indThermalWallC, indThermalMarginC, emergencyClearSec);
            }
        } else {
            // P1 round7: ACTIVE independent scan (not only when already EMERGENCY).
            // If the daemon's thermal management has failed to raise its flag but
            // the real sensor is at/above the hard wall, the watchdog must itself
            // trigger emergency tightening. The wall is never loosened here.
            long long indMaxC = wdReadMaxTempCelsius();
            if (indMaxC >= (long long)indThermalWallC) {
                logWatchdog("WARN: watchdog independent scan: max temp=%lldC >= wall=%dC "
                            "but daemon flag=0 — forcing emergency (daemon thermal mgmt failed)",
                            indMaxC, indThermalWallC);
                effThermal = true;
            }
            emergIndSafeStreak = 0;
        }
        // Effective thermal emergency: the daemon flag, UNLESS our independent
        // reading has independently confirmed it has cooled.
        if (indForcedClear) effThermal = false;

        // G9-001: stamp when thermal first cleared while we are in EMERGENCY, so the
        // exit edge requires a CONTINUOUS clear window (no flicker back).
        if (effThermal) {
            emergClearSince = 0;
        } else if (g_wdState == WdRuntimeState::EMERGENCY && emergClearSince == 0) {
            emergClearSince = nowH;
            if (indForcedClear) {
                // The independent safe streak already spent emergencyClearSec cool;
                // don't make the device wait another full window.
                emergClearSince = nowH - emergencyClearSec;
                logWatchdog("Thermal cleared (daemon flag=%d, watchdog independent read authoritative); "
                            "stepping toward RECOVERY", thermalEmergency);
            } else {
                logWatchdog("Thermal cleared; starting %lds exit window before leaving EMERGENCY",
                            (long)emergencyClearSec);
            }
        }

        // G9-001 / G9-002: single table-driven steady-state transition. Active faults
        // that block promotion right now: thermal==1 (effective), or an in-flight temp
        // passive window. When neither fires, the recovery hysteresis applies.
        WdEvidence ev;
        ev.daemonAlive    = daemonAlive;
        ev.thermalEmergency = effThermal;
        ev.migOver        = (tempPassiveUntil != 0);  // inside a temp passive window
        ev.loadOver       = false;
        ev.explicitStop  = false;                     // handled by early continue above
        WdRuntimeState nextState = wdSteadyTransition(g_wdState, ev, nowH,
                                                     emergClearSince, g_recoverySince,
                                                     watchdog::kRecoveryStableSec,
                                                     emergencyClearSec);
        if (nextState != g_wdState) {
            const char* reason =
                (nextState == WdRuntimeState::RECOVERY && g_wdState == WdRuntimeState::EMERGENCY)
                    ? (indForcedClear ? "thermal_stale_flag_independent_read" : "thermal_cleared")
                    : (nextState == WdRuntimeState::NORMAL ? "recovery_stable" : "recover");
            wdTransition(nextState, reason);
            // leaving EMERGENCY -> clear the clear-window stamp.
            if (g_wdState == WdRuntimeState::NORMAL) emergClearSince = 0;
        }
    }

    logWatchdog("Watchdog stopping");
    unlink(WD_PID_FILE);
    unlink(WD_LOCK_FILE);
    if (g_wdLogFp) fclose(g_wdLogFp);
    return 0;
}
