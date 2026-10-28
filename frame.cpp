#include <cmath>
// AATPE - Frame Awareness // AATPE - Frame Awareness & Refresh Rate (fix45 Problem 5: split from aatped.cpp) Refresh Rate (split from aatped.cpp)
// Contains: aatpeExec, dumpsys cooldown, refresh rate detection (sysfs + dumpsys),
// readSurfaceFlinger, foreground app detection (cgroup/oom/dumpsys), game detection,
// battery state, auto profile selection.
#include "aatped.h"
#include "frame_binder.h"
#include "aatped_apklabel.h"   // v4.3.1: background label warmup (peek/labelForPackage)
#include <algorithm>
#include <chrono>
#include <deque>
#include <list>
#include <map>      // F-5: per-package grouping of the single timestats dump parse
#include <mutex>    // v4.3.1h2: sInstalledPkgsMtx for installed-package cache
#include <set>      // P0: /data/app scan de-dup
#include <thread>   // v4.3.1 hotfix: dedicated label-warmup background thread
#include <sys/prctl.h>  // PR_SET_PDEATHSIG for child process safety
#include <sys/inotify.h> // Tier-2 event-driven foreground cache invalidation
#include <unistd.h>      // read() drain on the inotify fd

// 问题9 fix: LOG_FRAME config gates frame-module verbose logs.
// LOG_WARN/LOG_ERROR are always emitted (operator diagnostics: timeouts,
// disabled FrameSync, foreground-app extraction failures). Routine
// INFO/DEBUG logs (refresh-rate changes, foreground picks, auto-profile
// transitions, per-cycle debug) respect g_cfg.logFrame so a battery profile
// with LOG_FRAME=0 can silence the frame loop without dropping fault signals.
#define FRAME_LOG_INFO(...)  do { if (g_cfg.logFrame) logMsg(2, __VA_ARGS__); } while(0)
// #24: frame debug logging is compile-trimmed in release (AATPE_DEBUG undefined).
#ifdef AATPE_DEBUG
  #define FRAME_LOG_DEBUG(...) do { if (g_cfg.logFrame) do { if ((g_debugSampleCounter.fetch_add(1,std::memory_order_relaxed)%100)==0) logMsg(3,__VA_ARGS__); } while(0); } while(0)
#else
  #define FRAME_LOG_DEBUG(...) ((void)0)
#endif

// FRAME AWARENESS (V3) - with SF latency caching (optimization)
// Cache SF latency detection for 500ms (sfLatCacheMs < 500 threshold) to avoid repeated dumpsys calls.
// P3-8: s_cachedAvgLat / s_cachedMaxLat were dead code (never read/written); removed.
// passive mode protection: skip frame adjustments when controlMode == "passive"
// ============================================================================
// COMPLETE REWRITE of aatpeExec to fix 4 root-cause bugs:
//
// BUG #1 (PRIMARY): Blocking waitpid(child, &status, 0) in timeout path.
//   When dumpsys child enters D-state (uninterruptible sleep via Binder IPC
//   hang with SurfaceFlinger/ActivityManager), SIGKILL cannot kill it
//   immediately. waitpid(..., 0) then blocks FOREVER, freezing the daemon
//   loop. Heartbeat stops, counters freeze, watchdog kills daemon but can't
//   restart it (see watchdog BUG).
//
// BUG #2: Per-poll timeout, not total timeout.
//   poll() timeout resets on every iteration. If child sends data slowly
//   (1 byte every 1900ms), poll never times out and loop runs forever.
//
// BUG #3: Only kills sh child, not dumpsys grandchild.
//   Process tree: aatped -> sh -> dumpsys. SIGKILL to sh kills sh, but
//   dumpsys becomes orphan (reparented to init) and stays in D-state.
//
// BUG #4: Zombie leak in non-timeout path.
//   If child doesn't exit within 50ms after EOF, third WNOHANG returns 0
//   and function returns without reaping -> zombie accumulates.
//
// FIXES:
//   1. Total deadline (3000ms wall-clock) enforced in poll loop.
//   2. Child calls setpgid(0,0) -> parent kills(-child, SIGKILL) to kill
//      entire process group (sh + dumpsys + all grandchildren).
//   3. Non-blocking waitpid with bounded retries (max 300ms) in ALL paths.
//      Never blocks the daemon for more than 300ms on child reaping.
//   4. Dumpsys cooldown: after any timeout, skip all dumpsys calls for 10s
//      to avoid hammering a hung SurfaceFlinger.
//   5. SIGCHLD handler with SA_NOCLDWAIT installed in daemonInit() to
//      auto-reap any residual zombies (prevents PID exhaustion).
#include <poll.h>
#include <cctype>   // isalnum/isalpha for package-name whitelist validation
#include <dirent.h>  // P0: scan /data/app as root-side fallback when pm/cmd blocked
// .cpp files may use using namespace (not included elsewhere)
using namespace aatpe;
using namespace aatpe_ml;

// forward declaration. Definition lives next to aatpeExtractPackage();
// needed here because the gfxinfo shell sink (in aatpeDetectSFLatency) also
// uses the validator as defence-in-depth.
static bool aatpeIsValidPackageName(const std::string& p);

// #8/#9: the hardened exec (aatpeExec / aatpeExecArgv) and the dumpsys cooldown
// state/accessors (g_dumpsysCooldownUntil / dumpsysAvailable / triggerDumpsysCooldown)
// have moved to aatped_utils.cpp — the canonical shared home, so cold_start_rules and
// thermal can reuse them without pulling frame.cpp. They are declared in
// aatped_utils.h; frame.cpp just calls them. Only frame's own cache lock stays here.
// mutex protecting all static cache variables in frame.cpp
// (refresh rate, SF latency, layer count caches). These are accessed from the
// daemon loop and potentially observability threads; without a lock they race.
static std::recursive_mutex g_frameCacheMutex;


static int aatpeFirstInt(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) i++;
    return (i < s.size()) ? parseIntSafe(s.c_str() + i) : 0;
}

static float aatpeFirstFloat(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && !(s[i] >= '0' && s[i] <= '9') && s[i] != '.') i++;
    return (i < s.size()) ? atof(s.c_str() + i) : 0.0f;
}

static int aatpeExtractIntAfter(const std::string& haystack, const char* key) {
    size_t pos = 0;
    std::string k(key);
    while ((pos = haystack.find(k, pos)) != std::string::npos) {
        size_t i = pos + k.size();
        while (i < haystack.size() && !(haystack[i] >= '0' && haystack[i] <= '9')) i++;
        if (i < haystack.size()) {
            int v = parseIntSafe(haystack.c_str() + i);
            if (v >= refresh::kMinRate && v <= refresh::kMaxRate) return v;
        }
        pos += k.size();
    }
    return 0;
}

// Extract float after a key with WORD BOUNDARY check.
// Unlike aatpeExtractIntAfter (substring match), this ensures the key is
// preceded by a non-alphanumeric character (or start of string), so that
// searching for "refreshRate" does NOT match inside "peakRefreshRate".
// This is the core fix for current mode returning peak rate (120Hz) instead
// of the actual current refresh rate.
static float aatpeExtractFloatAfterWordBoundary(const std::string& haystack, const char* key) {
    size_t pos = 0;
    std::string k(key);
    while ((pos = haystack.find(k, pos)) != std::string::npos) {
        // Word boundary: char before key must NOT be alphanumeric or underscore
        if (pos > 0) {
            char c = haystack[pos - 1];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                pos += k.size();
                continue; // substring match inside another field, skip
            }
        }
        size_t i = pos + k.size();
        while (i < haystack.size() && !(haystack[i] >= '0' && haystack[i] <= '9') && haystack[i] != '.') i++;
        if (i < haystack.size()) {
            return atof(haystack.c_str() + i);
        }
        pos += k.size();
    }
    return 0.0f;
}

// Extract refresh rate from a "mode" block line.
// Android dumpsys display represents the active display mode as something like:
//   mCurrentMode=1080x2400/60.0
//   activeMode={id=1, width=1080, height=2400, refreshRate=60.0}
// Parse current refresh rate from dumpsys SurfaceFlinger output.
// Based on AOSP official source code (3 verified fields):
//   1. SurfaceFlinger.cpp dumpAllLocked:  " refresh-rate : 120.00fps"
//   2. DisplayDevice.cpp dump:             "activeMode={id=0,...,refreshRate=120.00fps,...}"
//   3. mRefreshRateConfigs->dump:          "Current mode: {id=0,...,fps=120.00,...}"
// All use exact prefix matching (not fuzzy substring) per AOSP source.
static int aatpeParseCurrentFromSurfaceFlinger(const std::string& sfOutput) {
    if (sfOutput.empty()) return 0;
    std::istringstream iss(sfOutput);
    std::string ln;

    // Pass 1: SurfaceFlinger.cpp dumpAllLocked emits a SINGLE
    // global line " refresh-rate : X.XXfps" that is the LIVE current vsync rate.
    // This is the authoritative field. The old single-pass "first match wins"
    // scan could hit a per-display "activeMode=" block BEFORE the global
    // "refresh-rate :" line on a multi-display MTK dump, returning a panel-config
    // peak instead of the live rate. Scan for the global rate FIRST, in a
    // dedicated pass, and only fall through to per-display fields if absent.
    while (std::getline(iss, ln)) {
        std::string t = ln;
        trim(t);
        if (t.rfind("refresh-rate", 0) == 0) {
            size_t colon = t.find(':');
            if (colon != std::string::npos) {
                float f = aatpeFirstFloat(t.substr(colon));
                if (f >= (float)refresh::kMinRate && f <= (float)refresh::kMaxRate) return (int)lroundf(f);
            }
        }
    }

    // Pass 2: per-display activeMode / "Current mode:" blocks (AOSP DisplayDevice
    // / mRefreshRateConfigs). Only reached when the global "refresh-rate :" line
    // is absent (OEM builds that omit it).
    iss.clear();
    iss.seekg(0, std::ios::beg);
    while (std::getline(iss, ln)) {
        // --- DisplayDevice.cpp "activeMode={id=0,...,refreshRate=120.00fps,...}" ---
        {
            size_t amPos = ln.find("activeMode=");
            if (amPos != std::string::npos) {
                if (amPos == 0 || !(isalnum((unsigned char)ln[amPos-1]) || ln[amPos-1] == '_')) {
                    float f = aatpeExtractFloatAfterWordBoundary(ln, "refreshRate");
                    if (f >= (float)refresh::kMinRate && f <= (float)refresh::kMaxRate) return (int)lroundf(f);
                    float f2 = aatpeExtractFloatAfterWordBoundary(ln, "fps");
                    if (f2 >= (float)refresh::kMinRate && f2 <= (float)refresh::kMaxRate) return (int)lroundf(f2);
                }
            }
        }
        // --- mRefreshRateConfigs->dump "Current mode: {id=0,...,fps=120.00,...}" ---
        {
            std::string t = ln;
            trim(t);
            if (t.rfind("Current mode:", 0) == 0) {
                float f = aatpeExtractFloatAfterWordBoundary(t, "fps");
                if (f >= (float)refresh::kMinRate && f <= (float)refresh::kMaxRate) return (int)lroundf(f);
                float f2 = aatpeExtractFloatAfterWordBoundary(t, "refreshRate");
                if (f2 >= (float)refresh::kMinRate && f2 <= (float)refresh::kMaxRate) return (int)lroundf(f2);
            }
        }
    }
    return 0;
}

// Parse current refresh rate from dumpsys display mBaseDisplayInfo.
// Based on AOSP official source code (DisplayManagerService / DisplayDeviceInfo):
//   mBaseDisplayInfo=DisplayInfo{..., mode N, ..., modes [{id=N, width=..., height=..., fps=...}, ...]}
//
// Parsing logic:
//   1. Find the line containing "mBaseDisplayInfo="
//   2. Extract the current mode ID from "mode N" (N is an integer)
//   3. Find the "modes [...]" list in the same line
//   4. Within modes list, find the entry with id=N
//   5. Read its fps= value
//
// Also supports "renderFrameRate N" as a secondary fallback on the same line.
static int aatpeParseCurrentFromDisplayBaseInfo(const std::string& displayOutput) {
    if (displayOutput.empty()) return 0;
    std::istringstream iss(displayOutput);
    std::string ln;
    while (std::getline(iss, ln)) {
        // Find line with mBaseDisplayInfo= (exact prefix, word boundary)
        size_t bdiPos = ln.find("mBaseDisplayInfo=");
        if (bdiPos == std::string::npos) continue;
        // Word boundary check
        if (bdiPos > 0 && (isalnum((unsigned char)ln[bdiPos-1]) || ln[bdiPos-1] == '_')) continue;

        // --- Step 1: Extract current mode ID from "mode N" ---
        // Pattern: " mode N" where N is integer (word boundary on "mode")
        int currentModeId = -1;
        {
            size_t modePos = 0;
            while ((modePos = ln.find("mode ", modePos)) != std::string::npos) {
                // Word boundary: char before "mode" must not be alphanumeric or underscore
                if (modePos > 0) {
                    char c = ln[modePos - 1];
                    if (isalnum((unsigned char)c) || c == '_') {
                        modePos += 5;
                        continue;
                    }
                }
                // Parse integer after "mode "
                size_t numStart = modePos + 5;
                if (numStart < ln.size() && isdigit((unsigned char)ln[numStart])) {
                    int id = 0;
                    size_t i = numStart;
                    while (i < ln.size() && isdigit((unsigned char)ln[i])) {
                        id = id * 10 + (ln[i] - '0');
                        // P3-26: hand-rolled decimal parser — cap at 5 digits
                        // (mode ids are small) so a hostile/truncated line
                        // cannot overflow `int` via repeated *10.
                        if (id > 100000) break;
                        i++;
                    }
                    currentModeId = id;
                    break;
                }
                modePos += 5;
            }
        }

        // --- Step 2: Find modes [...] list and locate id=currentModeId entry ---
        if (currentModeId >= 0) {
            size_t modesPos = ln.find("modes [");
            if (modesPos == std::string::npos) modesPos = ln.find("modes[");
            if (modesPos != std::string::npos) {
                // Find the closing bracket
                size_t bracketStart = ln.find('[', modesPos);
                if (bracketStart != std::string::npos) {
                    size_t bracketEnd = ln.find(']', bracketStart);
                    if (bracketEnd == std::string::npos) bracketEnd = ln.size();
                    std::string modesList = ln.substr(bracketStart + 1, bracketEnd - bracketStart - 1);

                    // Parse each mode entry: {id=N, width=..., height=..., fps=...}
                    // Entries are separated by "}, {" or ", "
                    size_t entryStart = 0;
                    while (entryStart < modesList.size()) {
                        // Find next "{"
                        size_t ob = modesList.find('{', entryStart);
                        if (ob == std::string::npos) break;
                        size_t cb = modesList.find('}', ob);
                        if (cb == std::string::npos) break;
                        std::string entry = modesList.substr(ob, cb - ob + 1);

                        // Extract id= from this entry
                        int entryId = -1;
                        size_t idPos = entry.find("id=");
                        if (idPos != std::string::npos) {
                            size_t idNumStart = idPos + 3;
                            if (idNumStart < entry.size() && isdigit((unsigned char)entry[idNumStart])) {
                                int id = 0;
                                size_t i = idNumStart;
                                while (i < entry.size() && isdigit((unsigned char)entry[i])) {
                                    id = id * 10 + (entry[i] - '0');
                                    // P3-26: overflow guard (same as mode-id parser).
                                    if (id > 100000) break;
                                    i++;
                                }
                                entryId = id;
                            }
                        }

                        // If this is the current mode, extract fps=
                        if (entryId == currentModeId) {
                            float f = aatpeExtractFloatAfterWordBoundary(entry, "fps");
                            if (f >= (float)refresh::kMinRate && f <= (float)refresh::kMaxRate) return (int)lroundf(f);
                            // Also try refreshRate= as fallback
                            float f2 = aatpeExtractFloatAfterWordBoundary(entry, "refreshRate");
                            if (f2 >= (float)refresh::kMinRate && f2 <= (float)refresh::kMaxRate) return (int)lroundf(f2);
                        }
                        entryStart = cb + 1;
                    }
                }
            }
        }

        // --- Step 3 (fallback): renderFrameRate N on the same mBaseDisplayInfo line ---
        // AOSP DisplayInfo also contains renderFrameRate field
        {
            size_t rfrPos = ln.find("renderFrameRate");
            if (rfrPos != std::string::npos) {
                // Word boundary check
                if (rfrPos == 0 || !(isalnum((unsigned char)ln[rfrPos-1]) || ln[rfrPos-1] == '_')) {
                    // Parse number after "renderFrameRate" (may be "renderFrameRate=120" or "renderFrameRate 120")
                    size_t i = rfrPos + 15;
                    while (i < ln.size() && !(isdigit((unsigned char)ln[i]) || ln[i] == '.')) i++;
                    if (i < ln.size()) {
                        float f = atof(ln.c_str() + i);
                        if (f >= (float)refresh::kMinRate && f <= (float)refresh::kMaxRate) return (int)lroundf(f);
                    }
                }
            }
        }
    }
    return 0;
}

// Parse refresh rate from dumpsys SurfaceFlinger --latency output.
// AOSP official: first line contains the refresh period in nanoseconds.
// refresh_rate = 1e9 / period_ns
static int aatpeParseRefreshFromLatency(const std::string& latencyOutput) {
    if (latencyOutput.empty()) return 0;
    std::istringstream iss(latencyOutput);
    std::string ln;
    while (std::getline(iss, ln)) {
        trim(ln);
        if (ln.empty()) continue;
        // Try 3-column format: "expected present timestamp" "actual present timestamp" "vsync period"
        long long v[3];
        int n = sscanf(ln.c_str(), "%lld %lld %lld", &v[0], &v[1], &v[2]);
        if (n == 3) {
            // Third column is vsync period in ns
            if (v[2] >= refresh::kMinVsyncPeriodNs && v[2] <= refresh::kMaxVsyncPeriodNs) {
                int r = (int)(refresh::kOneSecondNs / v[2]);
                if (r >= refresh::kMinRate && r <= refresh::kMaxRate) return r;
            }
        } else if (n == 1) {
            // Single column: just the vsync period
            char extra = 0;
            long long val = 0;
            if (sscanf(ln.c_str(), "%lld %c", &val, &extra) == 1) {
                if (val >= refresh::kMinVsyncPeriodNs && val <= refresh::kMaxVsyncPeriodNs) {
                    int r = (int)(refresh::kOneSecondNs / val);
                    if (r >= refresh::kMinRate && r <= refresh::kMaxRate) return r;
                }
            }
        }
    }
    return 0;
}

void aatpeProbeEnvironment() {
    LOG_INFO("PROBE: === Environment Diagnostics ===");
    LOG_INFO("PROBE: uid=%d euid=%d gid=%d egid=%d", getuid(), geteuid(), getgid(), getegid());
    // Log Android SDK version + version name (Android 10-17 audit)
    int sdk = aatpeAndroidSdkInt();
    LOG_INFO("PROBE: Android SDK=%d (%s) cgroupV2Cpuset=%d cpusetMount=%s",
             sdk, aatpeAndroidVersionName(),
             aatpeIsCgroupV2Cpuset() ? 1 : 0, aatpeCpusetMountRoot().c_str());
    const char* path = getenv("PATH");
    LOG_INFO("PROBE: PATH=%s", path ? path : "(null)");
    struct stat st;
    const char* dumpsysPaths[] = {"/system/bin/dumpsys", "/system/xbin/dumpsys", "/vendor/bin/dumpsys"};
    for (const char* dp : dumpsysPaths) {
        if (stat(dp, &st) == 0) LOG_INFO("PROBE: found %s (mode=%o size=%ld)", dp, st.st_mode & 0777, (long)st.st_size);
        else { int e = errno; LOG_INFO("PROBE: %s NOT found (errno=%d)", dp, e); }
    }
    // P0-3: Read SELinux state directly from /sys/fs/selinux/enforce (no popen)
    std::string se = readFile("/sys/fs/selinux/enforce");
    trim(se);
    const char* seStr = (se == "1") ? "Enforcing" : ((se == "0") ? "Permissive" : "Unknown");
    LOG_INFO("PROBE: SELinux=%s (enforce=%s)", seStr, se.empty() ? "(unreadable)" : se.c_str());
    std::string svc = aatpeExec("dumpsys -l 2>&1 | head -n 5", "PROBE_dumpsys_l");
    LOG_INFO("PROBE: dumpsys -l output (first 5 lines): %s", svc.empty() ? "(EMPTY)" : svc.c_str());
    LOG_INFO("PROBE: === End Diagnostics ===");
}

static int aatpeReadSysfsRefreshRate() {
    int best = 0;
    DIR* drmDir = opendir("/sys/class/drm");
    if (!drmDir) return 0;
    struct dirent* entry;
    while ((entry = readdir(drmDir)) != nullptr) {
        if (strncmp(entry->d_name, "card", 4) != 0) continue;
        if (strchr(entry->d_name, '-') == nullptr) continue;
        std::string basePath = std::string("/sys/class/drm/") + entry->d_name;
        // Problem 7: check status file — only consider connected connectors
        std::string status = readFile((basePath + "/status").c_str());
        if (!status.empty() && status.find("connected") == std::string::npos) continue;
        std::string modesPath = basePath + "/modes";
        std::ifstream f(modesPath);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            trim(line);
            if (line.empty()) continue;
            size_t sp = line.find(' ');
            if (sp == std::string::npos) continue;
            float rr = atof(line.c_str() + sp + 1);
            int r = (int)lroundf(rr);
            if (r >= refresh::kMinRate && r <= refresh::kMaxRate && r > best) best = r;
        }
    }
    closedir(drmDir);
    return best;
}

// Problem 7: Read current refresh rate from sysfs DRM.
// //   The old implementation read the FIRST line of
//   /sys/class/drm/cardN-X/modes and treated it as the *current* active mode.
//   That assumption is WRONG. The DRM "modes" file lists the SUPPORTED modes of
//   the connector, ordered by preference; its first line is the PREFERRED/PEAK
//   mode, never the live active mode. On the Redmi Note10 Pro (Dimensity 1100,
//   panel 120Hz) this returned the connector peak (144Hz) instead of the actual
//   120Hz. Two further bugs compounded it:
//     (a) `if (!status.empty() && status.find("connected")==npos) continue;`
//         -- when the status file is unreadable (empty under SELinux/root on
//         this kernel), the condition is false, so a NOT-connected virtual/HDMI
//         connector was accepted and its peak mode was reported.
//     (b) readdir() order is undefined, so a bogus connector could win over the
//         real panel.
//   Android DRM sysfs exposes NO "current active mode" node (the live rate
//   requires an ioctl on /dev/dri/cardN), so sysfs cannot authoritatively read
//   it. Return 0 here so the authoritative dumpsys SurfaceFlinger path (which
//   reads SurfaceFlinger's CURRENT vsync) takes over.
//
// FAST sysfs path (no shell, no dumpsys fork).
//   The DRM `modes` file marks the CURRENTLY ACTIVE mode with a leading '*'; the
//   second whitespace-separated column of that line is the live refresh rate (Hz).
//   We ONLY trust the '*'-active line (never the peak/first line), so this never
//   regresses to the 144Hz-peak bug the old author hit. When the kernel does not
//   mark the active mode (older Android 4.14), this returns 0 and the proven
//   dumpsys fallback runs exactly as before. We also probe the MediaTek GED live
//   composer-FPS nodes (Dimensity 1100), which KernelSU-root can read directly.
static int aatpeReadSysfsCurrentRefreshRate() {
    auto parseModeRefresh = [](const std::string& line) -> int {
        size_t sp = line.find(' ');
        if (sp == std::string::npos) return 0;
        float rr = atof(line.c_str() + sp + 1);
        int r = (int)lroundf(rr);
        if (r >= refresh::kMinRate && r <= refresh::kMaxRate) return r;
        return 0;
    };

    DIR* drmDir = opendir("/sys/class/drm");
    if (drmDir) {
        struct dirent* entry;
        int activeBest = 0;
        while ((entry = readdir(drmDir)) != nullptr) {
            if (strncmp(entry->d_name, "card", 4) != 0) continue;
            std::string basePath = std::string("/sys/class/drm/") + entry->d_name;
            // Only consider connected connectors (skip when status is readable but
            // explicitly NOT connected). An unreadable status (empty under SELinux)
            // is allowed so the real panel still matches.
            std::string status = readFile((basePath + "/status").c_str());
            if (!status.empty() && status.find("connected") == std::string::npos) continue;
            std::string modesPath = basePath + "/modes";
            std::ifstream f(modesPath.c_str());
            if (!f) continue;
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty() || line[0] != '*') continue;  // active-mode marker
                int r = parseModeRefresh(line.substr(1));
                if (r > activeBest) activeBest = r;
                break;  // at most one active mode per connector
            }
        }
        closedir(drmDir);
        if (activeBest >= refresh::kMinRate && activeBest <= refresh::kMaxRate) {
            return activeBest;
        }
    }

    // MediaTek GED (Graphics Environment Direct) live composer FPS nodes.
    const char* mtkNodes[] = {
        "/sys/kernel/ged/head/primary_fps",
        "/sys/kernel/ged/head/head0/fps",
        "/sys/kernel/ged/head/fps",
        nullptr
    };
    for (int i = 0; mtkNodes[i]; i++) {
        std::ifstream f(mtkNodes[i]);
        if (!f) continue;
        int v = 0; f >> v;
        if (v >= refresh::kMinRate && v <= refresh::kMaxRate) return v;
    }
    return 0;
}

// F-06: MTK GED (Graphics Environment Direct) composer primary_fps as a GAME-FPS
// fallback source (NOT panel refresh). On Dimensity 1100 these nodes expose the
// composer's actual composite rate, which is the most reliable "real on-screen
// fps" when the per-layer latency ring is sparse. Returns 0 when no node is
// readable / in range. Distinct from aatpeReadSysfsCurrentRefreshRate(): that one
// uses GED only to detect the PANEL refresh; this one reports the ACTUAL composite
// fps for the game panel.
static int aatpeReadGedComposerFps() {
    const char* mtkNodes[] = {
        "/sys/kernel/ged/head/primary_fps",
        "/sys/kernel/ged/head/head0/fps",
        "/sys/kernel/ged/head/fps",
        nullptr
    };
    for (int i = 0; mtkNodes[i]; i++) {
        std::ifstream f(mtkNodes[i]);
        if (!f) continue;
        int v = 0; f >> v;
        // Composer fps is 1..240 on real MTK; reject 0 / absurd.
        if (v >= 1 && v <= 240) return v;
    }
    return 0;
}

static int aatpeMaxRefreshInLine(const std::string& line) {
    int best = 0;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && !(line[i] >= '0' && line[i] <= '9')) i++;
        if (i >= line.size()) break;
        int v = 0;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
            // LED-052 (R4-008): overflow guard. Without this, a malformed
            // dumpsys line with a very long digit string would make v*10+...
            // overflow int (UB). 1,000,000 is well above kMaxRate and is the
            // same ceiling the mode-id parser in this file uses.
            if (v > 1000000) break;
            v = v * 10 + (line[i] - '0');
            i++;
        }
        if (v >= refresh::kMinRate && v <= refresh::kMaxRate && v > best) best = v;
    }
    return best;
}

// Forward declaration - detectGameProcess() is defined later in the file
bool detectGameProcess();

// Peak refresh rate detection (existing behavior, extracted as helper)
// All sources take the MAX value. drm/modes first, settings peak second, dumpsys fallback.
static int aatpeDetectPeakRefreshRate() {
    int best = 0;
    // P1-6: /sys/class/drm/modes is FIRST choice (direct file read, fast & stable)
    {
        int r = aatpeReadSysfsRefreshRate();
        if (r > 0) best = r;
    }
    // P1-6: settings provider as second choice
    if (best == 0) {
        const char* cmds[] = {
            "settings get system peak_refresh_rate 2>/dev/null",
            "settings get secure peak_refresh_rate 2>/dev/null",
            "settings get system min_refresh_rate 2>/dev/null",
            "settings get system refresh_rate 2>/dev/null",
        };
        for (const char* cmd : cmds) {
            std::string o = aatpeExec(cmd, "VSYNC_SET");
            if (!o.empty()) {
                float f = aatpeFirstFloat(o);
                if (f >= (float)refresh::kMinRate && f <= (float)refresh::kMaxRate) {
                    int r = (int)lroundf(f);
                    if (r > best) best = r;
                }
            }
        }
    }
    // P1-6: dumpsys SurfaceFlinger --latency as third choice
    if (best == 0) {
        std::string o = aatpeExec("dumpsys SurfaceFlinger --latency 2>/dev/null", "VSYNC_LAT");
        if (!o.empty()) {
            long long minPeriodNs = 0;
            std::istringstream iss(o);
            std::string ln;
            while (std::getline(iss, ln)) {
                trim(ln);
                if (ln.empty()) continue;
                long long v[3];
                int n = sscanf(ln.c_str(), "%lld %lld %lld", &v[0], &v[1], &v[2]);
                if (n == 3) {
                    if (v[2] >= refresh::kMinVsyncPeriodNs && v[2] <= refresh::kMaxVsyncPeriodNs) {
                        if (minPeriodNs == 0 || v[2] < minPeriodNs) minPeriodNs = v[2];
                    }
                } else if (n == 1) {
                    char extra = 0; long long val = 0;
                    if (sscanf(ln.c_str(), "%lld %c", &val, &extra) == 1) {
                        if (val >= refresh::kMinVsyncPeriodNs && val <= refresh::kMaxVsyncPeriodNs) {
                            if (minPeriodNs == 0 || val < minPeriodNs) minPeriodNs = val;
                        }
                    }
                }
            }
            if (minPeriodNs > 0) {
                int r = (int)(refresh::kOneSecondNs / minPeriodNs);
                if (r >= refresh::kMinRate && r <= refresh::kMaxRate && r > best) best = r;
            }
        }
    }
    // P1-6: dumpsys display as fourth choice
    if (best == 0) {
        std::string o = aatpeExec("dumpsys display 2>/dev/null", "VSYNC_DISP");
        if (!o.empty()) {
            int peak = 0;
            const char* keys[] = {"peakRefreshRate", "supportedRefreshRates", "mSupportedRefreshRates", "currentRefreshRate", "mRefreshRate", "defaultRefreshRate", "appRequestedRefreshRate", "refresh-rate", "refreshRate"};
            for (const char* k : keys) {
                int r = aatpeExtractIntAfter(o, k);
                if (r > peak) peak = r;
            }
            std::istringstream iss(o);
            std::string ln;
            while (std::getline(iss, ln)) {
                if (ln.find("efresh") != std::string::npos || ln.find("fps") != std::string::npos || ln.find("FPS") != std::string::npos) {
                    int v = aatpeMaxRefreshInLine(ln);
                    if (v > peak) peak = v;
                }
            }
            if (peak > 0 && peak > best) best = peak;
        }
    }
    // P1-6: dumpsys window as last fallback
    if (best == 0) {
        std::string o = aatpeExec("dumpsys window 2>/dev/null | grep -i -E 'refresh|fps' | head -n 5", "VSYNC_WIN");
        int r = aatpeFirstInt(o);
        if (r >= refresh::kMinRate && r <= refresh::kMaxRate) best = r;
    }
    if (best == 0) best = refresh::kDefaultRate;
    return best;
}

// Current refresh rate detection (AOSP-OFFICIAL REWRITE)
//
// ROOT CAUSE OF BUG:
//   old code used fields that DO NOT EXIST in AOSP official source code:
//   mCurrentMode, activeMode/currentMode blocks (in dumpsys display),
//   mRequestedRefreshRate, currentRefreshRate, mRefreshRate.
//   These fields are either OEM-specific or completely fabricated.
//
// STRATEGY (all fields verified against AOSP official source code):
//   Priority 1: dumpsys SurfaceFlinger (3 verified AOSP fields)
//               - " refresh-rate : 120.00fps"  (SurfaceFlinger.cpp dumpAllLocked)
//               - "activeMode={...refreshRate=...}"  (DisplayDevice.cpp dump)
//               - "Current mode: {...fps=...}"  (mRefreshRateConfigs->dump)
//   Priority 2: dumpsys display mBaseDisplayInfo (AOSP DisplayDeviceInfo)
//               - "mBaseDisplayInfo=DisplayInfo{..., mode N, ..., modes [{id=N,...,fps=...}, ...]}"
//               - Extract mode ID → find in modes list → read fps=
//               - Fallback: "renderFrameRate N" on same line
//   Priority 3: dumpsys SurfaceFlinger --latency (AOSP official)
//               - First line: vsync period in nanoseconds
//               - refresh_rate = 1e9 / period_ns
//   Priority 4: fallback to peak detection
//
// All non-existent fields (mCurrentMode, mRequestedRefreshRate, currentRefreshRate,
// mRefreshRate) have been REMOVED.
static int aatpeDetectCurrentRefreshRate() {
    // Problem 7: Priority 1: sysfs DRM (direct file read, no dumpsys)
    {
        int r = aatpeReadSysfsCurrentRefreshRate();
        if (r >= refresh::kMinRate && r <= refresh::kMaxRate) {
            FRAME_LOG_DEBUG("Current refresh rate (sysfs DRM): %dHz", r);
            return r;
        }
    }
    // --- Priority 2: dumpsys SurfaceFlinger (3 AOSP-verified fields) ---
    {
        std::string sf = aatpeExec("dumpsys SurfaceFlinger 2>/dev/null", "VSYNC_CUR_SF");
        if (!sf.empty()) {
            int r = aatpeParseCurrentFromSurfaceFlinger(sf);
            if (r >= refresh::kMinRate && r <= refresh::kMaxRate) {
                FRAME_LOG_DEBUG("Current refresh rate (SurfaceFlinger AOSP): %dHz", r);
                return r;
            }
        }
    }

    // --- Priority 2: dumpsys display mBaseDisplayInfo (AOSP DisplayDeviceInfo) ---
    {
        std::string o = aatpeExec("dumpsys display 2>/dev/null", "VSYNC_CUR_DISP");
        if (!o.empty()) {
            int r = aatpeParseCurrentFromDisplayBaseInfo(o);
            if (r >= refresh::kMinRate && r <= refresh::kMaxRate) {
                FRAME_LOG_DEBUG("Current refresh rate (mBaseDisplayInfo AOSP): %dHz", r);
                return r;
            }
        }
    }

    // --- Priority 3: dumpsys SurfaceFlinger --latency (AOSP official) ---
    {
        std::string lat = aatpeExec("dumpsys SurfaceFlinger --latency 2>/dev/null", "VSYNC_CUR_LAT");
        if (!lat.empty()) {
            int r = aatpeParseRefreshFromLatency(lat);
            if (r >= refresh::kMinRate && r <= refresh::kMaxRate) {
                FRAME_LOG_DEBUG("Current refresh rate (--latency AOSP): %dHz", r);
                return r;
            }
        }
    }

    // --- Priority 4: fallback to peak detection ---
    FRAME_LOG_DEBUG("Current refresh rate unavailable (all AOSP sources failed), falling back to peak");
    return aatpeDetectPeakRefreshRate();
}

// Main refresh rate detection dispatcher with 3 modes
//   peak   - take max from all sources (existing behavior, default)
//   current- take actual current refresh rate from dumpsys display
//   auto   - game foreground → peak, non-game foreground → current
// Cache is invalidated when mode changes.
// #28: forward-declared dirty flag (defined in the frameaware namespace at the
// bottom of this file); set by the inotify display-mode watcher.
namespace aatpe { namespace frameaware { extern std::atomic<bool> g_refreshRateDirty; } }
// ROUND-007c: forward declaration of the sustained-jank boost vote flag.
namespace aatpe { namespace frameaware { extern std::atomic<bool> g_jankBoostVote; } }
namespace aatpe { namespace frameaware { extern std::atomic<bool> g_appFsJankActive; } }  // RC3
namespace aatpe { namespace frameaware { extern std::atomic<bool> g_frameMissJankInput; } }  // P2-fix
namespace aatpe { namespace frameaware { bool aatpeFusionJankVote(bool, int&, int&, int, int, bool); } }  // RC3 pure
int aatpeDetectRefreshRate() {
    Config cfg; snapshotConfig(cfg);
    static int s_cachedRate = 0;
    static time_t s_cacheTime = 0;
    static std::string s_lastMode = "";
    time_t now = time(nullptr);
    const std::string mode = cfg.refreshRateMode;
    // protect static cache variables with mutex.
    // Lock only for cache check/write, not for expensive detection calls.
    {
        std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
        // Invalidate cache if mode changed
        if (s_lastMode != mode) {
            s_cachedRate = 0;
            s_cacheTime = 0;
            s_lastMode = mode;
            FRAME_LOG_INFO("Refresh rate mode changed to '%s', cache cleared", mode.c_str());
        }
        int cacheTtl = (g_refreshRateModeEnum == RefreshRateMode::CURRENT) ? refresh::kCurrentCacheTtl : refresh::kPeakCacheTtl;
        // #28: inotify event fired — bypass the cache TTL and re-detect now.
        bool dirty = aatpe::frameaware::g_refreshRateDirty.exchange(false, std::memory_order_relaxed);
        // under a high-load storm the drm display nodes fire inotify
        // 2-3x/sec (every frame / commit). Honoring every event forced an
        // expensive re-detect + log each time. Rate-limit forced re-detects to
        // at most once every 5s; anything more frequent just keeps the cached
        // rate (it cannot meaningfully change that fast).
        if (dirty) {
            static std::atomic<long long> s_lastForcedRedetectMs{0};
            long long nowRdMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            long long prevRd = s_lastForcedRedetectMs.load(std::memory_order_relaxed);
            if (prevRd != 0 && (nowRdMs - prevRd) < 5000) {
                dirty = false;   // too soon — keep cache
            } else {
                s_lastForcedRedetectMs.store(nowRdMs, std::memory_order_relaxed);
            }
        }
        if (!dirty && s_cachedRate >= refresh::kMinRate && s_cachedRate <= refresh::kMaxRate && (now - s_cacheTime) < cacheTtl) {
            return s_cachedRate;
        }
        if (dirty) FRAME_LOG_INFO("Refresh-rate event: cache invalidated, re-detecting");
    }
    int best = 0;
    if (g_refreshRateModeEnum == RefreshRateMode::CURRENT) {
        best = aatpeDetectCurrentRefreshRate();
    } else if (g_refreshRateModeEnum == RefreshRateMode::AUTO) {
        if (detectGameProcess()) {
            FRAME_LOG_DEBUG("Auto mode: game foreground detected, using peak");
            best = aatpeDetectPeakRefreshRate();
        } else {
            FRAME_LOG_DEBUG("Auto mode: non-game foreground, using current");
            best = aatpeDetectCurrentRefreshRate();
        }
    } else {
        best = aatpeDetectPeakRefreshRate();
    }
    if (best == 0) best = refresh::kDefaultRate;
    // write cache under mutex
    {
        std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
        s_cachedRate = best;
        s_cacheTime = now;
    }
    return best;
}

// SF latency with caching (optimization: cache for 3 seconds)
// P0-2: outValid indicates whether real SF latency data was obtained.
// When only fallback (theoretical frame period) is available, caller must
// NOT trigger frame-critical based on latency, because baseUs is not actual
// rendering latency but the vsync period.
int aatpeDetectSFLatency(int refreshRate, int& outMaxLat, bool& outValid) {
    outValid = false;
    static int s_cachedAvg = 0;
    static int s_cachedMax = 0;
    static bool s_cachedValid = false;
    static time_t s_cacheTime = 0;
    time_t now = time(nullptr);
    // protect static cache with mutex (check only)
    {
        std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
        // SF latency cache from 3s to 1s for faster jank detection
        if (s_cachedAvg > 0 && (now - s_cacheTime) < aatpe::frame_latency::kSfLatCacheTtlSec) {
            outMaxLat = s_cachedMax;
            outValid = s_cachedValid;
            return s_cachedAvg;
        }
    }
    // === BINDER DIRECT PATH (priority 1) ===
    // Try Binder direct SurfaceFlinger --latency first.
    // No fork, no shell, no dumpsys. Cached internally for 500ms.
    {
        // D: bounded exponential backoff retry for binder init.
        // Starts at 5s, doubles each failure, caps at 30s.
        static long long s_lastBinderTryMs = 0;
        static int s_binderRetryCount = 0;
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!aatpe::frame_binder::isAlive()) {
            long long backoffMs = 5000LL << std::min(s_binderRetryCount, 3);  // 5s→10s→20s→40s capped 30s
            if (backoffMs > 30000) backoffMs = 30000;
            if (nowMs - s_lastBinderTryMs >= backoffMs) {
                s_lastBinderTryMs = nowMs;
                s_binderRetryCount++;
                if (aatpe::frame_binder::init()) {
                    FRAME_LOG_INFO("Binder SurfaceFlinger path initialized on retry #%d", s_binderRetryCount);
                    s_binderRetryCount = 0;
                } else {
                    FRAME_LOG_DEBUG("Binder init failed (retry #%d, next in %lldms)",
                              s_binderRetryCount, backoffMs);
                }
            }
        }
        if (aatpe::frame_binder::isAlive()) {
            if (aatpe::frame_binder::refresh()) {
                auto stats = aatpe::frame_binder::getStats();
                if (stats.valid && stats.averageFrameTimeUs > 0) {
                    int avgLat = (int)stats.averageFrameTimeUs;
                    int maxLat = (stats.p99FrameTimeUs > 0)
                        ? (int)stats.p99FrameTimeUs
                        : (int)(avgLat * 2.0);
                    // Clamp to sane range.
                    // F-01: this is the GLOBAL (no-layer) SF latency. It tracks the
                    // ViewRoot/HWUI main layer and on a live SurfaceView game lands in
                    // 5-15ms, so outValid is almost always true. It feeds ONLY the jank /
                    // frame-critical health votes below; it must NOT be used to skip the
                    // per-layer game FPS sampler. readSurfaceFlinger() now samples the
                    // game layer whenever fgIsGame, regardless of this outValid.
                    if (avgLat >= 1000 && avgLat <= 100000) {
                        outMaxLat = maxLat;
                        outValid = true;
                        FRAME_LOG_DEBUG("SF latency (Binder direct): avg=%dus max=%dus p95=%.0f p99=%.0f jank=%lld n=%d",
                                        avgLat, maxLat, stats.p95FrameTimeUs, stats.p99FrameTimeUs,
                                        stats.jankCount, stats.sampleCount);
                        {
                            std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
                            s_cachedAvg = avgLat;
                            s_cachedMax = maxLat;
                            s_cachedValid = true;
                            s_cacheTime = now;
                        }
                        return avgLat;
                    }
                }
            }
        }
    }
    // === P1-8: cheap sysfs frame_timeline probe (before forking dumpsys) ===
    // Some kernels expose /sys/kernel/debug/surfaceflinger/frame_timeline with
    // recent present timestamps. Reading a sysfs node costs no fork / binder, so
    // it is strictly cheaper than `dumpsys SurfaceFlinger --latency`. On MTK
    // 4.14 this node usually does not exist; when it is absent or unparseable we
    // silently fall through to the dumpsys path below.
    if (!outValid) {
        static const char* kTimelineNodes[] = {
            "/sys/kernel/debug/surfaceflinger/frame_timeline",
            "/sys/kernel/debug/surfaceflinger/FrameTimeline",
        };
        for (const char* node : kTimelineNodes) {
            std::string raw = readFile(node);
            if (raw.empty()) continue;
            // Parse all integer tokens; treat consecutive deltas as frame gaps.
            std::vector<long long> ts;
            std::istringstream iss(raw);
            long long v;
            while (iss >> v) ts.push_back(v);
            if (ts.size() >= 4) {
                std::vector<long long> gaps;
                for (size_t i = 1; i < ts.size(); ++i) {
                    long long g = ts[i] - ts[i-1];
                    if (g > 0 && g < 200000000LL) gaps.push_back(g);  // <200ms gap
                }
                if (!gaps.empty()) {
                    long long sum = 0;
                    for (long long g : gaps) sum += g;
                    long long avgNs = sum / (long long)gaps.size();
                    int avgUs = (int)(avgNs / 1000);
                    if (avgUs >= 1000 && avgUs <= 100000) {
                        outMaxLat = avgUs * 2;
                        outValid = true;
                        {
                            std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
                            s_cachedAvg = avgUs;
                            s_cachedMax = outMaxLat;
                            s_cachedValid = true;
                            s_cacheTime = now;
                        }
                        FRAME_LOG_DEBUG("SF latency (frame_timeline sysfs): avg=%dus", avgUs);
                        return avgUs;
                    }
                }
            }
            break;  // only try the first node that opened
        }
    }
    int avgLat = 0, maxLat = 0;
    const int LAT_MIN_US = 1000;
    const int LAT_MAX_US = 100000;
    static int s_cachedMinIntervalUs = 0;
    int baseUs = 0;
    if (refreshRate >= refresh::kMinRate && refreshRate <= refresh::kMaxRate) baseUs = refresh::kOneSecondUs / refreshRate;
    {
        std::string raw = aatpeExec("dumpsys SurfaceFlinger --latency 2>/dev/null", "SF_LAT_L1");
        if (!raw.empty()) {
            long long minIntervalNs = 0;
            int threeColLines = 0;
            std::vector<long long> actualLatUs;
            std::istringstream iss(raw);
            std::string ln;
            while (std::getline(iss, ln)) {
                trim(ln);
                if (ln.empty()) continue;
                long long v[3];
                int n = sscanf(ln.c_str(), "%lld %lld %lld", &v[0], &v[1], &v[2]);
                if (n == 3) {
                    threeColLines++;
                    if (v[2] >= refresh::kMinVsyncPeriodNs && v[2] <= refresh::kMaxVsyncPeriodNs) {
                        if (minIntervalNs == 0 || v[2] < minIntervalNs) minIntervalNs = v[2];
                    }
                    // A3: guard the latency subtraction against malformed/extreme
                    // timestamps (negative values, wrap-around, or pathological deltas)
                    // before computing v[1]-v[0]. Real SF gfxinfo timestamps are
                    // uptime-nanoseconds, non-negative and within ~years; a single
                    // frame latency is at most tens of ms. Reject anything outside.
                    const long long kMaxGfxTsNs = (long long)1e18;
                    if (v[0] >= 0 && v[0] <= kMaxGfxTsNs &&
                        v[1] >= 0 && v[1] <= kMaxGfxTsNs) {
                        long long latNs = v[1] - v[0];
                        if (latNs >= 0 && latNs <= 60000000000LL) {
                            long long latUs = latNs / 1000;
                            if (latUs >= LAT_MIN_US && latUs <= LAT_MAX_US) actualLatUs.push_back(latUs);
                        }
                    }
                }
            }
            if (threeColLines > 0 && minIntervalNs > 0) s_cachedMinIntervalUs = (int)(minIntervalNs / 1000);
            // P0-2: real latency data obtained only when we have actual latency samples
            // P2-2: validate samples are NOT just vsync period echo (Android12+ dumpsys
            // SurfaceFlinger --latency often returns only the vsync period constant).
            // Valid = has samples AND has real variance (not all ≈ vsync period).
            if (!actualLatUs.empty()) {
                long long sum = 0;
                for (long long us : actualLatUs) sum += us;
                avgLat = (int)(sum / (long long)actualLatUs.size());
                long long mx = 0;
                for (long long us : actualLatUs) if (us > mx) mx = us;
                maxLat = (int)mx;
                // Check for real variance: at least one sample deviates >25% from mean.
                bool hasRealVariance = false;
                if (avgLat > 0) {
                    for (long long us : actualLatUs) {
                        double dev = std::abs((double)us - (double)avgLat) / (double)avgLat;
                        if (dev > 0.25) { hasRealVariance = true; break; }
                    }
                }
                // FIX(v4.3.1, RC3): the >25%-variance gate alone rejected STEADY-but-
                // over-budget lag — an app rendering at a flat 30/60fps on a 120Hz panel
                // has near-constant latency, so it was declared sfLatValid=false and
                // jankDetected was forced to 0 (the user's "very laggy but module silent").
                // Distinguish a REAL steady lag from the Android12+ "vsync period echo"
                // (which sits ~= the theoretical panel period): a mean latency comfortably
                // ABOVE the panel period is a genuine low-fps signal. Generic, no pkg names.
                double panelPeriodUs = (refreshRate >= refresh::kMinRate && refreshRate <= refresh::kMaxRate)
                                       ? (double)refresh::kOneSecondUs / (double)refreshRate
                                       : 16667.0;
                bool steadyOverBudget = (avgLat >= (long long)(panelPeriodUs * 1.5));
                outValid = hasRealVariance || steadyOverBudget;
            } else if (baseUs > 0) {
                // No actual latency samples; fall back to theoretical period (NOT valid latency)
                avgLat = baseUs; maxLat = baseUs * 2;
            } else if (s_cachedMinIntervalUs > 0) {
                avgLat = s_cachedMinIntervalUs; maxLat = avgLat * sf_latency::kMaxMultiplier;
            }
        }
    }
    // gfxinfo fallback for SF latency.
    // Root cause: dumpsys SurfaceFlinger --latency often returns only the vsync
    // period (1-column line) or empty output on modern Android (12+), leaving
    // actualLatUs empty and outValid=false permanently. When outValid is false,
    // readSurfaceFlinger() skips jank detection entirely (jankDetected stays 0).
    // Fix: parse `dumpsys gfxinfo <pkg>` frame percentiles (50th/95th/99th) as
    // a secondary latency source. gfxinfo is widely supported and provides real
    // frame timing data even when --latency is deprecated or empty.
    if (!outValid) {
        std::string fgPkg = aatpeGetForegroundPackage();
        // sink-side whitelist defence. g_foregroundApp may originate
        // from channels that do NOT go through aatpeExtractPackage():
        //   - Layer 1 Helper-APK file (/dev/aatpe/state/foreground_app),
        //   - Layer 2 sysfs/cgroup cmdline read,
        //   - Layer 3 /proc oom-scan cmdline read.
        // None of those are guaranteed to be a well-formed package name. Since
        // the string below is concatenated into a /system/bin/sh -c command,
        // we re-validate here as defence-in-depth. On failure we simply skip
        // the gfxinfo fallback and fall through to the next latency source
        // (line ~995: dumpsys SurfaceFlinger refresh|vsync|fps). This does NOT
        // change foreground-app detection; it only refuses to run a shell
        // command with an untrusted argument.
        bool pkgOk = !fgPkg.empty() && fgPkg != "unknown" && aatpeIsValidPackageName(fgPkg);
        if (pkgOk) {
            std::string gfxCmd = "dumpsys gfxinfo " + fgPkg + " 2>/dev/null";
            std::string gfxOut = aatpeExec(gfxCmd.c_str(), "SF_LAT_GFX");
            if (!gfxOut.empty()) {
                int p50 = 0, p95 = 0, p99 = 0;
                std::istringstream giss(gfxOut);
                std::string gline;
                while (std::getline(giss, gline)) {
                    trim(gline);
                    // Parse "50th percentile: 8ms", "95th percentile: 16ms", etc.
                    if (gline.find("percentile:") != std::string::npos) {
                        int valMs = 0;
                        // Extract integer after "percentile:"
                        size_t colonPos = gline.find(':');
                        if (colonPos != std::string::npos) {
                            std::string numPart = gline.substr(colonPos + 1);
                            trim(numPart);
                            // numPart like "8ms" or "16.5ms"
                            valMs = (int)aatpeFirstFloat(numPart);
                        }
                        if (gline.rfind("50th", 0) == 0) p50 = valMs;
                        else if (gline.rfind("95th", 0) == 0) p95 = valMs;
                        else if (gline.rfind("99th", 0) == 0) p99 = valMs;
                    }
                }
                if (p50 > 0) {
                    avgLat = p50 * 1000;  // ms -> us
                    maxLat = (p99 > 0) ? p99 * 1000 : ((p95 > 0) ? p95 * 1000 : avgLat * 2);
                    if (avgLat >= LAT_MIN_US && avgLat <= LAT_MAX_US) {
                        outValid = true;
                        FRAME_LOG_DEBUG("SF latency (gfxinfo fallback): avg=%dus max=%dus (p50=%d p95=%d p99=%d)",
                                  avgLat, maxLat, p50, p95, p99);
                    }
                }
            }
        }
    }
    if (avgLat == 0) {
        std::string o = aatpeExec("dumpsys SurfaceFlinger 2>/dev/null | grep -i -E 'refresh|vsync|fps' | head -n 5", "SF_LAT_L2");
        int rr = aatpeFirstInt(o);
        if (rr >= refresh::kMinRate && rr <= refresh::kMaxRate) { avgLat = refresh::kOneSecondUs / rr; maxLat = avgLat * sf_latency::kMaxMultiplier; }
    }
    if (avgLat == 0) {
        int rr = (refreshRate > 0) ? refreshRate : refresh::kDefaultRate;
        avgLat = refresh::kOneSecondUs / rr;
        maxLat = avgLat * sf_latency::kMaxMultiplier;
    }
    if (avgLat < LAT_MIN_US || avgLat > LAT_MAX_US) {
        int rr = (refreshRate > 0) ? refreshRate : refresh::kDefaultRate;
        avgLat = refresh::kOneSecondUs / rr;
        // P3-10: when avgLat is recomputed here, maxLat must be recomputed too,
        // otherwise maxLat still holds the stale out-of-range value from the
        // earlier branches and the next clamp (below) snaps it to avgLat,
        // losing the kMaxMultiplier headroom.
        maxLat = avgLat * sf_latency::kMaxMultiplier;
    }
    if (maxLat < LAT_MIN_US || maxLat > LAT_MAX_US * 2) maxLat = avgLat;
    // write cache under mutex
    {
        std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
        s_cachedAvg = avgLat;
        s_cachedMax = maxLat;
        s_cachedValid = outValid;
        s_cacheTime = now;
    }
    outMaxLat = maxLat;
    return avgLat;
}

struct SfLatencyResult { int miss; int samples; float intervalMs; float hz; bool available; double renderFps = 0.0; };
static SfLatencyResult parseSfLatency(const std::string& out) {
    SfLatencyResult r{0, 0, 0.0f, 0.0f, false};
    if (out.empty()) return r;
    long long periodNs = 0;
    { std::istringstream h(out); h >> periodNs; }
    // T2: reject implausible periods. Valid vsync periods are 5ms..200ns^-1 of
    // 5..200 ms ([5e6, 2e8] ns). An out-of-bound first line means a truncated /
    // malformed dump — do NOT trust it as refresh period (old code trusted any
    // positive value, producing absurd hz).
    if (periodNs <= 0 || periodNs < 5000000LL || periodNs > 200000000LL) return r;
    r.available = true;
    r.hz = 1e9f / (float)periodNs;
    r.intervalMs = (float)periodNs / 1e6f;
    std::istringstream ss(out);
    std::string line; std::getline(ss, line);
    // v6.0.0 round4b: instantaneous RENDER fps from the actual-present column over
    // the 128-row ring. SurfaceFlinger --latency gives a fixed-size ring (~128
    // rows = ~1s at 120Hz); the header period is the PANEL refresh, NOT the app's
    // real render rate (a game at 80-90fps on a 120Hz panel must show ~85, not 120).
    // Container / ActivityRecord layers return all "0 0 0" rows (skipped below).
    // INT64_MAX (9223372036854775807) in a column means "not presented" and must not
    // count as a present timestamp.
    const long long kInt64Max = 9223372036854775807LL;
    long long firstPresent = 0, lastPresent = 0;
    int presentedFrames = 0;
    while (std::getline(ss, line)) {
        long long a = 0, b = 0, c = 0;
        int ncol = sscanf(line.c_str(), "%lld %lld %lld", &a, &b, &c);
        if (ncol != 2 && ncol != 3) continue;
        if (a == -32768 || b == -32768) continue;
        const long long kTsMaxNs = 2000000000000LL;   // 2e12 ns
        // The frame "end" timestamp is the actual-present column: c for a 3-column
        // row, b for a 2-column row. Skip rows with a zero / INT64_MAX / out-of-range
        // present stamp (all-zero container rows, never-present rows).
        long long end = (ncol == 3) ? c : b;
        if (ncol == 3 && (a == -32768 || c == -32768)) continue;
        if (end <= 0 || end >= kInt64Max || end > kTsMaxNs) continue;
        if (a <= 0 || a > kTsMaxNs) continue;
        r.samples++;
        // Present-timestamp span for render-fps.
        if (firstPresent == 0) firstPresent = end;
        lastPresent = end;
        presentedFrames++;
        // WS-C2 (R3): deadline miss = end - vsync(a) > one period.
        if ((end - a) > periodNs) r.miss++;
    }
    // render fps = (presented-1) intervals / span. Need >=2 frames and a sane span.
    if (presentedFrames >= 2 && lastPresent > firstPresent) {
        double spanSec = (double)(lastPresent - firstPresent) / 1e9;
        if (spanSec > 0.05) {
            r.renderFps = (double)(presentedFrames - 1) / spanSec;
            if (r.renderFps < 0.0) r.renderFps = 0.0;
        }
    }
    return r;
}

// ============================================================================
// WS-C2: game SurfaceView frame sampling (R3 research).
//
// For SurfaceView games, `dumpsys gfxinfo <pkg>` has no app frame rows and
// FrameTimeline does not support SurfaceView layers. The correct per-frame
// source is `dumpsys SurfaceFlinger --latency "<layerName>"` (or the binder
// DUMP_TRANSACTION equivalent). These helpers are PURE so the host test can
// drive them with fake --list / --latency text.
// ============================================================================
namespace aatpe {
namespace frameaware {

// v6.0.0 round5: latency (real present timestamps) is ground truth; timestats is
// fallback + cross-check. Tolerance relGap>0.20 OR absGap>15fps: large enough to
// ignore 128-ring window jitter on 120Hz, small enough to catch the real-device
// 101 vs 12.4 case (~88% gap). Fully dynamic — no fixed fps/layer/model.
double chooseForegroundFps(double latencyRenderFps, double latencyHz,
                          bool latencyAvail, double tsAvgFps, int tsActiveFrames,
                          int& outSource, bool& outSuspect) {
    bool latOk = latencyAvail && latencyRenderFps > 1.0;
    double latFps = latOk ? latencyRenderFps
                         : (latencyAvail && latencyHz > 1.0 ? latencyHz : 0.0);
    bool tsOk = tsActiveFrames >= 5 && tsAvgFps > 1.0;
    outSuspect = false;
    if (latOk || (latencyAvail && latFps > 1.0)) {
        outSource = 0;
        if (tsOk) {
            double relGap = std::fabs(latFps - tsAvgFps) / std::max(latFps, tsAvgFps);
            if (relGap > 0.20 || std::fabs(latFps - tsAvgFps) > 15.0) outSuspect = true;
        }
        return latFps;
    }
    if (tsOk) { outSource = 1; return tsAvgFps; }
    outSource = -1; return 0.0;
}

// ---------------------------------------------------------------------------
// F-01: game per-layer sampler gate.
//
// The GLOBAL binder `--latency` (no layer name) tracks the ViewRoot/HWUI main
// layer and returns a submission latency that on ANY live SurfaceView game lands in
// the 5-15ms band, so `sfLatValid` is almost permanently true. Using sfLatValid as
// the switch for the whole game SurfaceView sampler therefore SHORT-CIRCUITED the
// very channel that measures the game (王者 etc.): the game per-layer latency was
// never read, gfxinfo has no SurfaceView app rows, and 5s later the game pkg was
// aged into background_stale -> WebUI showed 0 / single-digit fps.
//
// Rule: the per-layer sampler runs whenever the foreground IS a game OR the global
// channel itself has no usable data. A game in foreground is NEVER gated away by a
// valid-looking global latency. (Host-tested.)
bool gameSamplingRequired(bool sfLatValid, bool fgIsGame) {
    return fgIsGame || !sfLatValid;
}

// PURE (host-tested): decide the GLOBAL-SF-latency frame-critical + jank alarms.
//
// aatpeDetectSFLatency()'s binder path reads the GLOBAL SurfaceFlinger latency,
// which tracks the ViewRoot/HWUI main layer and is "almost always valid" even on
// an idle launcher/SystemUI. Comparing that number against the PANEL frame budget
// only means something for a performance-sensitive foreground surface (a game):
// a launcher rendering at ~60fps legitimately reads ~13ms on a 120Hz panel
// (budget 8.33ms) and must NOT latch frame_critical / jank. `alarmAllowed` is the
// foreground-is-a-game gate; when false we stay silent. When true the math below
// is byte-for-byte the legacy decision (no behavior change for real games).
struct FrameLatencyAlarm { bool latCritical; bool jank; };
FrameLatencyAlarm aatpeFrameLatencyAlarm(
        bool sfLatValid, int avgLatUs, int maxLatUs, int effectiveThresholdUs,
        int frameTimeUs, float jankThresholdPct, bool alarmAllowed) {
    FrameLatencyAlarm r{false, false};
    if (!alarmAllowed) return r;                 // no foreground game -> never alarm on global SF
    r.latCritical = sfLatValid && (avgLatUs > effectiveThresholdUs);
    if (sfLatValid && jankThresholdPct > 0.0f) {
        int jankThresholdUs = (int)(frameTimeUs * (1.0f + jankThresholdPct / 100.0f));
        r.jank = (avgLatUs > jankThresholdUs) ||
                 (maxLatUs > jankThresholdUs *
                  aatpe::frame_latency::kJankMaxMultiplierNum /
                  aatpe::frame_latency::kJankMaxMultiplierDen);
    }
    return r;
}

// F-04/F-06: honest game-fps resolution from real measurements.
//   renderFps : measured present rate over the latency ring (0 when <2 present
//               frames / sparse — layer-switch warmup, static frame).
//   layerHz   : the selected layer's vsync period == PANEL refresh. It is ONLY a
//               target-hint; it must NEVER be shown as the measured fps (the old
//               code fell back to it and displayed a fake 120fps while the game
//               was at single digits).
//   gedFps    : MTK GED composer primary_fps (0 = node absent). On Dimensity this
//               is the lowest-overhead, most-trustworthy "actual composite fps".
//
// Precedence: measured present-span fps > GED composer fps > DEGRADED.
// On DEGRADED we return fps=0 + degraded=true so the caller holds the last good
// display and labels the channel "degraded", rather than fabricating the panel hz.
struct GameFpsResolve {
    double fps = 0.0;
    const char* source = "degraded";
    bool degraded = true;
};
GameFpsResolve resolveGameFps(double renderFps, double /*layerHz*/, int gedFps,
                              const char* measuredSource) {
    GameFpsResolve r;
    if (renderFps > 1.0) {
        r.fps = renderFps;
        r.source = (measuredSource && *measuredSource) ? measuredSource : "latency";
        r.degraded = false;
        return r;
    }
    // F-06: per-layer present span too sparse -> fall back to the MTK composer node.
    if (gedFps >= 1 && gedFps <= 240) {
        r.fps = (double)gedFps;
        r.source = "ged_composer";
        r.degraded = false;
        return r;
    }
    // F-04: no trustworthy measurement. layerHz intentionally NOT used as fps.
    r.fps = 0.0;
    r.source = "degraded";
    r.degraded = true;
    return r;
}

std::vector<std::string> sfListLayers(const std::string& listOut) {
    std::vector<std::string> layers;
    std::istringstream iss(listOut);
    std::string ln;
    while (std::getline(iss, ln)) {
        trim(ln);
        if (ln.empty()) continue;
        // `dumpsys SurfaceFlinger --list` prints "LayerName" or
        // "LayerName (???)" etc. Skip hex ids / pure-digit noise and the same
        // metadata lines the layer-count parser already filters.
        if (ln.size() >= 2 && ln[0] == '0' && (ln[1] == 'x' || ln[1] == 'X')) continue;
        bool allDigits = true;
        for (char ch : ln) { if (!(ch >= '0' && ch <= '9')) { allDigits = false; break; } }
        if (allDigits) continue;
        const char* skipKw[] = {"Region", "Buffer", "HWC", "name=", "type=", "size=", "format="};
        bool skip = false;
        for (const char* kw : skipKw) {
            if (ln.find(kw) != std::string::npos) { skip = true; break; }
        }
        if (skip) continue;
        layers.push_back(ln);
    }
    return layers;
}

// Extract "com.pkg/com.pkg.Activity" from a layer name of the form
//   SurfaceView[com.pkg/com.pkg.SomeActivity]@0(BLAST)#132833
// Returns empty when there is no [...] bracket.
std::string extractBracketedPkg(const std::string& layer) {
    auto l = layer.find('[');
    auto r = layer.find(']');
    if (l == std::string::npos || r == std::string::npos || r <= l) return "";
    return layer.substr(l + 1, r - l - 1);
}

// v4.2.1: normalize a "core token" (the pkg/act portion of a layer name).
// Strips trailing "#nnn", "(BLAST)"/"(...)", "@0", and surrounding whitespace.
//   "com.pkg/com.pkg.Act#0"      -> "com.pkg/com.pkg.Act"
//   "com.pkg/com.pkg.Act(BLAST)" -> "com.pkg/com.pkg.Act"
static std::string normalizeCore(std::string core) {
    trim(core);
    // drop trailing parenthesized markers, e.g. (BLAST) / (???).
    size_t rp;
    while ((rp = core.rfind(')')) != std::string::npos) {
        size_t lp = core.rfind('(', rp);
        if (lp == std::string::npos) break;
        core.erase(lp, rp - lp + 1);
    }
    trim(core);
    // drop "#nnn" window/buffer index suffix (from the LAST '#' to end).
    size_t h = core.rfind('#');
    if (h != std::string::npos) core.erase(h);
    trim(core);
    // drop trailing "@0" generation marker.
    if (!core.empty() && core.back() == '@') core.pop_back();
    trim(core);
    return core;
}

// v4.2.1: extract the pkg/act "core" from ANY supported --list line format:
//   (a) SurfaceView[com.pkg/com.pkg.Act]@0(BLAST)#n   -> bracket content
//   (b) SurfaceView[com.pkg]@0(BLAST)#n               -> bracket content (pkg only)
//   (c) SurfaceView - com.pkg/com.pkg.Act#n           -> text after " - "
//   (d) com.pkg/com.pkg.Act#n                         -> whole line (bare MTK form)
// Returns "" when nothing pkg-like can be extracted.
static std::string extractLayerCore(const std::string& layer) {
    // (a)/(b): bracket form wins (most precise).
    std::string br = extractBracketedPkg(layer);
    if (!br.empty()) return normalizeCore(br);
    // (c) "ViewLabel - pkg/act" legacy dash form. Use the LAST " - " so a pkg
    //     itself containing dashes (rare) is not split early.
    auto dash = layer.rfind(" - ");
    if (dash != std::string::npos) {
        std::string tail = layer.substr(dash + 3);
        // only treat as dash-form when the tail looks pkg-like (has '/' or a dot).
        if (tail.find('/') != std::string::npos || tail.find('.') != std::string::npos)
            return normalizeCore(tail);
    }
    // (d) bare form: the whole line is the core. Strip a leading view-type label
    //     if present (e.g. "SurfaceView" with no bracket/dash — treat as whole).
    return normalizeCore(layer);
}

// v4.2.1: does a normalized core reference fgPkg?
//   "pkg/act"       -> leading segment == fgPkg
//   "pkg"           -> == fgPkg (bracket-only-package)
//   "pkg.Activity"  -> starts with fgPkg + "."
bool layerReferencesPkg(const std::string& core, const std::string& fgPkg) {
    if (core.empty() || fgPkg.empty()) return false;
    auto slash = core.find('/');
    if (slash != std::string::npos) {
        return core.substr(0, slash) == fgPkg;
    }
    // single component: exact pkg, or "<pkg>.<Activity>".
    if (core == fgPkg) return true;
    return core.size() > fgPkg.size() &&
           core.compare(0, fgPkg.size(), fgPkg) == 0 &&
           core[fgPkg.size()] == '.';
}

// True when the (bracketed) "pkg/pkg.Activity" looks like a known game engine.
// v4.2.1: promoted from a HARD gate to a strong RANKING signal.
bool isGameEngineClass(const std::string& core) {
    static const char* eng[] = {
        "com.unity3d", "com.Unity3D", "UnityActivity", "UnityPlayerActivity",
        "com.epicgames.ue4.GameActivity", "GameActivity", "com.unity", "UnityMain",
        "org.cocos2dx", "com.cocos", "Cocos2dx", "Cocos",
        "com.badlogic.gdx", "GdxActivity", "AndroidGraphics",
        "android.opengl.GLSurfaceView", "GLSurfaceView",
    };
    for (const char* e : eng) {
        if (core.find(e) != std::string::npos) return true;
    }
    return false;
}

// Blacklist: non-game SurfaceView consumers that must never be selected even
// if their layer name contains "SurfaceView"/"GameView" substrings.
static bool isBlacklistedLayerConsumer(const std::string& layer) {
    static const char* bl[] = {
        "InputMethod", "IME", "inputmethod",
        "Camera", "camera",
        "ExoPlayer", "MediaCodec", "VideoView", "Video",
        "AdView", "AdWebView", "AdContainer", "gdt", "TTAd", "ocean", "mraid",
    };
    for (const char* b : bl) {
        if (layer.find(b) != std::string::npos) return true;
    }
    return false;
}

// v4.2.1: does the raw layer line look like a render surface (vs. a plain
// status bar / navigation / HWUI window)? Used as a ranking / sanity hint.
static bool isSurfaceLike(const std::string& layer) {
    return layer.find("SurfaceView") != std::string::npos ||
           layer.find("SurfaceTexture") != std::string::npos ||
           layer.find("TextureView") != std::string::npos ||
           layer.find("(BLAST)") != std::string::npos;
}

// v4.2.1: trim a candidate layer name for the diagnostic key (control chars
// removed, truncated) so it can never inject / bloat the state file.
static std::string sanitizeCandidate(std::string s) {
    std::string out;
    for (char c : s) {
        if (c >= 0x20 && c < 0x7f) out.push_back(c);   // printable ASCII only
        if (out.size() >= 96) break;
    }
    return out;
}

// v4.2.1 relaxed, multi-format layer selection.
//
// Old AND-gate (bracket pkg==fgPkg AND engine activity) rejected the vast
// majority of real device layer names: MIUI/MTK ship the game SurfaceView as
//   "SurfaceView - pkg/act#0" / bare "pkg/act#0" / "[pkg]" with no engine class,
//   so 王者荣耀 (SGameActivity, Unity engine but NOT in the old keyword list)
//   fell through to no_game_layer forever.
//
// New rule: any NON-blacklisted layer whose package EXACTLY matches the
// foreground package is a candidate. Among candidates we rank:
//   +100  known game-engine class (Unity/Unreal/Cocos/libGDX/GLSurfaceView)
//   +10   SurfaceView/SurfaceTexture/TextureView render surface
//   +5    (BLAST) buffer queue
// The top-ranked candidate wins. forceGame (manual list) guarantees we never
// additionally require an engine/surface signal.
std::string selectGameLayer(const std::vector<std::string>& layers,
                            const std::string& fgPkg,
                            bool forceGame,
                            std::string* outReason,
                            std::string* outCandidates) {
    auto setReason = [&](const char* r) { if (outReason) *outReason = r; };
    if (fgPkg.empty() || fgPkg == "unknown") { setReason("no_fg"); return ""; }

    std::string best;
    int bestScore = -1;
    std::string candList;
    int candCount = 0;
    for (const auto& ly : layers) {
        if (isBlacklistedLayerConsumer(ly)) continue;
        std::string core = extractLayerCore(ly);
        if (!layerReferencesPkg(core, fgPkg)) continue;

        // diagnostic: record up to 8 raw layer names referencing this pkg.
        if (candCount < 8) {
            if (candCount) candList += " | ";
            candList += sanitizeCandidate(ly);
            candCount++;
        }

        int score = 0;
        if (isGameEngineClass(core)) score += 100;
        if (isSurfaceLike(ly)) score += 10;
        if (ly.find("(BLAST)") != std::string::npos) score += 5;
        // forceGame: a manual-list hit is always a valid game surface even if it
        // scores 0 (e.g. a bare "pkg/act#n" with no surface keyword).
        if (forceGame && score == 0) score = 1;

        if (score > bestScore) { bestScore = score; best = ly; }
    }

    if (outCandidates) *outCandidates = candList.empty() ? "none" : candList;

    if (best.empty()) {
        // Layers exist but none references fgPkg -> no render layer for this pkg.
        setReason("no_layer_for_pkg");
        return "";
    }
    setReason("");
    return best;
}

SfFrameSample sampleGameLayer(const std::string& listOut, const std::string& fgPkg,
                               const std::function<std::string(const std::string&)>& fetchLatency,
                               bool forceGame) {
    SfFrameSample out;
    std::vector<std::string> layers = sfListLayers(listOut);
    std::string reason, candidates;
    std::string layer = selectGameLayer(layers, fgPkg, forceGame, &reason, &candidates);
    out.candidates = candidates;

    if (layer.empty()) {
        out.frameUnavailable = true;
        if (reason == "no_fg") { out.reason = "no_fg"; }
        else if (layers.empty()) { out.reason = "sf_list_empty"; out.candidates = "none"; }
        else { out.reason = "no_layer_for_pkg"; }
        return out;
    }
    // v6.0.0 round4b: the highest-scored layer may be an ActivityRecord/container
    // that returns all "0 0 0" rows (no real present). Try it, and if it has no
    // valid frames, walk OTHER layers that reference fgPkg until one yields real
    // present rows (the actual content/SurfaceView layer).
    std::vector<std::string> tryOrder;
    tryOrder.push_back(layer);
    for (const auto& ly : layers) {
        if (ly == layer) continue;
        if (isBlacklistedLayerConsumer(ly)) continue;
        std::string core = extractLayerCore(ly);
        if (layerReferencesPkg(core, fgPkg)) tryOrder.push_back(ly);
    }
    for (const auto& ly : tryOrder) {
        out.chosenLayer = ly;
        std::string dump = fetchLatency(ly);
        SfLatencyResult r = parseSfLatency(dump);
        if (!r.available || r.samples == 0) continue;  // all-zero container -> next
        out.available = true;
        out.frameUnavailable = false;
        out.reason.clear();
        out.samples = r.samples;
        out.miss = r.miss;
        out.hz = r.hz;
        out.intervalMs = r.intervalMs;
        out.renderFps = r.renderFps;
        return out;
    }
    // Tried every candidate; none had valid present rows.
    out.frameUnavailable = true;
    out.reason = "no_valid_frames";
    return out;
}

// ============================================================================
// v4.2.1: manual game-override list + installed-apps reading.
//
// The auto game-layer recognizer (above) can miss engines it does not know, and
// on locked-down OEM builds `gameDetection` heuristics may never fire. Users can
// pin a package to ALWAYS be treated as a game for FRAME SAMPLING ONLY (no
// scheduling/cold-start side effects) by listing it in
//   /data/adb/aatpe/game_overrides.list
// one package per line, '#' comments allowed. The file lives in the module's
// upgrade-preserved data area (same tier as aatpe.conf) and is hot-reloaded by
// mtime — no daemon restart.
// ============================================================================

// Pure: is `s` a syntactically valid Android package name? [A-Za-z0-9_.],
// leading alpha, no leading/trailing '.', no "..". Shared by the file parser,
// the WebUI write path and the pm parser so all three reject the same shape.
static bool aatpeValidPkgCharName(const std::string& s) {
    if (s.empty() || s.size() > 256) return false;
    if (!(s[0] >= 'A' && s[0] <= 'Z') && !(s[0] >= 'a' && s[0] <= 'z')) return false;
    if (s.front() == '.' || s.back() == '.') return false;
    if (s.find("..") != std::string::npos) return false;
    for (char c : s) {
        if (isalnum((unsigned char)c)) continue;
        if (c == '.' || c == '_') continue;
        return false;
    }
    return true;
}

std::set<std::string> parseGameOverridesContent(const std::string& content) {
    std::set<std::string> out;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        // strip inline comments and surrounding whitespace.
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        trim(line);
        if (line.empty()) continue;
        // reject anything with shell/path metacharacters; accept only valid pkgs.
        if (aatpeValidPkgCharName(line)) out.insert(line);
    }
    return out;
}

std::vector<std::string> parsePmPackageList(const std::string& pmOutput) {
    // `pm list packages[-3]` / `cmd package list packages[-3]` print one of:
    //   package:com.foo
    //   package:com.foo  (with optional trailing whitespace / arch suffix)
    // We extract the token after "package:" up to whitespace. Malformed lines
    // (no "package:" prefix, invalid pkg) are skipped.
    std::vector<std::string> out;
    std::istringstream ss(pmOutput);
    std::string line;
    while (std::getline(ss, line)) {
        trim(line);
        const char* prefix = "package:";
        if (line.compare(0, 8, prefix) != 0) continue;
        std::string pkg = line.substr(8);
        trim(pkg);
        // drop optional trailing columns/whitespace (some builds append nothing,
        // some append "  arch"). trim already removed whitespace; take up to any
        // embedded space just in case.
        size_t sp = pkg.find_first_of(" \t");
        if (sp != std::string::npos) pkg.erase(sp);
        if (aatpeValidPkgCharName(pkg)) out.push_back(pkg);
    }
    return out;
}


// ============================================================================
// Timestats channel (ROUND-014 P3 / P2 research §9-14).
//
// `dumpsys SurfaceFlinger --timestats -dump` output block per layer:
//   Layer Name's timestats: SurfaceView[pkg/cls]@0(BLAST)#id
//     totalFrames = 120
//     averageFPS = 59.8
//     presentToPresent histogram:
//       8ms: 90
//      16ms: 20
//      33ms: 10
// NOTE: there is NO jankyFrames/missedPresent field (that is gfxinfo hwui
// ProfileData). We derive miss from the presentToPresent histogram:
//   missRate = Σ(bucket_ms > 1.5*periodMs) / totalFrames  (same口径 as T5).
// P90/P99: cumulative frames crossing total*0.90/0.99 -> 1000/bucket ms = fps.
// Pure function: host test drives it with fake dump text.
// (TimestatsResult struct defined in aatped.h.)
// Parse one layer block. Fills res when it has usable data.
// Snap an observed interval to the nearest standard refresh rate period.
// Returns the snapped target period in ms.
static double snapToStandardPeriod(double observedMs) {
    // F-1: cadence ladder extended to cover high-refresh panels. kMaxRate=240
    // (aatpe-constants.h), so 144Hz/165Hz/240Hz layer buckets must snap to their
    // TRUE period instead of being rounded down to 120Hz (which also forced the
    // jank threshold to 12.5ms instead of the real 1.5x-period).
    static const struct { double ms; double fps; } standards[] = {
        {33.33, 30.0},
        {16.67, 60.0},
        {11.11, 90.0},
        {8.33, 120.0},
        {6.94, 144.0},
        {6.06, 165.0},
        {4.17, 240.0},
    };
    double best = 16.67;
    double bestDiff = 1e9;
    for (auto& s : standards) {
        double diff = std::abs(observedMs - s.ms);
        if (diff < bestDiff) { bestDiff = diff; best = s.ms; }
    }
    // Only snap if within 20% of a standard rate; otherwise keep observed.
    if (bestDiff / best < 0.2) return best;
    return observedMs;
}

static void finishTimestatsBlock(const std::string& layer, long long totalFrames,
                                 double avgFpsFromDump,
                                 const std::vector<std::pair<double, long long>>& hist,
                                 TimestatsResult& res) {
    (void)avgFpsFromDump;  // v4.2.6: target/active FPS now derived from the histogram.
    if (totalFrames <= 0 || hist.empty()) return;   // no real data
    res.available = true;
    res.layer = layer;
    res.totalFrames = (int)totalFrames;

    // v4.2.6 SF game-layer authoritative metrics:
    // 1. Find mode bucket (most frames) → snap to standard refresh rate.
    long long modeCount = 0;
    double modeBucketMs = 16.67;
    for (auto& b : hist) {
        if (b.second > modeCount && b.first > 0.5) {
            modeCount = b.second;
            modeBucketMs = b.first;
        }
    }
    double targetPeriodMs = snapToStandardPeriod(modeBucketMs);
    double targetFps = 1000.0 / targetPeriodMs;
    res.targetFps = targetFps;

    // 2. Filter long gaps (>250ms or >5× target period) = background/loading/idle.
    // These are NOT counted in active frames or jank stats.
    double longGapThresholdMs = std::max(250.0, 5.0 * targetPeriodMs);
    long long activeFrames = 0;
    double activeWeightedSum = 0.0;
    long long jankyFrames = 0;
    for (auto& b : hist) {
        if (b.first > longGapThresholdMs) continue;  // skip long gaps
        activeFrames += b.second;
        activeWeightedSum += b.first * b.second;
        // Jank = active frame with interval >= 1.5× target period.
        if (b.first >= 1.5 * targetPeriodMs) {
            jankyFrames += b.second;
        }
    }
    res.activeFrames = (int)activeFrames;

    // 3. Active FPS = 1000 / weighted mean of active intervals.
    double activeAvgInterval = (activeFrames > 0) ? (activeWeightedSum / activeFrames) : 16.67;
    double activeFps = (activeAvgInterval > 0.01) ? (1000.0 / activeAvgInterval) : 0.0;
    res.avgFps = activeFps;

    // 4. Jank rate = janky frames / active frames.
    double jankRate = (activeFrames > 0) ? ((double)jankyFrames / (double)activeFrames) : 0.0;
    if (jankRate < 0.0) jankRate = 0.0;
    if (jankRate > 1.0) jankRate = 1.0;
    res.missRate = jankRate;
    // v4.2.6: on-cadence ratio = (active - janky) / active.
    res.onCadenceRatio = (activeFrames > 0) ? (1.0 - jankRate) : 0.0;

    // p90/p99 from active intervals.
    std::vector<std::pair<double, long long>> h;
    for (auto& b : hist) {
        if (b.first <= longGapThresholdMs) h.push_back(b);
    }
    std::sort(h.begin(), h.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto pct = [&](double p) -> double {
        long long target = (long long)(p * (double)activeFrames);
        long long cum = 0;
        for (auto& b : h) { cum += b.second; if (cum >= target && b.first > 0.0) return 1000.0 / b.first; }
        return activeFps;
    };
    res.p90Fps = pct(0.90);
    res.p99Fps = pct(0.99);
    // v6.0.0 round4b: carry the raw cumulative histogram so the caller can diff it
    // against the previous dump (instantaneous fps = Δframes / Δwallsec, not the
    // dilated cumulative weighted mean).
    res.hist = hist;
}

// F-01: parse block-by-block; only the block whose bracket package matches fgPkg
// is accumulated. Multi-layer dumpsys output (multi-window / post-switch residue)
// must NOT mix background layers into the foreground app's vote.
// v4.2.6: when multiple BLAST layers match the same package, pick the one with
// the most active (on-cadence) frames — the primary rendering surface.

// F-5: regression seam. Counts how many times the whole dump is streamed+parsed
// per processManualGameLayers() call. Before the fix this equaled the number of
// manual packages (each re-parsed the ≤1MB dump from scratch); after the fix it
// is exactly 1 regardless of manual-list size.
static long long s_timestatsParsePasses = 0;
long long aatpeDebugTimestatsParsePasses() { return s_timestatsParsePasses; }
void aatpeDebugTimestatsResetParseCounter() { s_timestatsParsePasses = 0; }

struct TimestatsBlockInfo {
    std::string pkg;       // package extracted from the layer bracket (before '/')
    TimestatsResult res;
};

// F-5: ONE single streaming pass over the whole dump. Every finished, usable
// layer block is returned tagged with its extracted package. Callers then group
// / select by package without re-streaming the dump.
static std::vector<TimestatsBlockInfo> parseTimestatsAllBlocks(const std::string& dump) {
    std::vector<TimestatsBlockInfo> out;
    if (dump.empty()) return out;
    s_timestatsParsePasses++;
    std::istringstream ss(dump);
    std::string line;
    std::string curLayer;
    long long curTotal = 0;
    double curAvg = 0.0;
    std::vector<std::pair<double, long long>> curHist;
    bool inBlock = false;
    auto flushBlock = [&]() {
        if (!inBlock) return;
        TimestatsResult candidate;
        finishTimestatsBlock(curLayer, curTotal, curAvg, curHist, candidate);
        if (!candidate.available) return;
        std::string br = extractBracketedPkg(curLayer);
        auto slash = br.find('/');
        std::string pkg = (slash != std::string::npos) ? br.substr(0, slash) : br;
        TimestatsBlockInfo bi;
        bi.pkg = pkg;
        bi.res = candidate;
        out.push_back(std::move(bi));
    };
    while (std::getline(ss, line)) {
        trim(line);
        if (line.empty()) continue;
        if (line.find("timestats:") != std::string::npos) {
            flushBlock();                 // close previous block
            auto pos = line.find(':');
            curLayer = (pos != std::string::npos) ? line.substr(pos + 1) : "";
            trim(curLayer);
            curTotal = 0; curAvg = 0.0; curHist.clear(); inBlock = true;
            continue;
        }
        if (!inBlock) continue;
        if (line.find("totalFrames") != std::string::npos) {
            long long v = 0;
            if (sscanf(line.c_str(), "totalFrames = %lld", &v) == 1) curTotal = v;
            continue;
        }
        if (line.find("averageFPS") != std::string::npos) {
            double v = 0.0;
            if (sscanf(line.c_str(), "averageFPS = %lf", &v) == 1) curAvg = v;
            continue;
        }
        double bucketMs = 0.0; long long cnt = 0;
        if (sscanf(line.c_str(), "%lfms : %lld", &bucketMs, &cnt) == 2 ||
            sscanf(line.c_str(), "%lfms: %lld", &bucketMs, &cnt) == 2) {
            curHist.push_back({bucketMs, cnt});
        }
    }
    flushBlock();   // final block
    return out;
}

// v4.2.6: pick the PRIMARY rendering surface from a set of same-package candidates.
// Selection rule:
//   1. Only consider layers whose target period snaps to a standard refresh rate
//      (30/60/90/120/144/165/240 fps). Filters static ViewRoot layers with absurdly
//      long intervals (e.g. 200ms UI layers).
//   2. Highest on-cadence ratio (most frames on-pace).
//   3. Tie-break: more active frames.
static TimestatsResult pickBestTimestatsCandidate(const std::vector<TimestatsResult>& candidates) {
    TimestatsResult res;
    if (candidates.empty()) return res;
    size_t best = 0;
    // Filter: only consider candidates with valid (snapped) target fps.
    // F-1: upper bound aligned to refresh::kMaxRate (240Hz). The old hard
    // ceiling of 130Hz silently REJECTED genuine 144/165/240Hz game layers
    // (falling back to the first, possibly-wrong candidate). The 25fps floor
    // is retained to filter static ViewRoot layers (~5fps / 200ms intervals).
    std::vector<size_t> valid;
    for (size_t i = 0; i < candidates.size(); i++) {
        double tf = candidates[i].targetFps;
        bool snaps = (tf >= 25.0 && tf <= (double)refresh::kMaxRate + 1e-6);
        if (snaps) valid.push_back(i);
    }
    if (valid.empty()) valid.push_back(0);  // fallback: use first if none snap
    for (size_t j = 1; j < valid.size(); j++) {
        size_t i = valid[j];
        size_t b = valid[best];
        if (candidates[i].onCadenceRatio > candidates[b].onCadenceRatio)
            best = i;
        else if (std::abs(candidates[i].onCadenceRatio - candidates[b].onCadenceRatio) < 0.05
                 && candidates[i].activeFrames > candidates[b].activeFrames)
            best = i;
    }
    res = candidates[best];
    return res;
}

TimestatsResult parseTimestatsBlock(const std::string& dump,
                                    const std::string& fgPkg) {
    bool wantFirst = (fgPkg.empty() || fgPkg == "unknown");
    std::vector<TimestatsResult> candidates;
    for (const auto& bi : parseTimestatsAllBlocks(dump)) {
        if (wantFirst || bi.pkg == fgPkg) candidates.push_back(bi.res);
    }
    return pickBestTimestatsCandidate(candidates);
}

// T4 (round6): layer-name safety is now "valid UTF-8, no control chars".
//
// Why we widened past ASCII-only: real SF content-layer names on non-ASCII
// locales contain multi-byte UTF-8 (e.g. `bin.mt.plus/l.ۨۘܰ#0`, bytes
// db a8 db 98 d9 b0). The old ASCII whitelist rejected those -> the dumpsys
// path bailed -> no real frames -> fell back to a wrong channel.
//
// Security: the layer is passed via aatpeExecArgv (fork + execvp, standalone
// argv element, NO shell, NO word expansion). Non-ASCII bytes therefore cannot
// inject shell metacharacters even in theory. This validator is defence-in-depth:
// (a) rejects C0 control bytes (0x00-0x1f incl \n \r \t \0) and DEL 0x7f so a
// forged layer name can't smuggle newlines into logs/argv; (b) rejects INVALID
// UTF-8 (lone continuation, overlong, truncated lead, 0xff/0xfe) so we never
// propagate malformed text. Accepts ASCII printable (0x20-0x7e) and well-formed
// multi-byte UTF-8 (CJK, Arabic diacritics, etc.). Length cap 512 BYTES
// (non-ASCII expands). Empty still rejected.
bool isLayerNameSafe(const std::string& s) {
    if (s.empty() || s.size() > 512) return false;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return false;          // C0 control / DEL
        if (c < 0x80) { ++i; continue; }                  // ASCII printable
        // multi-byte lead: expect k continuation bytes 10xxxxxx
        int need = 0; unsigned int minv = 0;
        if      (c <= 0xBF) return false;                 // lone continuation 10xxxxxx
        else if (c < 0xE0) { need = 1; minv = 0x80;  }   // 2-byte
        else if (c < 0xF0) { need = 2; minv = 0x800; }   // 3-byte
        else if (c < 0xF5) { need = 3; minv = 0x10000; } // 4-byte
        else return false;                                // 0xF5-0xFF incl 0xff/0xfe
        if (i + need >= s.size()) return false;           // truncated
        unsigned long cp = c & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
        for (int k = 1; k <= need; ++k) {
            unsigned char d = (unsigned char)s[i + k];
            if (d < 0x80 || d > 0xBF) return false;       // bad continuation
            cp = (cp << 6) | (d & 0x3F);
        }
        if (cp < minv) return false;                      // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;   // UTF-16 surrogate
        if (cp > 0x10FFFF) return false;                  // beyond Unicode
        i += need + 1;
    }
    return true;
}

// Timestats session state machine methods (struct declared in aatped.h).
// Lifecycle: -clear -enable (once per session) -> wait warmup -> -dump each window
// -> -disable at session end. Channel failure/unavailable -> 5min TTL (no repeat
// fork). Multi-instance safe: we never leave -clear enabled on others; we only
// -disable our own session.
// v4.1.1 final-round: classify WHY a timestats -dump produced no usable result,
// based ONLY on the raw stdout we received (we discard stderr and cannot see the
// exit code). The dumpsys/binder channel itself is known-good on this context
// (gfxinfo dumpsys succeeds from the same daemon), so an empty timestats dump is
// NOT a generic channel/selinux failure -- it means the enable did not arm the
// feature. Reasons:
//   disabled       - empty stdout, or stdout has no "timestats:" block at all:
//                    SF never produced timestats (enable rejected / feature off).
//   selinux_denied - stdout carries a permission-denial / security message.
//   no_layer       - timestats blocks exist but none for the foreground package.
//   parse_empty    - a block exists (fg pkg present) but parse found no usable
//                    frames/histogram (MTK SF output format differs / zero frames).
//   unknown        - none of the above (never guessed beyond these signatures).
static std::string classifyTimestatsReason(const std::string& dump, const std::string& fgPkg) {
    if (dump.empty()) return "disabled";
    if (dump.find("Permission Denial") != std::string::npos ||
        dump.find("Security exception") != std::string::npos ||
        dump.find("denied") != std::string::npos) return "selinux_denied";
    if (dump.find("timestats:") == std::string::npos) return "disabled";
    bool wantFirst = (fgPkg.empty() || fgPkg == "unknown");
    if (!wantFirst && dump.find(fgPkg) == std::string::npos) return "no_layer";
    return "parse_empty";
}

// C2: adaptive timestats blind-window TTL.
// The old fixed 300000ms (5-minute) TTL blinded the WHOLE frame subsystem for 5
// minutes after a SINGLE failed `--timestats -dump` — even a transient "no_layer"
// (the foreground app had not started rendering its SurfaceView yet) or
// "parse_empty" kept us blind long after the channel would have recovered. We now
// use a short TTL so the channel is re-probed periodically (recovery detection):
//   no_layer / parse_empty : 15s — the app may start rendering any moment.
//   disabled / selinux_denied: 60s — channel appears permanently off (MIUI returns
//                              0 bytes); still re-probe periodically so a
//                              surfaceflinger restart / policy change is picked up,
//                              but do not fork a dumpsys every loop tick.
//   unknown                 : 30s.
// Foreground switch already forces unavailableUntilMs=0 upstream (frame loop), so
// a fresh app always gets an immediate re-probe regardless of these TTLs.
static long long timestatsBlindTtlMs(const std::string& reason) {
    if (reason == "no_layer" || reason == "parse_empty") return 15000;
    if (reason == "disabled" || reason == "selinux_denied") return 60000;
    return 30000;
}

TimestatsResult TimestatsSession::tick(long long nowMs,
                     const std::string& fgPkg,
                     const std::function<std::string(const std::string&)>& fetch) {
    TimestatsResult empty;
    if (nowMs < unavailableUntilMs) {
        empty.reason = lastReason;   // surface the classified reason even on TTL short-circuit
        return empty;
    }
    if (!enabled) {
        forkCalls++;
        fetch("dumpsys SurfaceFlinger --timestats -clear -enable 2>/dev/null");
        enabled = true;
        enabledAtMs = nowMs;
        return empty;                                   // warmup: need frames
    }
    if (nowMs - enabledAtMs < 1000) return empty;       // 1s warmup
    forkCalls++;
    std::string dump = fetch("dumpsys SurfaceFlinger --timestats -dump 2>/dev/null");
    // v4.2.8 B8: soft size limit for SF dump (≥1MB, not 64KB — must cover all layers).
    // Protects against pathological large dumps from hundreds of windows.
    static constexpr size_t kMaxTimestatsDumpSize = size_t(1024) * 1024;  // 1MB
    if (dump.size() > kMaxTimestatsDumpSize) {
        dump.resize(kMaxTimestatsDumpSize);
    }
    lastDumpAtMs = nowMs;
    lastDumpContent = dump;  // v4.2.8: save for multi-package parsing
    TimestatsResult r = parseTimestatsBlock(dump, fgPkg);
    if (!r.available) {
        std::string reason = classifyTimestatsReason(dump, fgPkg);
        r.reason = reason;
        lastReason = reason;
        // C2: adaptive short blind window (was fixed 5min). See timestatsBlindTtlMs.
        unavailableUntilMs = nowMs + timestatsBlindTtlMs(reason);
    } else {
        lastReason.clear();
        // v6.0.0 round4c: convert the cumulative active-weighted-mean fps into an
        // INSTANTANEOUS window rate. On the same layer, deltaFrames/ΔwallMs reflects
        // only frames produced since the last tick; old fast frames and long idle
        // gaps no longer dilute the displayed fps. On a layer change / first sample
        // we keep the cumulative value (no baseline to diff) and seed the snapshot.
        if (r.layer == snapLayer && snapTotalFrames >= 0 &&
            r.totalFrames >= snapTotalFrames) {
            long long deltaFrames = (long long)r.totalFrames - snapTotalFrames;
            long long deltaWallMs = nowMs - snapDumpMs;
            if (deltaFrames > 0 && deltaWallMs >= 200) {
                double instFps = (double)deltaFrames / ((double)deltaWallMs / 1000.0);
                // Sanity: reject absurd bursts (defensive, never >300fps).
                if (instFps > 0.0 && instFps <= 300.0) {
                    r.avgFps = instFps;
                    // Scale activeFrames to the window so jank% stays consistent.
                    r.activeFrames = (int)deltaFrames;
                }
            } else if (deltaFrames == 0) {
                // No new frames this window (static UI). Keep avgFps honest: the
                // caller ages the row via tsMs; don't report a stale rate.
                r.avgFps = 0.0;
                r.activeFrames = 0;
            }
        }
        snapLayer = r.layer;
        snapTotalFrames = r.totalFrames;
        snapDumpMs = nowMs;
    }
    return r;
}

void TimestatsSession::stop(const std::function<std::string(const std::string&)>& fetch) {
    if (enabled) {
        forkCalls++;
        fetch("dumpsys SurfaceFlinger --timestats -disable 2>/dev/null");
        enabled = false;
    }
}

} // namespace frameaware
} // namespace aatpe

// ============================================================================
// v4.2.1 manual game-override list runtime (mtime-cached hot reload) +
// installed-apps reading. Defined at top level (sibling of frameaware), not
// inside it, so they are reachable from webui_http / aatped_control as
// aatpe::gamelist / aatpe::apps.
// ============================================================================
namespace {
// Local package validator for the runtime write path (mirrors the pure
// frameaware::aatpeValidPkgCharName; kept here so gamelist does not depend on a
// file-static symbol from another namespace).
bool manualListValidPkg(const std::string& s) {
    if (s.empty() || s.size() > 256) return false;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) return false;
    if (s.front() == '.' || s.back() == '.') return false;
    if (s.find("..") != std::string::npos) return false;
    for (char c : s) {
        if (isalnum((unsigned char)c)) continue;
        if (c == '.' || c == '_') continue;
        return false;
    }
    return true;
}
std::string gameOverridesPath() {
    // Upgrade-preserved module data dir (same tier as aatpe.conf user edits).
    return "/data/adb/aatpe/game_overrides.list";
}
std::mutex g_ovMutex;
std::set<std::string> g_ovCache;
time_t g_ovMtime = 0;

void gameOverridesReloadLocked(bool force) {
    std::string path = gameOverridesPath();
    struct stat st;
    time_t mt = 0;
    if (stat(path.c_str(), &st) == 0) mt = st.st_mtime;
    if (!force && mt == g_ovMtime) return;   // unchanged -> keep cache
    g_ovMtime = mt;
    g_ovCache.clear();
    if (mt != 0) {
        std::ifstream f(path);
        if (f) {
            std::stringstream buf; buf << f.rdbuf();
            g_ovCache = aatpe::frameaware::parseGameOverridesContent(buf.str());
        }
    }
}
}  // namespace

namespace aatpe {
namespace gamelist {
bool isManualGame(const std::string& pkg) {
    if (pkg.empty() || pkg == "unknown") return false;
    std::lock_guard<std::mutex> lk(g_ovMutex);
    gameOverridesReloadLocked(false);
    return g_ovCache.count(pkg) > 0;
}
std::string currentManualGameList() {
    std::lock_guard<std::mutex> lk(g_ovMutex);
    gameOverridesReloadLocked(false);
    std::string out;
    for (auto it = g_ovCache.begin(); it != g_ovCache.end(); ++it) {
        if (!out.empty()) out += "\n";
        out += *it;
    }
    return out;
}
// v4.3.1 (诉求2): snapshot the manual list as a vector (hermetically testable).
std::vector<std::string> manualGameListSnapshot() {
    std::lock_guard<std::mutex> lk(g_ovMutex);
    gameOverridesReloadLocked(false);
    return std::vector<std::string>(g_ovCache.begin(), g_ovCache.end());
}
// v4.3.1 (诉求2, TEST-ONLY): inject the manual list in-memory (no file I/O; on-device
// /data/adb is unavailable to the host harness). g_ovMtime=0 matches a missing file so
// gameOverridesReloadLocked short-circuits and keeps the injected set.
#ifdef AATPE_HOST_TEST
void setManualListForTest(const std::vector<std::string>& pkgs) {
    std::lock_guard<std::mutex> lk(g_ovMutex);
    g_ovCache.clear();
    for (const auto& p : pkgs) g_ovCache.insert(p);
    g_ovMtime = 0;
}
#endif
bool addManualGame(const std::string& pkg) {
    if (!manualListValidPkg(pkg)) return false;
    std::lock_guard<std::mutex> lk(g_ovMutex);
    gameOverridesReloadLocked(false);
    if (g_ovCache.count(pkg) > 0) return true;   // idempotent
    mkdir("/data/adb/aatpe", 0755);
    std::string path = gameOverridesPath();
    FILE* f = fopen(path.c_str(), "a");
    if (!f) return false;
    fprintf(f, "%s\n", pkg.c_str());
    fclose(f);
    g_ovCache.insert(pkg);
    struct stat st; if (stat(path.c_str(), &st) == 0) g_ovMtime = st.st_mtime;
    return true;
}
bool removeManualGame(const std::string& pkg) {
    if (!manualListValidPkg(pkg)) return false;
    std::lock_guard<std::mutex> lk(g_ovMutex);
    gameOverridesReloadLocked(false);
    if (g_ovCache.erase(pkg) == 0) return true;   // not present -> treat as ok
    std::string path = gameOverridesPath();
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    for (auto it = g_ovCache.begin(); it != g_ovCache.end(); ++it)
        fprintf(f, "%s\n", it->c_str());
    fclose(f);
    struct stat st; if (stat(path.c_str(), &st) == 0) g_ovMtime = st.st_mtime;
    return true;
}
}  // namespace gamelist

namespace apps {
// P0 D1100/MIUI/KernelSU: root-side /data/app scan fallback. On MIUI under
// KernelSU, `pm`/`cmd package` can be blocked by SELinux or hit a slow cold
// start (binder to a not-yet-woken AMS) and exceed the 3s exec budget. Root can
// read /data/app directly. Layouts:
//   /data/app/<pkg>-<hash>/base.apk
//   /data/app/~~<rand>==/<pkg>-<hash>/base.apk   (Android 11+)
// We read the install-dir leaf name "<pkg>-<hash>" and strip the trailing
// "-<hash>". Non-valid package names (system dirs, "~~") are skipped.
static std::vector<std::string> scanDataAppPackages() {
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::vector<std::string> roots = {"/data/app"};
    // Android 11+ bucket dirs like /data/app/~~abcd==/
    if (DIR* top = opendir("/data/app")) {
        struct dirent* de;
        while ((de = readdir(top)) != nullptr) {
            std::string n = de->d_name;
            if (n.rfind("~~", 0) == 0) roots.push_back("/data/app/" + n);
        }
        closedir(top);
    }
    for (const std::string& root : roots) {
        DIR* d = opendir(root.c_str());
        if (!d) continue;
        struct dirent* de;
        while ((de = readdir(d)) != nullptr) {
            std::string leaf = de->d_name;
            if (leaf == "." || leaf == "..") continue;
            // leaf = "<pkg>-<hash>" ; strip last "-<hash>" segment.
            size_t dash = leaf.rfind('-');
            std::string pkg = (dash != std::string::npos && dash > 0) ? leaf.substr(0, dash) : leaf;
            if (!frameaware::aatpeValidPkgCharName(pkg)) continue;
            if (seen.insert(pkg).second) out.push_back(pkg);
        }
        closedir(d);
    }
    return out;
}

std::vector<std::string> listInstalledPackages() {
    // v4.3.1h2: dedicated mutex for the installed-package static cache. The v4.3.1
    // detached label-warmup thread now calls this every 2s AND the HTTP thread
    // (handleApps) calls it concurrently; the previous unlocked static
    // vector/string + 30s mtime cache was a data race (UB / torn read). Forks
    // (pm enumeration) are done OUTSIDE the lock into a local vector, then the
    // cache is swapped under a short critical section. Lock order: this lock is
    // leaf-level (never held while taking sLabelCacheMtx/g_perAppMutex), so no
    // nesting inversion.
    static std::mutex sInstalledPkgsMtx;
    static std::vector<std::string> s_cache;
    static time_t s_cacheT = 0;
    time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> lk(sInstalledPkgsMtx);
        if (!s_cache.empty() && (now - s_cacheT) < 30) return s_cache;
    }

    // P0: use ABSOLUTE paths — under KernelSU root shell PATH may not include
    // /system/bin, so bare `pm` resolves to "not found". Tiered fallback:
    //   1) /system/bin/pm list packages -3
    //   2) /system/bin/cmd package list packages -3
    //   3) /system/bin/pm list packages (all)
    //   4) scan /data/app directly (root, no binder) — MIUI/SELinux-resilient.
    // NOTE: enumeration (fork) runs WITHOUT holding sInstalledPkgsMtx.
    static const char* cmds[] = {
        "/system/bin/pm list packages -3 2>/dev/null",
        "/system/bin/cmd package list packages -3 2>/dev/null",
        "/system/bin/pm list packages 2>/dev/null",
    };
    std::vector<std::string> fresh;
    for (const char* c : cmds) {
        std::string out = aatpeExec(c, "APPS_LIST");
        std::vector<std::string> pkgs = frameaware::parsePmPackageList(out);
        if (!pkgs.empty()) { fresh = std::move(pkgs); break; }
    }
    // Tier 3.5 (R7): merge in work-profile / Private Space packages that
    // `pm list packages` omits by default. Reads the global
    // /data/system/packages.list directly (root, no fork), filters to
    // third-party apps (uid >= 10000), and deduplicates against `fresh`.
    // Fail-closed: if the global list read fails, `fresh` is unchanged.
    {
        std::vector<std::string> all = aatpe::apklabel::listInstalledPackagesAllUsers();
        for (const auto& p : all) {
            if (std::find(fresh.begin(), fresh.end(), p) == fresh.end())
                fresh.push_back(p);
        }
    }
    if (fresh.empty()) {
        // Tier 4: direct /data/app scan (no pm needed).
        fresh = scanDataAppPackages();
    }

    std::lock_guard<std::mutex> lk(sInstalledPkgsMtx);
    if (!fresh.empty()) {
        s_cache = fresh;
        s_cacheT = now;
        return fresh;
    }
    return s_cache;   // keep stale cache if all probes fail
}
}  // namespace apps
}  // namespace aatpe

// ROUND-014 P3 §2.1 — 帧/jank 统一收口（G5/G7 只读 g_jankBoostVote）。
// 所有帧通道（gfxinfo / latency_binder / latency_dumpsys / timestats）都必须
// 经此上报；不存在第二处判票逻辑。
//   - source == "none" 或 samples<5：显式清票，不误抬 boost。
//   - samples>=5 且 missRate>=0.05：arm g_jankBoostVote。
//   - 状态键全部 atomic writeFile 0600（frame_source/game_*fps/game_frame_miss/
//     frame_unavailable_reason）。
// v4.2.1 T3: per-app rolling frame stats — file-scope shared state.
// Design:
//   - PerAppWindow ring (15 windows ≈ 30s): aggregated fps/dropped/samples/jank.
//   - PerAppFrameTimes ring (≤600 individual frame durations, ms): REAL p50/p95/p99
//     from `dumpsys gfxinfo <pkg> framestats`. Falls back to SF-histogram p99
//     when framestats unavailable/blocked/empty; p50/p95 stay 0.0 (honest "--").
struct PerAppWindow {
    double fps;        // avg fps in this window
    double p99Fps;     // p99 frame rate from SF histogram (fps, not ms)
    long   samples;    // frames in this window
    long   dropped;    // jank frames in this window (= missRate * samples)
    bool   jankArmed;  // missRate >= 5% in this window
    long long tsMs;    // window end timestamp
};
static constexpr size_t kMaxWindowsPerApp = 15;   // ~30s at 2s/cycle
static constexpr size_t kMaxFrameSamplesPerApp = 600;  // ~10s at 60fps
static constexpr long long kActiveGapMaxMs = 4000;  // gap >4s = background
// Moved up (was ~2295): processManualGameLayers (RC2) now evicts against this cap.
constexpr size_t kMaxPerAppStats = 24;

// g_perAppMutex is DEFINED once in aatped_config.cpp (declared extern in
// aatped_config.h:77). This must NOT be a second defining declaration (ld.lld
// duplicate symbol); reference it via the header's extern.
extern std::mutex g_perAppMutex;
static std::unordered_map<std::string, std::deque<PerAppWindow>> g_perAppWindows;
static std::unordered_map<std::string, std::deque<double>> g_perAppFrameTimes;
// v4.2.6: per-app framestats metrics (target_fps / janky_pct / missed_vsyncs).
// Populated by computeFramestatsMetrics() on each 5s framestats refresh.
struct PerAppFsMetrics {
    double fps = 0.0;
    double targetFps = 0.0;
    double jankyPct = 0.0;
    long missedVsyncs = 0;
    long totalFrames = 0;
    long long updatedMs = 0;  // v4.2.8 B12: NOTE: written but not used for stale filter.
                              // Stale filter uses g_perAppWindows ring.back().tsMs
                              // (monotonic/steady_clock via frameNowMs(), NOT wall-clock).
                              // Kept for future use / debugging.
};
static std::unordered_map<std::string, PerAppFsMetrics> g_perAppFsMetrics;
std::list<std::string> g_perAppLru;

// v6.0.0 round4b: per-LAYER cumulative timestats snapshot. SurfaceFlinger timestats
// is a CUMULATIVE histogram since layer creation — not a rolling window. Computing
// fps directly from it dilutes real combat fps (80-120) down to 0.1/0.2 by mixing
// load/menu/pause intervals. We diff consecutive dumps: instantaneous fps =
// ΔtotalFrames / ΔwallMs. A layer whose ΔtotalFrames<=0 this tick is NOT actively
// presenting (stopped / backgrounded) -> we do NOT refresh its fsMetrics/window, so
// its ring ageMs grows and the row ages into background_stale ("--"). Guarded by
// g_perAppMutex (same lock as g_perAppFsMetrics).
struct TimestatsLayerSnap {
    long long totalFrames = 0;
    std::unordered_map<double, long long> cumHist;
    long long lastDumpMs = 0;
    bool has = false;
};
static std::unordered_map<std::string, TimestatsLayerSnap> g_tsLayerSnap;

// R6: monotonic clock for per-app window age / staleness. Steady clock is immune to
// wall-clock jumps (user setting the time, NITZ, timezone DST) that would otherwise make
// the 60s stale filter misfire. Used BOTH when stamping a window (tsMs) and when the
// accessor measures ageMs, so the two ends always agree. Display/log filenames may still
// use system_clock; age/TTL/staleness must NOT.
static long long frameNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Test accessors for per-app fs-metrics eviction + monotonic clock (v94 unit tests).
int aatpeDebugFsMetricsSize() {
    std::lock_guard<std::mutex> lk(g_perAppMutex);
    return (int)g_perAppFsMetrics.size();
}
bool aatpeDebugFsMetricsHas(const std::string& p) {
    std::lock_guard<std::mutex> lk(g_perAppMutex);
    return g_perAppFsMetrics.count(p) > 0;
}
long long aatpeDebugNowMonotonic() { return frameNowMs(); }
void aatpeDebugFsMetricsSet(const std::string& p) {
    std::lock_guard<std::mutex> lk(g_perAppMutex);
    auto& m = g_perAppFsMetrics[p];
    m.fps = 60; m.targetFps = 60; m.jankyPct = 0.05; m.totalFrames = 10;
    m.updatedMs = frameNowMs();
}
// rel12 Bug1/Bug2: richer inject seam so a host test can drive the exact rel14
// on-device scenario (missedVsyncs >> totalFrames -> dropped used to exceed
// total_frames; single fresh window -> active_ms used to be 0).
void aatpeDebugFsMetricsInject(const std::string& p, double fps, double jankyPct,
                               long totalFrames, long missedVsyncs) {
    std::lock_guard<std::mutex> lk(g_perAppMutex);
    auto& m = g_perAppFsMetrics[p];
    m.fps = fps; m.targetFps = fps; m.jankyPct = jankyPct;
    m.totalFrames = totalFrames; m.missedVsyncs = missedVsyncs;
    m.updatedMs = frameNowMs();
}

// FRAME-01: g_tsLayerSnap is keyed by SF layer name (which embeds the package,
// e.g. "SurfaceView[com.foo/Bar]@0(BLAST)#1234"). The old LRU eviction cleared
// g_perAppWindows / g_perAppFrameTimes / g_perAppFsMetrics but LEFT
// g_tsLayerSnap entries behind — every window rotation (#1234 -> #1235) created
// a new entry that was never reclaimed, growing the map without bound.
// Callers MUST hold g_perAppMutex. When pkg is evicted, drop every layer snap
// whose key contains pkg (layer names always embed the owning package).
static void evictTsLayerSnapForPkgLocked(const std::string& pkg) {
    if (pkg.empty()) return;
    for (auto it = g_tsLayerSnap.begin(); it != g_tsLayerSnap.end(); ) {
        if (it->first.find(pkg) != std::string::npos) {
            it = g_tsLayerSnap.erase(it);
        } else {
            ++it;
        }
    }
}

// Debug accessors for the FRAME-01 regression test (host).
int aatpeDebugTsLayerSnapSize() {
    std::lock_guard<std::mutex> lk(g_perAppMutex);
    return (int)g_tsLayerSnap.size();
}
bool aatpeDebugTsLayerSnapHas(const std::string& layer) {
    std::lock_guard<std::mutex> lk(g_perAppMutex);
    return g_tsLayerSnap.count(layer) > 0;
}
void aatpeDebugTsLayerSnapAdd(const std::string& layer) {
    std::lock_guard<std::mutex> lk(g_perAppMutex);
    auto& sn = g_tsLayerSnap[layer];
    sn.totalFrames = 100;
    sn.lastDumpMs = frameNowMs();
    sn.has = true;
}

// v4.2.8: authoritative foreground snapshot for frame subsystem.
// Written every tick by the frame sampling loop (both success and failure paths).
// Read by aatpe_getPerAppFrameStats() to ensure "current" matches frame_debug_fg.
static std::mutex g_frameAuthMutex;
static std::string g_frameAuthFg = "unknown";
static std::string g_frameAuthSource = "none";
static std::string g_frameAuthReason = "";

void frameSetAuthSnapshot(const std::string& fg, const std::string& source, const std::string& reason) {
    // v4.2.8 B2: copy values under lock, then write files outside lock.
    // This avoids holding g_frameAuthMutex during file I/O (W1-009).
    std::string fgCopy, srcCopy, rsnCopy;
    {
        std::lock_guard<std::mutex> lk(g_frameAuthMutex);
        g_frameAuthFg = fg;
        g_frameAuthSource = source;
        g_frameAuthReason = reason;
        fgCopy = fg;
        srcCopy = source;
        rsnCopy = reason;
    }
    // Write files outside lock.
    writeFile((std::string(STATE_DIR) + "/frame_debug_fg").c_str(), fgCopy.c_str());
    writeFile((std::string(STATE_DIR) + "/frame_source").c_str(), srcCopy.c_str());
    writeFile((std::string(STATE_DIR) + "/frame_unavailable_reason").c_str(), rsnCopy.c_str());
}
void frameGetAuthSnapshot(std::string& fg, std::string& source, std::string& reason) {
    std::lock_guard<std::mutex> lk(g_frameAuthMutex);
    fg = g_frameAuthFg;
    source = g_frameAuthSource;
    reason = g_frameAuthReason;
}

// v4.3.1 RC3: fusion jank-boost enter/exit hysteresis, extracted as a PURE function so
// the enter/exit rules are hermetically testable (frame.o links into host tests).
//   anyJank=true  -> arm streak grows; vote arms once >=armThresh; clear streak resets.
//   anyJank=false -> arm streak resets; vote RELEASES only after >=releaseThresh clean
//   cycles, so a single clean cycle (or sampling jitter) cannot thrash placement.
// Streaks are caller-held; prevVote carried across calls. Behavior identical to the old
// inline atomic counter, but now unit-testable.
bool aatpe::frameaware::aatpeFusionJankVote(bool anyJank, int& armStreak, int& clearStreak,
                                             int armThresh, int releaseThresh, bool prevVote) {
    if (anyJank) {
        clearStreak = 0;
        armStreak++;
        return armStreak >= armThresh ? true : prevVote;
    }
    armStreak = 0;
    clearStreak++;
    if (clearStreak >= releaseThresh) return false;
    return prevVote;
}

// v4.3.1: label warmup round-robin picker (PURE, hermetically testable).
// Given installed packages + which already have a cached label, return up to
// maxPerBeat indices to warm this beat, advancing `cursor`. Skips cached ones so the
// HTTP thread only ever does peekCachedLabel (non-blocking). No busy-wait, no growth.
std::vector<size_t> aatpeLabelWarmupPick(const std::vector<std::string>& pkgs,
                                         const std::vector<bool>& cached,
                                         size_t& cursor, size_t maxPerBeat) {
    std::vector<size_t> out;
    if (pkgs.empty()) return out;
    size_t n = pkgs.size();
    size_t i = cursor % n;
    size_t start = i;
    do {
        bool hit = (i < cached.size()) ? cached[i] : false;
        if (!hit) out.push_back(i);
        i = (i + 1) % n;
        if (out.size() >= maxPerBeat) break;
    } while (i != start);
    cursor = i;  // resume next beat after the last examined index
    return out;
}

// v4.3.1 hotfix: ONE warmup beat on whatever thread calls it. Picks up to
// maxPerBeat uncached packages and resolves their Chinese label via
// labelForPackage (open APK + central-dir scan + zlib inflate, up to 96MB).
// Split out so it can be unit-tested and so the background thread body is
// identical to the old inline logic. Returns how many packages it resolved.
size_t aatpeLabelWarmupTick(size_t maxPerBeat) {
    std::vector<std::string> pkgs = apps::listInstalledPackages();
    if (pkgs.empty()) return 0;
    std::vector<bool> cached(pkgs.size(), false);
    for (size_t i = 0; i < pkgs.size(); i++)
        cached[i] = !aatpe::apklabel::peekCachedLabel(pkgs[i]).empty();
    static size_t s_labelCursor = 0;
    std::vector<size_t> picks = aatpeLabelWarmupPick(pkgs, cached, s_labelCursor, maxPerBeat);
    for (size_t idx : picks)
        (void)aatpe::apklabel::labelForPackage(pkgs[idx]);
    return picks.size();
}

// v4.3.1 hotfix: label warmup MUST NOT run on the decision thread. The old
// inline block at the tail of readSurfaceFlinger() called labelForPackage
// synchronously every ~200ms beat; on real devices with hundreds of apps and
// multi-hundred-MB game APKs that blocked the decision loop for hundreds of
// ms~seconds, so g_stateGeneration stalled, the SSE thread only emitted
// lightweight state_generation probes, and the WebUI froze at "~2 seconds"
// while manual apps never got their first frame sample. The fix: run the exact
// same warmup on a detached low-nice background thread. The decision loop
// never calls labelForPackage again; HTTP stays on peekCachedLabel only.
static void aatpeLabelWarmupThreadFn() {
    nice(15);  // low priority: never compete with the decision loop
    while (!g_shouldExit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        if (g_shouldExit.load(std::memory_order_relaxed)) break;
        if (g_thermalEmergency) continue;  // thermal emergency: no extra I/O
        (void)aatpeLabelWarmupTick(2);
    }
}

static void aatpeStartLabelWarmupThread() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        std::thread(aatpeLabelWarmupThreadFn).detach();
    });
}

// Forward declare VmContainerInfo (defined later in this file).
struct VmContainerInfo;

// v4.2.8: multi-package SF timestats tracking helper.
// Parses all manual game layers from the same dump, handles VM attribution.
// Called from both success and failure paths. Skips already-processed fgPkg.
// F-2: last param is the dump CONTENT fetch time (lastDumpAtMs), not the loop tick.
static void processManualGameLayers(const std::string& dump,
                                     const std::string& fgPkg,
                                     const std::string& fgVmInner,
                                     long long contentTsMs);

// v4.2.7: validate SF latency samples — check for real variance (not just vsync period echo).
bool aatpe::frameaware::sfLatencySamplesValid(const std::vector<long long>& us) {
    if (us.empty()) return false;
    long long sum = 0;
    for (long long v : us) sum += v;
    long long avg = sum / (long long)us.size();
    if (avg <= 0) return false;
    for (long long v : us) {
        double dev = std::abs((double)v - (double)avg) / (double)avg;
        if (dev > 0.25) return true;  // >25% deviation = real variance
    }
    return false;  // all ≈ same value = vsync period echo
}

// v4.2.7: binder retry backoff — 5s, 10s, 20s, capped at 30s.
long long aatpe::frameaware::frameBinderBackoffMs(int retryCount) {
    if (retryCount <= 0) return 5000;
    long long ms = 5000LL << retryCount;
    if (ms > 30000) ms = 30000;
    return ms;
}

// v4.2.7: VM/container game detection.
// Returns {containerPkg, innerGamePkg} when foreground is a known VM container
// and a manual game override package is running inside it.
// Generic: iterate game_overrides, check via pidof + /proc/<pid>/status UID match.
struct VmContainerInfo {
    std::string containerPkg;
    std::string innerGamePkg;
    std::string innerPids;  // space-separated pids for state reporting
    bool valid = false;
};

// ============================================================================
// R5 (zero-package-name hardcoding, C-group): the set of VM/dual-space container
// prefixes is NO LONGER a built-in literal table in frame.cpp. It is a runtime-
// loaded, externally configurable manifest (<state_dir>/vm_containers.list, one
// package prefix per line, '#' comments / blank lines ignored) plus a test seam —
// the EXACT contract placement.cpp (FIX-B) uses. Adding a new container is a config
// edit, not a code change. No VM package names appear in production source; all
// three prior literal tables (processManualGameLayers / isTransientFocus /
// detectVmContainerGame) now consult this single loaded prefix set.
// ============================================================================
static std::vector<std::string> g_frameVmContainerPrefixes;
static std::mutex g_frameVmContainerMtx;
static bool g_frameVmContainerListLoaded = false;

static void loadFrameVmContainerPrefixesLocked() {
    g_frameVmContainerPrefixes.clear();
    std::string path = std::string(STATE_DIR) + "/vm_containers.list";
    std::ifstream f(path.c_str());
    if (!f) return;   // manifest missing -> empty set (safe: matches nothing)
    std::string line;
    while (std::getline(f, line)) {
        // trim surrounding whitespace
        size_t b = line.find_first_not_of(" \t\r");
        size_t e = line.find_last_not_of(" \t\r");
        if (b == std::string::npos) continue;
        line = line.substr(b, e - b + 1);
        if (line.empty() || line[0] == '#') continue;   // blank / comment
        g_frameVmContainerPrefixes.push_back(line);
    }
}

// Test seam / runtime API: override the container prefix list (host tests inject
// the known family; operators can also push a list programmatically). Marks the
// list as loaded so we do not re-read the file behind the injection.
#ifdef AATPE_HOST_TEST
void setFrameVmContainerPrefixesForTest(const std::vector<std::string>& pkgs) {
    std::lock_guard<std::mutex> lk(g_frameVmContainerMtx);
    g_frameVmContainerPrefixes = pkgs;
    g_frameVmContainerListLoaded = true;
}
#endif

// True iff pkg is a known VM/dual-space container (exact match or dotted-prefix
// variant). Lazy-loads the manifest once on first use. Missing/empty manifest ->
// matches nothing: safe (no crash, no false positive, no default-allow), and a
// spoof like "com.malicious.spoof" never matches.
bool aatpeFrameIsVmContainerPkg(const std::string& pkg) {
    if (pkg.empty()) return false;
    std::lock_guard<std::mutex> lk(g_frameVmContainerMtx);
    if (!g_frameVmContainerListLoaded) {
        g_frameVmContainerListLoaded = true;
        loadFrameVmContainerPrefixesLocked();
    }
    for (const std::string& p : g_frameVmContainerPrefixes) {
        if (p.empty()) continue;
        if (pkg == p) return true;
        if (pkg.rfind(p + ".", 0) == 0) return true;   // dotted-prefix variant
    }
    return false;
}

// True iff prevFg and newFg belong to the SAME loaded VM-container prefix group
// (both exact-match or dotted-match ONE shared prefix). This preserves the old
// isTransientFocus "same container group" semantics (a transient VM assistant
// window popping up over the container is NOT a session switch) without hardcoding
// prefixes. Two unrelated dual-space apps do NOT count as the same group.
static bool frameSameVmContainerGroup(const std::string& prevFg, const std::string& newFg) {
    if (prevFg.empty() || newFg.empty()) return false;
    std::lock_guard<std::mutex> lk(g_frameVmContainerMtx);
    if (!g_frameVmContainerListLoaded) {
        g_frameVmContainerListLoaded = true;
        loadFrameVmContainerPrefixesLocked();
    }
    for (const std::string& p : g_frameVmContainerPrefixes) {
        if (p.empty()) continue;
        auto under = [&](const std::string& s) {
            return s == p || s.rfind(p + ".", 0) == 0;
        };
        if (under(prevFg) && under(newFg)) return true;
    }
    return false;
}

// Forward declaration (defined later in this file).
static VmContainerInfo detectVmContainerGame(const std::string& fgPkg);

// v4.2.8: multi-package SF timestats tracking helper.
// Parses all manual game layers from the same dump, handles VM attribution.
// Called from both success and failure paths. Skips already-processed fgPkg.
//
// F-2: `contentTsMs` is the timestamp at which THIS dump content was actually
// fetched (TimestatsSession::lastDumpAtMs) — NOT the current loop tick. When the
// timestats channel is in its blind window, tick() short-circuits WITHOUT
// re-fetching, so lastDumpContent holds STALE bytes; stamping windows with the
// loop's nowMs made frozen manual rows look live and defeated the 60s stale
// filter. Windows/metrics are now stamped with the content's real age.
//
// F-5: the dump is streamed+parsed EXACTLY ONCE here (parseTimestatsAllBlocks),
// then the per-package best block is looked up from the cached grouping —
// previously every manual package re-parsed the whole ≤1MB dump from scratch.
static void processManualGameLayers(const std::string& dump,
                                     const std::string& fgPkg,
                                     const std::string& fgVmInner,
                                     long long contentTsMs) {
    if (dump.empty()) return;
    std::string manualList = gamelist::currentManualGameList();
    std::istringstream iss(manualList);
    std::string pkg;

    // F-5: one single dump pass. Group every usable block by its extracted
    // package, then apply the same primary-surface selection per package.
    std::map<std::string, std::vector<aatpe::frameaware::TimestatsResult>> grouped;
    for (const auto& bi : aatpe::frameaware::parseTimestatsAllBlocks(dump)) {
        grouped[bi.pkg].push_back(bi.res);
    }
    // v6.0.0 round4b: DELTA-based instantaneous fps. group still holds CUMULATIVE
    // histograms; selection + diffing happens under g_perAppMutex below.
    (void)0;

    // v6.0.0 round4b: do ALL selection + snapshot-diffing under g_perAppMutex (it
    // also guards g_tsLayerSnap). For each manual package, among its candidate SF
    // layers pick the one whose cumulative totalFrames grew the MOST this tick —
    // the layer that is actively presenting, not a static parent container. fps =
    // Δframes / ΔwallMs. A layer with Δ<=0 is not presenting: we skip it, so its
    // fsMetrics/window go stale and the row ages into background_stale ("--").
    std::lock_guard<std::mutex> lk(g_perAppMutex);
    while (std::getline(iss, pkg)) {
        if (pkg.empty()) continue;

        std::string matchPkg = pkg;
        std::string attrPkg = pkg;

        bool isKnownContainer = aatpeFrameIsVmContainerPkg(pkg);
        if (isKnownContainer) {
            VmContainerInfo vmInfo = detectVmContainerGame(pkg);
            if (vmInfo.valid && !vmInfo.innerGamePkg.empty()) {
                matchPkg = vmInfo.containerPkg;
                attrPkg = vmInfo.innerGamePkg;
            } else {
                continue;
            }
        }
        else if (!fgVmInner.empty() && pkg == fgVmInner) {
            continue;
        }
        if (attrPkg == fgPkg) continue;  // skip already-processed fg

        auto git = grouped.find(matchPkg);
        if (git == grouped.end() || git->second.empty()) continue;

        // Choose the layer with the largest cumulative growth this tick.
        const aatpe::frameaware::TimestatsResult* best = nullptr;
        long long bestDelta = -1;
        for (const auto& blk : git->second) {
            if (!blk.available) continue;
            auto& sn = g_tsLayerSnap[blk.layer];
            long long d = (long long)blk.totalFrames - sn.totalFrames;
            if (d > bestDelta) { bestDelta = d; best = &blk; }
        }
        if (!best) continue;
        auto& sn = g_tsLayerSnap[best->layer];

        // First sighting: seed baseline, warmup — do NOT emit a (meaningless) fps.
        if (!sn.has) {
            sn.totalFrames = best->totalFrames;
            sn.lastDumpMs = contentTsMs;
            sn.cumHist.clear();
            for (auto& b : best->hist) sn.cumHist[b.first] = b.second;
            sn.has = true;
            continue;
        }

        long long deltaTotal = (long long)best->totalFrames - sn.totalFrames;
        long long deltaWallMs = contentTsMs - sn.lastDumpMs;
        // Delta-jank computed from the OLD snapshot buckets (before we overwrite).
        double targetPeriodMs = (best->targetFps > 1.0) ? (1000.0 / best->targetFps) : 16.67;
        long long jankyDelta = 0;
        for (auto& b : best->hist) {
            long long prevC = 0;
            auto itc = sn.cumHist.find(b.first);
            if (itc != sn.cumHist.end()) prevC = itc->second;
            long long dC = b.second - prevC;
            if (dC < 0) dC = 0;
            if (b.first >= 1.5 * targetPeriodMs) jankyDelta += dC;
        }

        // Advance snapshot to this dump (regardless of active/inactive).
        sn.totalFrames = best->totalFrames;
        sn.lastDumpMs = contentTsMs;
        sn.cumHist.clear();
        for (auto& b : best->hist) sn.cumHist[b.first] = b.second;

        // Not actively presenting this tick -> do NOT refresh metrics/window.
        if (deltaTotal <= 0 || deltaWallMs <= 0) continue;

        double fps = (double)deltaTotal / ((double)deltaWallMs / 1000.0);
        double jankPct = (deltaTotal > 0) ? ((double)jankyDelta / (double)deltaTotal) : 0.0;
        if (jankPct < 0.0) jankPct = 0.0;
        if (jankPct > 1.0) jankPct = 1.0;

        auto& m = g_perAppFsMetrics[attrPkg];
        m.fps = fps;
        m.targetFps = best->targetFps;
        m.jankyPct = jankPct;
        m.missedVsyncs = jankyDelta;
        m.totalFrames = (long)deltaTotal;
        m.updatedMs = contentTsMs;

        // upsert window + LRU (only on an actively-presenting layer, round4b-B).
        auto wit = g_perAppWindows.find(attrPkg);
        if (wit == g_perAppWindows.end()) {
            if (g_perAppWindows.size() >= kMaxPerAppStats) {
                std::string old = g_perAppLru.back(); g_perAppLru.pop_back();
                g_perAppWindows.erase(old);
                g_perAppFrameTimes.erase(old);
                g_perAppFsMetrics.erase(old);
                evictTsLayerSnapForPkgLocked(old);  // FRAME-01
            }
            g_perAppLru.push_front(attrPkg);
        } else {
            g_perAppLru.remove(attrPkg);
            g_perAppLru.push_front(attrPkg);
        }
        auto& ring = g_perAppWindows[attrPkg];
        PerAppWindow w;
        w.fps = fps;
        w.samples = (long)deltaTotal;
        w.dropped = (long)jankyDelta;
        w.jankArmed = (jankPct >= 0.05);
        w.tsMs = contentTsMs;  // last moment this layer was actively presenting
        ring.push_back(w);
        while (ring.size() > kMaxWindowsPerApp) ring.pop_front();
    }
}

// F-2/F-5 regression seam: drive processManualGameLayers from a host test with an
// explicit content timestamp (the loop would otherwise pass lastDumpAtMs).
#ifdef AATPE_HOST_TEST
void aatpeDebugProcessManualLayersForTest(const std::string& dump,
                                          const std::string& fgPkg,
                                          long long contentTsMs) {
    processManualGameLayers(dump, fgPkg, "", contentTsMs);
}
#endif

// v4.2.7: focus debounce / transient focus detection.
// Returns true if the new foreground is a transient overlay that should NOT
// switch away from the current game session (VM assistant windows, floating
// windows, status bar, launcher, etc.).
bool aatpe::frameaware::isTransientFocus(const std::string& prevFg, const std::string& newFg) {
    if (newFg.empty() || newFg == "unknown") return true;
    // Transient/overlay packages that shouldn't trigger a session switch.
    static const char* transientPkgs[] = {
        "com.android.systemui",       // status bar / notification shade
        "com.android.launcher",      // generic launcher
        "com.miui.home",             // MIUI launcher
        "com.miui.securitycenter",   // MIUI security / floating windows
        "com.omarea.vtools",         // vtools floating window
        "com.android.inputmethod",   // IME
        "org.lsposed.lspatch",      // LSPatch
        "com.dergoogler.mmrl",       // MMRL / SukiSU WebUI
        nullptr
    };
    for (int i = 0; transientPkgs[i]; i++) {
        if (newFg == transientPkgs[i]) return true;
    }
    // VM assistant windows (same container package prefix).
    // R5: container group membership comes from the runtime-loaded manifest
    // (frameSameVmContainerGroup) — no hardcoded package literals here.
    // If prevFg and newFg are in the same loaded container prefix group, treat as
    // transient (assistant window popping up over the VM).
    if (frameSameVmContainerGroup(prevFg, newFg)) {
        return true;  // same container group - assistant window
    }
    return false;
}

// Get UID of a package from /data/system/packages.list (or fall back to /proc).
// v4.2.9 F-02: read /data/system/packages.list directly (no shell, no fork).
// Eliminates shell injection surface entirely (was: grep '^pkg ' ... | head -1).
// File format: "pkgName uid debugFlag dataPath ..." per line.
// M4/R1: the parsing body is split into getPkgUidImpl(pkg, path) so host v94
// regression tests can drive it directly through the aatpeTestGetPkgUid seam
// (link frame.o) instead of #include'ing this whole ~300KB TU. The only thing
// that differs between release and host-test builds is PATH SELECTION:
//   - release (no AATPE_HOST_TEST): path is the compile-time constant
//     /data/system/packages.list. Neither the getenv() call nor the literal
//     "AATPE_PKGLIST_PATH" survives in the release .rodata (R1 F1 gate: zero
//     test symbols / zero test strings in the NDK release binary).
//   - host test (AATPE_HOST_TEST): AATPE_PKGLIST_PATH may override the path so
//     host parsers/fuzzers can drive the parser. The env is read on each call
//     (no mutable global state); production behaviour is byte-identical.
static long getPkgUidImpl(const std::string& pkg, const char* path) {
    char line[512];
    // RCV M3 (NIT, defense-in-depth): pkgLen must never reach the line buffer, or
    // strncmp()/line[pkgLen] would read past line[512]. All production entry
    // points hard-cap pkg at <=256 chars, so this is unreachable in release; it
    // pins the contract for any future caller.
    if (pkg.size() >= sizeof(line)) return -1;
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    long uid = -1;
    while (fgets(line, sizeof(line), f)) {
        // Line starts with pkg name followed by space.
        // Use strncmp to match prefix, then verify next char is space.
        size_t pkgLen = pkg.size();
        if (strncmp(line, pkg.c_str(), pkgLen) == 0 && line[pkgLen] == ' ') {
            // Parse uid (second field).
            char* p = line + pkgLen + 1;
            uid = strtol(p, nullptr, 10);
            break;
        }
    }
    fclose(f);
    return uid;
}

static long getPkgUid(const std::string& pkg) {
#ifdef AATPE_HOST_TEST
    const char* envPath = getenv("AATPE_PKGLIST_PATH");
    const char* path = (envPath && *envPath) ? envPath : "/data/system/packages.list";
    return getPkgUidImpl(pkg, path);
#else
    return getPkgUidImpl(pkg, "/data/system/packages.list");
#endif
}

#ifdef AATPE_HOST_TEST
// Host-test seam: drive the packages.list parser on an explicit path. Not
// compiled in release (no AATPE_HOST_TEST). Follows the tree's extern-"C"
// *ForTest convention (see aatped_priority.cpp / integrity.cpp guarded seams).
extern "C" long aatpeTestGetPkgUid(const char* pkg, const char* pkglistPath) {
    if (!pkg || !*pkg) return -1;
    const char* path = (pkglistPath && *pkglistPath) ? pkglistPath : "/data/system/packages.list";
    return getPkgUidImpl(std::string(pkg), path);
}
#endif

// v4.2.8 B2: time-based cache for VM container detection (avoids pidof fork storm).
struct VmCacheEntry {
    VmContainerInfo info;
    long long checkedMs = 0;
};
// FRAME-10: protect s_vmDetectCache with a dedicated mutex. All current callers
// are on the main daemon loop thread, but gpuBoostTick / future threads may
// call detectVmContainerGame concurrently; the lock makes the data-race contract
// explicit rather than relying on single-threaded luck.
static std::mutex s_vmDetectMtx;
static std::unordered_map<std::string, VmCacheEntry> s_vmDetectCache;
static constexpr long long kVmCacheTtlMs = 3000;  // 3s TTL

static VmContainerInfo detectVmContainerGame(const std::string& fgPkg) {
    VmContainerInfo info;
    if (fgPkg.empty() || fgPkg == "unknown") return info;
    // Check cache first (FRAME-10: locked, then release before expensive I/O).
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> lk(s_vmDetectMtx);
        auto cit = s_vmDetectCache.find(fgPkg);
        if (cit != s_vmDetectCache.end() && (nowMs - cit->second.checkedMs) < kVmCacheTtlMs) {
            return cit->second.info;
        }
    }
    // Known VM container packages (fast-path heuristic; uid check is the real test).
    // R5: membership comes from the runtime-loaded manifest
    // (aatpeFrameIsVmContainerPkg) — no hardcoded package literals here.
    bool isContainer = aatpeFrameIsVmContainerPkg(fgPkg);
    if (!isContainer) return info;

    long containerUid = getPkgUid(fgPkg);
    if (containerUid <= 0) return info;  // can't verify uid, skip

    // Iterate game_overrides list and find which inner game has a process
    // running with the same UID as the container.
    std::string overrides = gamelist::currentManualGameList();
    if (overrides.empty()) return info;
    std::istringstream iss(overrides);
    std::string innerPkg;
    std::vector<std::string> runningNoLayerCandidates;  // v4.2.8 heuristic fallback
    while (std::getline(iss, innerPkg)) {
        if (innerPkg.empty() || innerPkg == fgPkg) continue;  // skip container itself
        // v4.2.8 B8: use argv exec instead of shell string (no injection surface).
        std::string pidOut = aatpeExecArgv({"pidof", innerPkg}, "VM_PIDOF");
        if (pidOut.empty()) continue;  // not running on host
        // Check UID of first pid in /proc/<pid>/status.
        long firstPid = 0;
        sscanf(pidOut.c_str(), "%ld", &firstPid);
        if (firstPid <= 0) continue;
        std::string statusPath = "/proc/" + std::to_string(firstPid) + "/status";
        FILE* f = fopen(statusPath.c_str(), "r");
        if (!f) continue;
        char buf[256];
        long innerUid = -1;
        while (fgets(buf, sizeof(buf), f)) {
            if (strncmp(buf, "Uid:", 4) == 0) {
                sscanf(buf + 4, "%ld", &innerUid);
                break;
            }
        }
        fclose(f);
        if (innerUid == containerUid) {
            info.containerPkg = fgPkg;
            info.innerGamePkg = innerPkg;
            info.innerPids = pidOut;
            info.valid = true;
            // v4.2.8 B11: diagnostic — UID match succeeded.
            writeFile((std::string(STATE_DIR) + "/vm_inner_detect_reason").c_str(), "uid_match");
            return info;
        }
        // v4.2.8: heuristic fallback — running on host but UID doesn't match.
        runningNoLayerCandidates.push_back(innerPkg);
    }
    // v4.2.8: heuristic fallback — exactly one running manual game that's not
    // the container itself. Treat it as the inner game.
    if (runningNoLayerCandidates.size() == 1) {
        info.containerPkg = fgPkg;
        info.innerGamePkg = runningNoLayerCandidates[0];
        info.valid = true;
        // v4.2.8 B11: diagnostic — single-candidate heuristic.
        writeFile((std::string(STATE_DIR) + "/vm_inner_detect_reason").c_str(), "single_candidate_heuristic");
    }
    // v4.2.8 B11: 0 or >1 candidates → ambiguous, fail-closed.
    if (!info.valid) {
        writeFile((std::string(STATE_DIR) + "/vm_inner_detect_reason").c_str(),
                  runningNoLayerCandidates.empty() ? "no_candidates" : "ambiguous_multi");
    }
    // Note: innerPids not populated via heuristic (only via UID match).
    // If 0 or >1 candidates: leave valid=false (ambiguous / no inner game found).
    // v4.2.8 B2: store result in cache (FRAME-10: locked, brief critical section).
    {
        std::lock_guard<std::mutex> lk(s_vmDetectMtx);
        s_vmDetectCache[fgPkg] = {info, nowMs};
    }
    return info;
}

// Throttle for `dumpsys gfxinfo <pkg> framestats` (heavy fork ~100-200ms).
// Runs at most once per 5s on the current fg pkg only. Silent fallback on error.
static constexpr int kFramestatsIntervalSec = 5;
static time_t g_framestatsLastRun = 0;

// Split a framestats row into columns. Android 12+ dumpsys gfxinfo emits
// comma-separated rows with NO spaces (e.g. "0,121353,1518...,0,"), while
// older fixtures use whitespace. We detect by presence of ',' and split
// accordingly, trimming whitespace around each token.
static std::vector<std::string> splitFramestatsLine(const std::string& line) {
    std::vector<std::string> cols;
    if (line.find(',') != std::string::npos) {
        std::string cur;
        for (char c : line) {
            if (c == ',') {
                while (!cur.empty() && (cur.front()==' '||cur.front()=='\t')) cur.erase(cur.begin());
                while (!cur.empty() && (cur.back()==' '||cur.back()=='\t')) cur.pop_back();
                cols.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        // trailing empty column after last comma
        while (!cur.empty() && (cur.front()==' '||cur.front()=='\t')) cur.erase(cur.begin());
        while (!cur.empty() && (cur.back()==' '||cur.back()=='\t')) cur.pop_back();
        cols.push_back(cur);
    } else {
        std::istringstream ls(line);
        std::string tok;
        while (ls >> tok) cols.push_back(tok);
    }
    return cols;
}

// Locate column index by header name. Header rows look like:
// "N  Flags  IntendedVsync  Vsync  ...  FrameCompleted  ..."
// or comma-separated. Returns -1 if not found.
static int findHeaderCol(const std::vector<std::string>& header, const char* name) {
    for (size_t i = 0; i < header.size(); i++) {
        if (header[i] == name) return (int)i;
    }
    return -1;
}

// #3-regression fix: derive the FrameCompleted column from a header line.
// A REAL AOSP framestats header has >=14 columns and "FrameCompleted" sits at a
// meaningful index (>=14, among the timing columns). A DEGENERATE marker line such
// as the bare token "FrameCompleted" (1 column, used by host fixtures) must NOT map
// to col 0 — previously it did, which made data-row col0 ("hdr"/"N") parse via
// strtoll as 0 ns and silently skipped EVERY row (0 frames). Returns -1 when the
// header is not a full multi-column AOSP header, forcing positional fallback.
static int headerCompletedCol(const std::vector<std::string>& hdr) {
    int c = findHeaderCol(hdr, "FrameCompleted");
    if ((int)hdr.size() >= 14 && c >= 14) return c;
    return -1;
}

// #3-regression fix: positional fallback for completedNs when no reliable header
// column exists. Walk backward over the row, skipping empty cells and cells that
// are absent / zero / non-numeric, and take the FIRST column that parses as a
// strictly positive integer (strtoll advanced past the start and value > 0). This
// skips trailing "0" and empty cells (rows often end with ",0,") and lands on the
// real FrameCompleted value. Returns -1 if no positive integer cell exists.
static int findLastPositiveIntCol(const std::vector<std::string>& cols) {
    for (int i = (int)cols.size() - 1; i >= 0; i--) {
        if (cols[i].empty()) continue;
        char* endp = nullptr;
        long long v = strtoll(cols[i].c_str(), &endp, 10);
        if (endp != cols[i].c_str() && v > 0) return i;
    }
    return -1;
}

// Parse `dumpsys gfxinfo <pkg> framestats` output into per-frame durations (ms).
// Column layout (AOSP): N Flags IntendedVsync Vsync ... FrameCompleted (ns).
// Duration = FrameCompleted - Vsync. Skip incomplete/background frames.
std::vector<double> parseGfxinfoFramestats(const std::string& out) {
    std::vector<double> durations;
    if (out.empty()) return durations;
    std::istringstream iss(out);
    std::string line;
    bool inTable = false;
    int completedCol = -1;
    while (std::getline(iss, line)) {
        trim(line);
        if (line.empty()) continue;
        if (line.find("FrameCompleted") != std::string::npos) {
            inTable = true;
            auto hdr = splitFramestatsLine(line);
            completedCol = headerCompletedCol(hdr);   // #3 fix: -1 for marker header
            continue;
        }
        if (!inTable) continue;
        if (line[0] == '-' || line[0] == '=') continue;
        auto cols = splitFramestatsLine(line);
        if (cols.size() < 14) continue;
        // Determine completedNs: prefer header-mapped column (only when header was a
        // full AOSP multi-column row); otherwise positional fallback to the last
        // positive-integer cell (skips trailing empty/0).
        std::string completedStr;
        int ccol = completedCol;
        if (!(ccol >= 0 && ccol < (int)cols.size())) {
            ccol = findLastPositiveIntCol(cols);
        }
        if (ccol < 0) continue;
        completedStr = cols[ccol];
        char* endp = nullptr;
        long long flags = strtoll(cols[1].c_str(), &endp, 10);
        long long vsyncNs = strtoll(cols[3].c_str(), &endp, 10);
        long long completedNs = strtoll(completedStr.c_str(), &endp, 10);
        if (completedNs <= 0 || vsyncNs <= 0) continue;
        if (flags & 0x20000000LL) continue;  // deferred/background frame
        long long durNs = completedNs - vsyncNs;
        if (durNs < 0 || durNs > 50000000LL) continue;  // >50ms = bogus
        durations.push_back((double)durNs / 1000000.0);
    }
    return durations;
}

// P0 final: authoritative dropped/jank calculation from gfxinfo framestats.
// Uses IntendedVsync (not display refresh rate) to infer the app's actual target
// frame period (60/90/120 fps), then counts missed vsyncs per AOSP definition.
//
// 口径说明：
// - targetPeriodMs = median(adjacent IntendedVsync gaps) — 引擎锁帧→IntendedVsync间隔即目标周期；
//   想跑120却性能不足→IntendedVsync仍8.3ms而FrameCompleted滞后→正确计丢。
// - missed vsyncs per frame = max(0, floor((FrameCompleted - IntendedVsync - epsilon) / targetPeriod)).
//   epsilon = 0.5ms 容差，避免 off-by-one。帧在 IntendedVsync+targetPeriod 之前呈现算准时。
// - janky frames = late >= 1 target period 的帧数（迟到即 janky，不管跨几个周期）。
// - droppedPct = 累计 missed vsyncs / totalFrames（"每帧平均掉几个vsync"）。
// - jankyPct = jankyFrames / totalFrames（"百分之多少的帧是卡的"）。
struct FramestatsMetrics {
    double fps;              // actual present rate = frames / time span
    double targetPeriodMs;   // inferred target frame period (median of IntendedVsync gaps)
    double droppedPct;       // cumulative missed vsyncs / total frames
    double jankyPct;         // frames late >= 1 target period / total frames
    long totalFrames;
    long missedVsyncs;      // cumulative missed vsync count
    long jankyFrames;       // frames that missed >= 1 target period
    double targetFps;       // inferred target FPS
};

static double medianOfSorted(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 1) return v[n/2];
    return (v[n/2 - 1] + v[n/2]) / 2.0;
}

static constexpr double kLateEpsilonMs = 0.5;  // 0.5ms tolerance for off-by-one

FramestatsMetrics computeFramestatsMetrics(const std::string& out) {
    FramestatsMetrics m{};
    if (out.empty()) return m;
    std::istringstream iss(out);
    std::string line;
    bool inTable = false;
    int completedCol = -1;
    std::vector<long long> intendedVsyncNs;
    std::vector<long long> frameCompletedNs;
    std::vector<long long> flagsList;
    while (std::getline(iss, line)) {
        trim(line);
        if (line.empty()) continue;
        if (line.find("FrameCompleted") != std::string::npos) {
            inTable = true;
            auto hdr = splitFramestatsLine(line);
            completedCol = headerCompletedCol(hdr);   // #3 fix: -1 for marker header
            continue;
        }
        if (!inTable) continue;
        if (line[0] == '-' || line[0] == '=') continue;
        auto cols = splitFramestatsLine(line);
        if (cols.size() < 14) continue;
        std::string completedStr;
        int ccol = completedCol;
        if (!(ccol >= 0 && ccol < (int)cols.size())) {
            ccol = findLastPositiveIntCol(cols);
        }
        if (ccol < 0) continue;
        completedStr = cols[ccol];
        char* endp = nullptr;
        long long flags = strtoll(cols[1].c_str(), &endp, 10);
        long long intendedVs = strtoll(cols[2].c_str(), &endp, 10);  // IntendedVsync
        long long completedNs = strtoll(completedStr.c_str(), &endp, 10);
        if (completedNs <= 0 || intendedVs <= 0) continue;
        if (flags & 0x20000000LL) continue;  // deferred/background frame
        intendedVsyncNs.push_back(intendedVs);
        frameCompletedNs.push_back(completedNs);
        flagsList.push_back(flags);
    }
    int n = (int)intendedVsyncNs.size();
    if (n < 5) return m;  // not enough frames
    m.totalFrames = n;

    // 1. Infer targetPeriodMs from median of adjacent IntendedVsync gaps.
    std::vector<double> gaps;
    for (int i = 1; i < n; i++) {
        double gapMs = (double)(intendedVsyncNs[i] - intendedVsyncNs[i-1]) / 1000000.0;
        if (gapMs > 1.0 && gapMs < 100.0) gaps.push_back(gapMs);  // sanity filter
    }
    m.targetPeriodMs = medianOfSorted(gaps);
    if (m.targetPeriodMs < 4.0 || m.targetPeriodMs > 50.0) m.targetPeriodMs = 16.67;  // fallback
    m.targetFps = 1000.0 / m.targetPeriodMs;

    // 2. Actual FPS = frames / time span.
    double spanMs = (double)(intendedVsyncNs[n-1] - intendedVsyncNs[0]) / 1000000.0;
    if (spanMs > 0) m.fps = (double)(n - 1) * 1000.0 / spanMs;

    // 3. Missed vsyncs + janky frames.
    // missed = max(0, floor((lateMs - epsilon) / targetPeriodMs))
    // janky = missed >= 1
    long long totalMissed = 0;
    long long jankyCount = 0;
    for (int i = 0; i < n; i++) {
        double lateMs = (double)(frameCompletedNs[i] - intendedVsyncNs[i]) / 1000000.0;
        if (lateMs < 0) lateMs = 0;
        double adjLate = lateMs - kLateEpsilonMs;
        if (adjLate < 0) adjLate = 0;
        long long missed = (long long)(adjLate / m.targetPeriodMs);
        totalMissed += missed;
        if (missed >= 1) jankyCount++;
    }
    m.missedVsyncs = totalMissed;
    m.jankyFrames = jankyCount;
    m.droppedPct = (double)totalMissed / (double)n;
    m.jankyPct = (double)jankyCount / (double)n;

    return m;
}

// ============================================================================
// D4: gfxinfo SUMMARY fallback (binder SF + timestats both dead).
//
// On the target (Redmi K40 / Dimensity 1200 / MIUI, root u:r:su:s0) the binder
// SurfaceFlinger handle cannot be obtained and `dumpsys SurfaceFlinger --timestats`
// returns 0 bytes, so both primary frame channels die together. The plain summary
// `dumpsys gfxinfo <fgPkg>` (NO "framestats" arg) DOES work from su and carries a
// self-describing sampling window:
//
//   Stats since: <uptimeNs>ns
//   Total frames rendered: N
//   Janky frames: J (P.PP%)
//   50th percentile: ..ms / 90th percentile: ..ms / 95th percentile: ..ms / 99th percentile: ..ms
//   Number Missed Vsync: M
//   Number Frame deadline missed: D
//
// fps is derived from the window, NOT guessed from the panel:
//   fps = totalFrames / ((nowUptimeNs - statsSinceNs) / 1e9)
// nowUptimeNs must be the SAME monotonic-uptime clock gfxinfo used (Android
// uptimeNanos ~= CLOCK_MONOTONIC = std::chrono::steady_clock on Linux host).
//
// PURE + host-testable (no fork, no file IO). nowUptimeNs is injected so the
// harness controls the elapsed window deterministically.
// ============================================================================
struct GfxinfoSummary {
    bool valid = false;
    long totalFrames = 0;
    double jankyPct = 0.0;     // 0..1 (from "Janky frames: X (Y%)")
    double fps = 0.0;          // actual present rate over the stats window
    long long missedVsyncs = 0;
    long long deadlineMissed = 0;
    double p50ms = 0.0, p90ms = 0.0, p95ms = 0.0, p99ms = 0.0;
    long long statsSinceNs = 0;
};

static inline long long aatpeUptimeNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

GfxinfoSummary parseGfxinfoSummary(const std::string& out, long long nowUptimeNs) {
    GfxinfoSummary g;
    if (out.empty()) return g;
    std::istringstream iss(out);
    std::string line;
    long long statsSinceNs = 0;
    long totalFrames = 0;
    double jankyPct = -1.0;
    while (std::getline(iss, line)) {
        std::string t = line;
        trim(t);
        if (t.empty()) continue;
        size_t colon = t.find(':');
        if (colon == std::string::npos) continue;
        std::string rhs = t.substr(colon + 1);
        if (t.rfind("Stats since:", 0) == 0) {
            statsSinceNs = (long long)aatpeFirstFloat(rhs);   // "1234567890123ns"
        } else if (t.rfind("Total frames rendered:", 0) == 0) {
            totalFrames = (long)aatpeFirstInt(rhs);
        } else if (t.rfind("Janky frames:", 0) == 0) {
            // "Janky frames: 56 (4.54%)" -> pull the parenthesized percent
            size_t lp = t.find('(');
            if (lp != std::string::npos) {
                std::string pct = t.substr(lp + 1);
                size_t rp = pct.find('%');
                if (rp != std::string::npos) pct = pct.substr(0, rp);
                jankyPct = aatpeFirstFloat(pct) / 100.0;
            }
        } else if (t.rfind("50th percentile:", 0) == 0) {
            g.p50ms = aatpeFirstFloat(rhs);
        } else if (t.rfind("90th percentile:", 0) == 0) {
            g.p90ms = aatpeFirstFloat(rhs);
        } else if (t.rfind("95th percentile:", 0) == 0) {
            g.p95ms = aatpeFirstFloat(rhs);
        } else if (t.rfind("99th percentile:", 0) == 0) {
            g.p99ms = aatpeFirstFloat(rhs);
        } else if (t.rfind("Number Missed Vsync:", 0) == 0) {
            g.missedVsyncs = (long long)aatpeFirstInt(rhs);
        } else if (t.rfind("Number Frame deadline missed:", 0) == 0) {
            g.deadlineMissed = (long long)aatpeFirstInt(rhs);
        }
    }
    g.statsSinceNs = statsSinceNs;
    g.totalFrames = totalFrames;
    if (jankyPct >= 0.0) g.jankyPct = jankyPct;
    // Need a real sampling window with enough frames; otherwise report invalid so
    // the caller falls through (and does NOT falsely claim a "gfxinfo" vote).
    if (totalFrames < 5 || statsSinceNs <= 0 || nowUptimeNs <= statsSinceNs) return g;
    double elapsedSec = (double)(nowUptimeNs - statsSinceNs) / 1e9;
    if (elapsedSec <= 0.0) return g;
    g.fps = (double)totalFrames / elapsedSec;
    g.valid = true;
    return g;
}

// D4: is a parsed gfxinfo summary good enough to REPORT as the frame source
// (instead of "none")? Pure + host-testable. Guards the same floor aatpeSetFrameJank
// uses (>=5 samples) plus a sane fps so a degenerate one-shot summary can't vote.
bool aatpeGfxinfoSummaryUsable(const GfxinfoSummary& g) {
    return g.valid && g.totalFrames >= 5 && g.fps > 1.0;
}

void aatpeSetFrameJank(const char* source, int totalSamples, double missRate,
                      double avgFps, double p90Fps, double p99Fps) {
    std::string src = source ? source : "none";
    writeFile((std::string(STATE_DIR) + "/frame_source").c_str(), src.c_str());
    if (src == "none" || totalSamples < 5) {
        // 无数据/样本不足：不作为 jank 输入。绝不凭"没数据"直接落票——
        // g_jankBoostVote 唯一由融合迟滞块写。这里只清输入信号。
        aatpe::frameaware::g_frameMissJankInput.store(false, std::memory_order_relaxed);
        return;
    }
    // F-07: clamp missRate to [0,1] before writing percentage state key.
    if (missRate < 0.0) missRate = 0.0;
    if (missRate > 1.0) missRate = 1.0;
    // P1-3: override old keys with authoritative per-app framestats metrics if available.
    // This ensures game_avg_fps/game_frame_miss match the new frame_apps panel.
    // v4.2.8 B1: use authoritative attribution key (not raw fgPkg) — for VM games
    // the data is stored under inner game pkg, not container pkg.
    std::string authFg, authSrc, authRsn;
    frameGetAuthSnapshot(authFg, authSrc, authRsn);
    double outFps = avgFps;
    double outMissPct = missRate * 100.0;
    {
        std::lock_guard<std::mutex> lk(g_perAppMutex);
        auto fsIt = g_perAppFsMetrics.find(authFg);
        if (fsIt != g_perAppFsMetrics.end() && fsIt->second.totalFrames >= 5) {
            outFps = fsIt->second.fps;
            outMissPct = fsIt->second.jankyPct * 100.0;
        }
    }
    writeFile((std::string(STATE_DIR) + "/game_frame_miss").c_str(), (int)lround(outMissPct));
    writeFile((std::string(STATE_DIR) + "/game_avg_fps").c_str(), (int)lround(outFps));
    writeFile((std::string(STATE_DIR) + "/game_p90_fps").c_str(), (int)lround(p90Fps));
    writeFile((std::string(STATE_DIR) + "/game_p99_fps").c_str(), (int)lround(p99Fps));
    // P2-fix: missRate>=0.05 is an INPUT to the single fusion voter, NOT a direct vote.
    // The old code stored g_jankBoostVote here (no hysteresis), which let a single
    // timestats/binder window arm/disarm and thrash placement for SurfaceView games.
    bool missJank = (missRate >= 0.05);
    aatpe::frameaware::g_frameMissJankInput.store(missJank, std::memory_order_relaxed);

    // v4.2.1 T3: per-app rolling window. Use aatpeGetForegroundPackage() (takes
    // g_foregroundMutex) for a race-free snapshot of the current fg package.
    {
        std::string pkg = aatpeGetForegroundPackage();
        if (pkg.empty() || pkg == "unknown") return;
        long long nowMs = frameNowMs();  // R6: monotonic, immune to wall-clock rollback
        std::lock_guard<std::mutex> lk(g_perAppMutex);
        auto it = g_perAppWindows.find(pkg);
        if (it == g_perAppWindows.end()) {
            if (g_perAppWindows.size() >= kMaxPerAppStats) {
                std::string old = g_perAppLru.back(); g_perAppLru.pop_back();
                g_perAppWindows.erase(old);
                g_perAppFrameTimes.erase(old);  // P2-2: also free per-frame ring
                g_perAppFsMetrics.erase(old);   // R5: bound per-app maps together
                evictTsLayerSnapForPkgLocked(old);  // FRAME-01
            }
            g_perAppLru.push_front(pkg);
        } else {
            g_perAppLru.remove(pkg);
            g_perAppLru.push_front(pkg);
        }
        auto& ring = g_perAppWindows[pkg];
        PerAppWindow w;
        w.fps = avgFps;
        w.p99Fps = p99Fps;
        w.samples = totalSamples;
        w.dropped = (long)(missRate * totalSamples);
        w.jankArmed = missJank;   // per-window marker; NOT a direct boost vote
        w.tsMs = nowMs;
        ring.push_back(w);
        while (ring.size() > kMaxWindowsPerApp) ring.pop_front();
    }
}

// C-001: the global SF-latency channel (sfLatValid) yields NO timestats/binder
// miss-rate sample. Symmetrically clear the stale missJank input ("无数据即清票")
// so a previous timestats/binder missJank=true cannot keep fusion anyJank pinned
// and starve the release hysteresis (boost ticket latch). Also called from the
// screen-off suspend gate (C-003). Non-static so the host regression test drives
// the exact store the production channel branches make.
void aatpeFrameClearMissJankInput() {
    aatpe::frameaware::g_frameMissJankInput.store(false, std::memory_order_relaxed);
}

// C-002: a framestats parse that produced <5 frames is NOT a jank signal.
// Symmetrically clear the app-framestats jank vote so it does not latch true after
// the foreground moves to a low-frame app (launcher / static page / just-switched).
// Non-static for the host regression test.
void aatpeFrameClearAppFsJank() {
    aatpe::frameaware::g_appFsJankActive.store(false, std::memory_order_relaxed);
}

// FRAME-02: when the global SF-latency channel (sfLatValid=true) is live, the
// !sfLatValid SurfaceView sampler block is skipped, so aatpeSetFrameJank is NOT
// called and the game_* state files (game_avg_fps / game_frame_miss / game_hz /
// game_p90_fps / game_p99_fps) retain values from the PREVIOUS SurfaceView game.
// After exiting a game to the launcher/View UI, the WebUI keeps showing the
// game's 85fps / 2% miss — exactly the "帧丢失0% 与 实际FPS 矛盾" class of
// stale-buffer false positive. Write zeros so the panel reflects "no active
// SurfaceView game frame this cycle". The per-app panel (aatpe_getPerAppFrameStats)
// still gets real fps from g_perAppFsMetrics (populated by the throttled
// `dumpsys gfxinfo <fgPkg> framestats` block below).
void aatpeFrameClearGameStats() {
    writeFile((std::string(STATE_DIR) + "/game_avg_fps").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/game_frame_miss").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/game_p90_fps").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/game_p99_fps").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/game_hz").c_str(), 0);
    aatpe::frameaware::g_frameMissJankInput.store(false, std::memory_order_relaxed);
}

int aatpe_getPerAppFrameStats(std::vector<AatpePerAppFrameStat>& out,
                               std::string& currentPkg) {
    // v4.3.1 (RC1/诉求1): the serialized "current" MUST track the authoritative
    // foreground cascade (aatpeGetForegroundPackage = cgroup->dumpsys->oom, RC1-fixed),
    // not only the auth snapshot. The auth snapshot carries source/reason (how fg was
    // decided) but it is refreshed once per readSurfaceFlinger cycle; aatpeSetFrameJank
    // can already have written a window for the freshly-switched foreground before the
    // snapshot refreshes. Trusting only the snapshot made the UI "current" lag/stall on
    // the OLD package — the same mis-attribution class as RC1. We therefore take the
    // authoritative fg for currentPkg, and keep the auth snapshot's source/reason.
    std::string src, reason;
    frameGetAuthSnapshot(currentPkg, src, reason);
    std::string authoritativeFg = aatpeGetForegroundPackage();
    if (!authoritativeFg.empty() && authoritativeFg != "unknown") {
        currentPkg = authoritativeFg;
    }
    long long nowMs = frameNowMs();  // R6: monotonic age reference (matches window tsMs)
    // v4.4.1 P0 (RC-2b): monotonic->epoch baseline. Window tsMs is boot-relative
    // (steady_clock); the UI age display subtracts from Date.now() (epoch). Feeding
    // a monotonic ms into an epoch subtraction produced "497725 hours ago"
    // (= epoch_now - uptime_ms ~= 56 years). Correct conversion:
    //   lastSeenEpoch = bootEpochMs + lastFrameMonotonicTs,
    // where bootEpochMs = system_now - steady_now. Computed once per read.
    long long nowSysMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    long long nowSteadyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long bootEpochMs = nowSysMs - nowSteadyMs;
    // v4.3.1 (诉求2): snapshot the manual list BEFORE taking g_perAppMutex (avoids a
    // lock-order inversion with g_ovMutex). Every manual-list package must appear in the
    // panel, even if it never ran or is currently backgrounded.
    std::vector<std::string> manualList = gamelist::manualGameListSnapshot();
    std::set<std::string> manualSet(manualList.begin(), manualList.end());
    std::lock_guard<std::mutex> lk(g_perAppMutex);
    out.clear();
    out.reserve(g_perAppWindows.size() + manualList.size());
    std::set<std::string> emitted;
    for (const auto& pkg : g_perAppLru) {
        auto it = g_perAppWindows.find(pkg);
        if (it == g_perAppWindows.end()) continue;
        const auto& ring = it->second;
        if (ring.empty()) continue;
        long long ageMs = nowMs - ring.back().tsMs;
        bool onManual = manualSet.count(pkg) > 0;
        bool stale = ageMs > 60000;
        // P1-4: drop packages not updated in >60s UNLESS they are on the manual list —
        // a manual package that merely went to the background must still stay visible
        // (reason="background") during the keepalive poll, not vanish from the panel.
        if (stale && !onManual) continue;
        AatpePerAppFrameStat st;
        st.pkg = pkg;
        // Sample-weighted aggregates over the rolling window.
        long long totSamples = 0, totDropped = 0, jankWindows = 0;
        double wsumFps = 0.0, wsumP99 = 0.0;
        for (const auto& w : ring) {
            totSamples += w.samples;
            totDropped += w.dropped;
            if (w.jankArmed) jankWindows++;
            wsumFps += w.fps * w.samples;
            wsumP99 += w.p99Fps * w.samples;
        }
        st.samples = totSamples;
        st.dropped = totDropped;
        st.jank = (long)jankWindows;
        st.fps = (totSamples > 0) ? (wsumFps / totSamples) : 0.0;
        double wP99 = (totSamples > 0) ? (wsumP99 / totSamples) : 0.0;
        st.ft_avg_ms = (st.fps > 1.0) ? (1000.0 / st.fps) : 0.0;
        st.ft_p99 = (wP99 > 1.0) ? (1000.0 / wP99) : 0.0;
        // Real per-frame percentiles from framestats ring (if available).
        auto ftIt = g_perAppFrameTimes.find(pkg);
        if (ftIt != g_perAppFrameTimes.end() && !ftIt->second.empty()) {
            // P2-1: filter out >250ms long gaps (loading/background) from percentiles.
            std::vector<double> filtered;
            for (double d : ftIt->second) {
                if (d <= 250.0) filtered.push_back(d);
            }
            if (filtered.empty()) filtered.assign(ftIt->second.begin(), ftIt->second.end());
            std::sort(filtered.begin(), filtered.end());
            size_t n = filtered.size();
            st.ft_p50 = filtered[(size_t)(n * 0.50)];
            st.ft_p95 = filtered[(size_t)(n * 0.95)];
            // p99: prefer real framestats; fall back to SF-histogram p99.
            double fsP99 = filtered[(size_t)(n * 0.99)];
            st.ft_p99 = (fsP99 > 0.5) ? fsP99 : st.ft_p99;
        } else {
            st.ft_p50 = 0.0;
            st.ft_p95 = 0.0;
            // ft_p99 already set from SF histogram above.
        }
        // v4.2.6: override fps/jank with authoritative framestats metrics if available.
        auto fsIt = g_perAppFsMetrics.find(pkg);
        const PerAppFsMetrics* fmPtr =
            (fsIt != g_perAppFsMetrics.end() && fsIt->second.totalFrames >= 5)
                ? &fsIt->second : nullptr;
        if (fmPtr) {
            const auto& fm = *fmPtr;
            st.fps = fm.fps;               // actual present rate from IntendedVsync
            st.target_fps = fm.targetFps;  // inferred target frame rate
            // rel12 Bug1: "丢帧 dropped" = real janky/missed FRAMES and MUST be
            // <= total_frames. fm.missedVsyncs is the raw vsync accumulator -- one
            // badly-late frame can miss MANY vsyncs, so it could exceed the frame
            // count and produced the impossible "丢帧574 / 总帧242". Keep the raw
            // vsync count in st.missed_vsyncs for the detail panel.
            long jankyFrames = (long)(fm.jankyPct * fm.totalFrames);
            if (jankyFrames < 0) jankyFrames = 0;
            if (jankyFrames > (long)fm.totalFrames) jankyFrames = (long)fm.totalFrames;
            st.jank = jankyFrames;
            st.dropped = jankyFrames;  // janky/missed frames (<= total_frames)
            st.missed_vsyncs = fm.missedVsyncs;
            st.total_frames = fm.totalFrames;  // total rendered frames (denominator for jank%)
            st.frameSource = "timestats";  // SF per-layer present frames (self-render games)
        }
        // rel12 Bug2: active_ms must reflect real foreground-rendered time. The old
        // code only summed <=4s gaps between sampling windows, so a freshly-opened
        // app with a single window always rendered "前台 0ms" even though it had been
        // actively presenting. Prefer the authoritative framestats rendering span
        // (totalFrames / present-rate) when available; fall back to the contiguous
        // window-gap sum (which telescopes to first..last for one session).
        long long activeMs = 0;
        for (size_t i = 1; i < ring.size(); i++) {
            long long gap = ring[i].tsMs - ring[i-1].tsMs;
            if (gap > 0 && gap <= kActiveGapMaxMs) activeMs += gap;
        }
        if (fmPtr && fmPtr->fps > 1.0 && fmPtr->totalFrames > 0) {
            long long fsSpan = (long long)((double)fmPtr->totalFrames * 1000.0 / fmPtr->fps);
            if (fsSpan > activeMs) activeMs = fsSpan;
        }
        st.active_ms = activeMs;
        st.last_update_ms = ring.back().tsMs;
        // v6.0.0 P1 (realdevice FPS 0.1/0.2): a backgrounded app keeps its old
        // timestats ring, and IntendedVsync span covers the whole former foreground
        // session, diluting fps to a meaningless tiny value (0.1/0.2). If the app
        // has NOT rendered a frame in the last 5s (ageMs>5000), blank the
        // user-facing fps and tag reason=background_stale so WebUI renders "--"
        // instead of a scary sub-1 number. We deliberately do NOT blank on
        // activeMs==0 alone: a brand-new app with a single window also has
        // activeMs==0 but its last frame is fresh, so the staleness clock (ageMs)
        // is the reliable "not currently rendering" signal. Keep samples/dropped/
        // lastSeen raw data for inspection; we only hide the fps field.
        if (ageMs > 5000) {
            st.fps = 0.0;
            st.ft_avg_ms = 0.0;
            st.reason = "background_stale";
        }
        // v4.3.1 (诉求2): honest per-entry state.
        if (st.reason.empty()) st.reason = stale ? "background" : "";
        st.isCurrent = (pkg == currentPkg);
        st.lastSeenMs = ring.back().tsMs;
        // v4.4.1 P0 (RC-2b): epoch of the LAST FRAME (not serialization time). The
        // old code set this to system_clock::now(), so every observed row always
        // showed "刚刚" and never reflected real staleness. Convert the monotonic
        // window tsMs to epoch via the per-read bootEpochMs baseline.
        st.lastSeenEpochMs = bootEpochMs + ring.back().tsMs;
        emitted.insert(pkg);
        out.push_back(std::move(st));
    }
    // v4.3.1 (诉求2): emit a stub row for every manual-list package we have NEVER observed
    // a frame for, so the panel shows it (fps/jank=0, source=none, reason=waiting) rather
    // than silently omitting it. Removed from the list -> it no longer appears.
    for (const auto& mpkg : manualList) {
        if (emitted.count(mpkg)) continue;
        AatpePerAppFrameStat st;
        st.pkg = mpkg;
        st.fps = 0.0;
        st.jank = 0;
        st.reason = "waiting_first_sample";
        st.frameSource = "none";
        st.isCurrent = (mpkg == currentPkg);
        st.lastSeenMs = 0;
        st.lastSeenEpochMs = -1;   // v4.4.1 P0: explicit sentinel -> UI shows no age
        out.push_back(std::move(st));
    }
    return (int)out.size();
}

void readSurfaceFlinger() {
    // P1-1: snapshot g_cfg under g_cfgMutex — main loop calls this while the
    // config inotify thread may be writing g_cfg. Use the local snapshot for
    // all g_cfg field reads below to avoid torn reads.
    Config sfCfg;
    snapshotConfig(sfCfg);
    // v4.3.1 hotfix: idempotently start the detached label-warmup background
    // thread BEFORE the frameAware early-return, so labels warm even when frame
    // awareness is off. The thread itself never blocks this decision loop.
    aatpeStartLabelWarmupThread();
    if (!sfCfg.frameAware) return;
    int rate = aatpeDetectRefreshRate();
    writeFile((std::string(STATE_DIR) + "/vsync_rate").c_str(), rate);
    int maxLat = 0;
    bool sfLatValid = false;
    int avgLat = 0;
    // Cache SF latency detection for 20ms (fast jank detection for emergency migration) to reduce dumpsys overhead.
    // SF latency changes slowly relative to 50ms decision interval, so caching
    // is safe and reduces per-cycle overhead by 5-30ms (dumpsys SurfaceFlinger).
    static int s_cachedAvgLat = 0;
    static int s_cachedMaxLat = 0;
    static bool s_cachedSfLatValid = false;
    static auto s_lastSfLatCheck = std::chrono::steady_clock::now();
    auto nowSf = std::chrono::steady_clock::now();
    int sfLatCacheMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowSf - s_lastSfLatCheck).count();
    // protect SF latency static cache with mutex
    {
        std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
        if (s_cachedSfLatValid && sfLatCacheMs < 20) {
            avgLat = s_cachedAvgLat;
            maxLat = s_cachedMaxLat;
            sfLatValid = s_cachedSfLatValid;
        } else {
            avgLat = aatpeDetectSFLatency(rate, maxLat, sfLatValid);
            s_cachedAvgLat = avgLat;
            s_cachedMaxLat = maxLat;
            s_cachedSfLatValid = sfLatValid;
            s_lastSfLatCheck = nowSf;
        }
    }
    writeFile((std::string(STATE_DIR) + "/sf_latency_avg").c_str(), avgLat);
    writeFile((std::string(STATE_DIR) + "/sf_latency_max").c_str(), maxLat);
    // Write sf_latency_valid for diagnostics.
    // When this is 0, jank detection is permanently disabled. Allows users to
    // verify whether SurfaceFlinger latency detection is actually working.
    writeFile((std::string(STATE_DIR) + "/sf_latency_valid").c_str(), sfLatValid ? 1 : 0);
    int layers = 0;
    {
        // P0-2: Cache layer count for 2 seconds (only uncached high-frequency popen)
        static int s_cachedLayers = 0;
        static time_t s_layerCacheTime = 0;
        time_t now2 = time(nullptr);
        // protect layer count static cache with mutex.
        // Check cache under lock, do expensive popen outside lock.
        {
            std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
            if (s_layerCacheTime > 0 && (now2 - s_layerCacheTime) < 2) {
                layers = s_cachedLayers; // cache hit: skip popen
            }
        }
        if (layers == 0) {
            std::string lout = aatpeExec("dumpsys SurfaceFlinger --list 2>/dev/null", "LAYER_COUNT");
            if (!lout.empty()) {
                std::istringstream iss(lout);
                std::string ln;
                while (std::getline(iss, ln)) {
                    trim(ln);
                    if (ln.empty()) continue;
                    bool skip = false;
                    if (ln.size() >= 2 && ln[0] == '0' && (ln[1] == 'x' || ln[1] == 'X')) skip = true;
                    if (!skip) {
                        bool allDigits = true;
                        for (char c : ln) { if (!(c >= '0' && c <= '9')) { allDigits = false; break; } }
                        if (allDigits) skip = true;
                    }
                    if (!skip) {
                        const char* skipKw[] = {"Region", "Buffer", "HWC", "name=", "type=", "size=", "format="};
                        for (const char* kw : skipKw) {
                            if (ln.find(kw) != std::string::npos) { skip = true; break; }
                        }
                    }
                    if (!skip) layers++;
                }
            }
            // write layer cache under mutex
            {
                std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
                s_cachedLayers = layers;
                s_layerCacheTime = now2;
            }
        }
    }
    writeFile((std::string(STATE_DIR) + "/layer_count").c_str(), layers);
    // (问题38): TARGET_FRAME_TIME now enters core decision.
    // P1-1: Use actual frame time when refresh rate is valid; fallback to
    // config baseline only when refresh rate detection fails. Previously used
    // max(actual, target) which always returned 16667 (target) for 120Hz devices,
    // making target_frame_time and jank threshold incorrect.
    // G5-008: when the config targetFrameTime is absent, derive the fallback
    // from the DETECTED refresh rate instead of hardcoding 60Hz (16667us). A 120Hz
    // device with a missing config baseline previously got a 2x-too-loose jank
    // threshold; only fall back to 16667 when rate detection also fails.
    int targetFrameTime;
    if (sfCfg.targetFrameTime > 0) {
        targetFrameTime = sfCfg.targetFrameTime;
    } else if (rate >= refresh::kMinRate && rate <= refresh::kMaxRate) {
        targetFrameTime = (refresh::kOneSecondUs + rate / 2) / rate;
    } else {
        targetFrameTime = 16667;  // last-resort 60Hz assumption
    }
    // 问题4 fix: per-app maxFpsHint override. When the foreground package has a
    // [app:pkg] section with max_fps=N, clamp the target frame time to 1e6/N us
    // so the jank / frame-critical thresholds and the target_frame_time state
    // file reflect the app-specific cap instead of the global baseline.
    if (g_currentAppConfig.valid && g_currentAppConfig.maxFpsHint > 0) {
        int appTargetUs = 1000000 / g_currentAppConfig.maxFpsHint;
        if (appTargetUs > 0) targetFrameTime = appTargetUs;
    }
    // Integer-divide with rounding to nearest (avoids truncation bias for
    // non-divisible refresh rates, e.g. 90Hz -> 11111us instead of 11111us).
    int actualFrameTime = (rate >= refresh::kMinRate && rate <= refresh::kMaxRate)
        ? (refresh::kOneSecondUs + rate / 2) / rate : targetFrameTime;
    int frameTimeUs = (rate >= refresh::kMinRate && rate <= refresh::kMaxRate) ? actualFrameTime : targetFrameTime;
    int dynamicThreshold = (int)(frameTimeUs * refresh::kFrameCriticalMultiplier);
    int effectiveThreshold = (dynamicThreshold > sfCfg.frameCriticalThreshold) ? dynamicThreshold : sfCfg.frameCriticalThreshold;
    // Resolve foreground package + is-a-game gate UP FRONT so the global-SF-latency
    // alarms below can be suppressed when there is no performance-sensitive
    // foreground surface (idle launcher/SystemUI). This is the same resolution the
    // per-layer game sampler uses later in this function; moved here, not duplicated.
    std::string fgPkg = aatpeGetForegroundPackage();
    if (fgPkg.empty() || fgPkg == "unknown") {
        std::string manualFg = gamelist::detectManualGameForeground();
        if (!manualFg.empty()) fgPkg = manualFg;
    }
    const bool fgIsGame = !fgPkg.empty() && fgPkg != "unknown" &&
                          (aatpe::gamelist::isManualGame(fgPkg) ||
                           isPackageInGameList(fgPkg));
    // P0-2 / real-device false positive: the GLOBAL SurfaceFlinger latency tracks
    // the ViewRoot/HWUI main layer and is "almost always valid" even on an idle
    // launcher. Comparing it to the PANEL frame budget there is meaningless (a 60fps
    // launcher legitimately reads ~13ms on a 120Hz panel whose budget is 8.33ms),
    // which is exactly the idle 05:43 screenshot: frame_critical=是 / jank=是 at 13ms
    // while CPU was only 76°C. Gate the latency alarms on fgIsGame; layerCritical
    // (raw layer-count resource pressure) stays ungated.
    aatpe::frameaware::FrameLatencyAlarm fa = aatpe::frameaware::aatpeFrameLatencyAlarm(
        sfLatValid, avgLat, maxLat, effectiveThreshold, frameTimeUs,
        sfCfg.jankThresholdPercent, fgIsGame);
    bool latCritical = fa.latCritical;
    bool layerCritical = (layers > sfCfg.layerCriticalThreshold);
    bool frameCritical = latCritical || layerCritical;
    bool jankDetected = fa.jank;
    writeFile((std::string(STATE_DIR) + "/jank_detected").c_str(), jankDetected ? 1 : 0);

    // ROUND-007c #6: sustained-jank bounded boost vote. Google official jank =
    // p95 frame time > budget (16ms @60Hz) or P99 overrun. jankDetected already
    // compares against the device frameTimeUs budget. Require N consecutive
    // jank samples to debounce a single dropped frame; then arm a bounded boost
    // vote that placement/spillover merges (NOT stacked) into the next critical-
    // thread promotion. No global min_freq raise; EMERGENCY/thermal still gates it.
    // FIX(v4.3.1, RC3): exit hysteresis. Arm after kJankBoostConsecutive jank samples,
    // but require MORE consecutive clean cycles before disarming, so a single clean
    // cycle (or the 50ms latency/framestats sampling jitter) does not oscillate the
    // vote and thrash thread placement. The enter/exit rule lives in the PURE
    // aatpeFusionJankVote() (hermetically tested in tests/v94/test_per_app_frame.cpp).
    static constexpr int kJankBoostReleaseConsecutive = 4;
    static int s_jankConsecutive = 0;
    static int s_jankClearStreak = 0;
    // Multi-signal fusion: latency jank (above) OR per-app framestats jank/low-fps
    // OR the timestats/binder missRate input (P2-fix). g_jankBoostVote is written ONLY
    // here, through the hysteresis pure function — no second writer remains.
    bool appFsJank = aatpe::frameaware::g_appFsJankActive.load(std::memory_order_relaxed);
    bool missJank = aatpe::frameaware::g_frameMissJankInput.load(std::memory_order_relaxed);
    bool anyJank = jankDetected || appFsJank || missJank;
    bool prevVote = aatpe::frameaware::g_jankBoostVote.load(std::memory_order_relaxed);
    bool newVote = aatpe::frameaware::aatpeFusionJankVote(
        anyJank, s_jankConsecutive, s_jankClearStreak,
        aatpe::frame_latency::kJankBoostConsecutive, kJankBoostReleaseConsecutive, prevVote);
    aatpe::frameaware::g_jankBoostVote.store(newVote, std::memory_order_relaxed);
    writeFile((std::string(STATE_DIR) + "/jank_boost_vote").c_str(),
              newVote ? 1 : 0);
    // (问题40) + v95-fix: SAFETY_LATENCY_LIMIT gates placement via an
    // INDEPENDENT atomic (g_safetyLatencyActive), NOT by overwriting
    // g_cfg.controlMode. The old code forced controlMode="passive" on breach but
    // never restored it on recovery, so the daemon got stuck in passive mode
    // (and clobbered the user's manually selected mode). Now the breach only
    // latches this flag; placement.cpp gates migrations on it (same position as
    // the thermal-emergency gate). On recovery the flag clears and normal
    // scheduling resumes with the user's original controlMode intact.
    bool safetyLatencyBreached = false;
    if (sfLatValid && sfCfg.safetyLatencyLimit > 0 && avgLat > sfCfg.safetyLatencyLimit) {
        safetyLatencyBreached = true;
        if (!g_safetyLatencyActive.load(std::memory_order_relaxed)) {
            LOG_WARN("SAFETY LATENCY LIMIT: avgLat=%dus > limit=%dus. Forcing no-migration (placement gated).",
                     avgLat, sfCfg.safetyLatencyLimit);
            g_safetyLatencyActive.store(true, std::memory_order_relaxed);
        }
        writeFile((std::string(STATE_DIR) + "/safety_latency_active").c_str(), 1);
    } else {
        // Clear safety latency flag when latency recovers
        g_safetyLatencyActive.store(false, std::memory_order_relaxed);
        writeFile((std::string(STATE_DIR) + "/safety_latency_active").c_str(), 0);
    }
    // P1-2: g_frameCritical is std::atomic<bool> (declared in aatped_state.h,
    // defined in aatped_main.cpp). Watchdog reads it cross-thread; use
    // explicit .load()/.store() to match g_safetyLatencyActive style and
    // avoid relying on implicit atomic->bool conversion (which is legal but
    // easy to miss in review).
    bool prevCritical = g_frameCritical.load(std::memory_order_relaxed);
    if (frameCritical && !prevCritical) {
        g_frameCritical.store(true, std::memory_order_relaxed);
        LOG_WARN("FRAME CRITICAL: latency=%dus layers=%d jank=%d safety=%d",
                 avgLat, layers, jankDetected ? 1 : 0, safetyLatencyBreached ? 1 : 0);
    } else if (!frameCritical && prevCritical) {
        g_frameCritical.store(false, std::memory_order_relaxed);
        FRAME_LOG_INFO("Frame critical cleared: latency=%dus layers=%d", avgLat, layers);
    }
    writeFile((std::string(STATE_DIR) + "/frame_critical").c_str(),
              g_frameCritical.load(std::memory_order_relaxed) ? 1 : 0);
    // P1-2: Write actual dynamic frame time (frameTimeUs), not config baseline (targetFrameTime).
    // 120Hz device: frameTimeUs=8333us (was incorrectly showing 16667us=60Hz).
    writeFile((std::string(STATE_DIR) + "/target_frame_time").c_str(), frameTimeUs);

    // WS-C2: game SurfaceView frame sampling (R3).
    //
    // Trigger: the gfxinfo path produced no valid frame latency (sfLatValid==0),
    // which is exactly the SurfaceView-game case (gfxinfo has no app rows,
    // FrameTimeline does not support SurfaceView). We then enumerate
    // `dumpsys SurfaceFlinger --list`, pick the game SurfaceView layer for the
    // foreground package, and read its per-layer `--latency`. The refresh rate is
    // taken from the layer's first line (LTPO dynamic) — never a hardcoded 16.6ms.
    //
    // Fail-closed: if there is no game layer, the command fails, or every row is
    // invalid, we set frame_unavailable=1 and DO NOT cast a jank vote on missing
    // data — the cycle trusts cluster pressure / input events only.
    // ROUND-014 P3: the timestats session is a SINGLE function-scope static,
    // shared by the screen-off stop() path and the screen-on tick() path.
    // (Two sibling-block statics would have stopped instance A while tick ran
    // on instance B -> the real session never got -disable on screen-off.)
    static aatpe::frameaware::TimestatsSession s_tsSession;
    // C-003 (screen-off red line): the screen-off suspend applies to EVERY frame
    // channel (timestats / binder / gfxinfo-summary / global SF-latency), not only
    // the !sfLatValid branch. On screen-off / idle we do NOT fork dumpsys (would
    // burn power and read a stale SurfaceFlinger state): we stop the timestats
    // session, report frame_source=none, and clear every jank input signal so the
    // fusion release-hysteresis counts cleanly (no boost latch). On screen-on the
    // session restarts fresh (no TTL carryover issue because -enable re-arms).
    // fb0/blank: 0=on,1=off. Missing -> assume on (fail-open for collection, not
    // for voting).
    static auto s_lastScreenCheck = std::chrono::steady_clock::now();
    static bool s_screenOn = true;
    auto nowSc = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(nowSc - s_lastScreenCheck).count() >= 2) {
        s_lastScreenCheck = nowSc;
        int blank = 0;
        FILE* f = fopen("/sys/class/graphics/fb0/blank", "r");
        if (f) { if (fscanf(f, "%d", &blank) != 1) blank = 0; fclose(f); }
        s_screenOn = (blank == 0);
    }
    if (!s_screenOn) {
        s_tsSession.stop([](const std::string& c) { return aatpeExec(c.c_str(), "SF_TS_STOP"); });
        // 无数据即清票: screen-off yields no fresh frame data on ANY channel.
        // aatpeSetFrameJank("none") clears the timestats/binder missJank input;
        // g_appFsJankActive is cleared symmetrically so the fusion voter's release
        // streak can run down instead of being pinned by a stale vote. Returning
        // here also skips the tail `dumpsys gfxinfo <pkg> framestats` fork below.
        aatpeSetFrameJank("none", 0, 0.0, 0.0, 0.0, 0.0);
        aatpeFrameClearAppFsJank();
        // v4.2.8: authoritative snapshot (screen off path).
        std::string curFg = aatpeGetForegroundPackage();
        frameSetAuthSnapshot(curFg.empty() ? "unknown" : curFg, "none", "screen_off");
        writeFile((std::string(STATE_DIR) + "/frame_unavailable").c_str(), 1);
        return;
    }

    // F-01: fgPkg / fgIsGame were resolved UP FRONT (just above the frame-critical
    // vote) so the global-SF-latency alarms could be gated on "is this a game".
    // They are reused here to gate the per-layer game sampler: a game must ALWAYS
    // get its per-layer SurfaceView FPS measured, even when the global binder
    // --latency (ViewRoot/HWUI main layer, always looks "valid" at 5-15ms) reports
    // sfLatValid. Without this, a SurfaceView game was short-circuited into the
    // else-branch, its per-layer channel never ran, and gfxinfo has no SurfaceView
    // app rows -> 5s later background_stale -> WebUI showed 0 / single-digit fps.
    if (!sfLatValid || fgIsGame) {
        // Channel 1: timestats (SurfaceView-aware, per P2 research). On success
        // it is the authoritative source; on failure/unavailable we fall through
        // to binder -> dumpsys latency. (fgPkg resolved above the gate, F-01.)
        // F-05: foreground package changed -> drop cached --list output and reset
        // the timestats TTL so a stale block from the old app can't vote.
        static std::string s_lastFgPkg;
        static std::string s_cachedListOut;
        static time_t s_listCacheTime = 0;
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
        if (s_lastFgPkg != fgPkg) {
            s_lastFgPkg = fgPkg;
            s_cachedListOut.clear();
            s_listCacheTime = 0;
            s_tsSession.unavailableUntilMs = 0;   // re-probe timestats for new pkg
            // P3: reset warmup clock so the new foreground app re-enters the 1s
            // warmup before its first vote. Without this, enabledAtMs stays at the
            // old session's start time, so nowMs-enabledAtMs >> 1000 and the very
            // first tick after switch samples the transition frames (stack/push
            // jank) and arms the boost vote on noise. enabled stays true; no
            // -enable re-emit needed (session already running).
            s_tsSession.enabledAtMs = nowMs;
        }
        auto tsFetch = [](const std::string& c) { return aatpeExec(c.c_str(), "SF_TIMESTATS"); };
        // v4.2.7: VM container detection — if foreground is a VM/container and
        // a manual game is running inside, use the container package for SF layer
        // matching but attribute stats to the inner game package.
        VmContainerInfo vmInfo = detectVmContainerGame(fgPkg);
        std::string sfMatchPkg = fgPkg;      // package name to match SF layers
        std::string statAttrPkg = fgPkg;     // package name to attribute stats to
        if (vmInfo.valid) {
            sfMatchPkg = vmInfo.containerPkg;
            statAttrPkg = vmInfo.innerGamePkg;
            writeFile((std::string(STATE_DIR) + "/vm_container_pkg").c_str(), vmInfo.containerPkg.c_str());
            writeFile((std::string(STATE_DIR) + "/vm_inner_game").c_str(), vmInfo.innerGamePkg.c_str());
            writeFile((std::string(STATE_DIR) + "/vm_inner_pids").c_str(), vmInfo.innerPids.c_str());
        }
        aatpe::frameaware::TimestatsResult tsr = s_tsSession.tick(nowMs, sfMatchPkg, tsFetch);
        // v6.0.0 round5: timestats is NO LONGER authoritative and must NOT early-return
        // before the latency channel. Real-device census (136 layers) showed the
        // content layer presents at a true ~101fps (latency present column, n=125)
        // while timestats reported 12.4fps — on this box timestats layer-selection /
        // histograms are unreliable (and 0 bytes right after -enable on earlier
        // dumps). Latency (real present timestamps) wins whenever it has valid
        // frames; timestats is only a fallback + cross-check below.
        // v4.2.8: snapshot will be written at the END of this tick (after fallback
        // channels complete) — see below. Early write removed to avoid inconsistency
        // when latency/gfxinfo fallback succeeds.
        // v4.1.1 final-round: publish WHY the timestats channel failed (read-only
        // diagnostic; the TTL short-circuit also carries session.lastReason).
        // v4.1.x fix: write the reason RAW. On a successful tick lastReason is
        // cleared (tick()) and tsr.reason is empty, so the old fallback wrote
        // "unknown" on EVERY tick — a healthy SF timestats channel permanently
        // rendered "未知" in the WebUI. The classifier only emits a concrete
        // token (disabled/selinux_denied/no_layer/parse_empty) on real failures;
        // an empty file renders "—" (no problem to report) which is the honest
        // state for warmup AND for a working channel.
        std::string tsrReason;
        {
            tsrReason = !tsr.reason.empty() ? tsr.reason : s_tsSession.lastReason;
            writeFile((std::string(STATE_DIR) + "/frame_timestats_reason").c_str(),
                      tsrReason.c_str());
        }
        // v4.2.8: snapshot will be written at the END of this tick (after fallback
        // channels complete) — see below. Early write removed to avoid inconsistency
        // when latency/gfxinfo fallback succeeds.

        // v4.2.8: multi-package tracking — runs REGARDLESS of tsr.available.
        // Even if foreground layer not found, parse all manual game layers from
        // the same SF dump. This is the core fix for "fg=unknown but manual
        // game is actively rendering still gets tracked".
        // F-2: pass the dump CONTENT fetch time (lastDumpAtMs), not the loop nowMs.
        // When tick() short-circuits inside its blind window it does NOT re-fetch,
        // so lastDumpContent is frozen — stamping it with the loop tick made stale
        // manual rows look live and defeated the 60s stale filter. Using
        // lastDumpAtMs ages frozen rows by their REAL content time.
        processManualGameLayers(s_tsSession.lastDumpContent, statAttrPkg,
                                 vmInfo.innerGamePkg, s_tsSession.lastDumpAtMs);

        // Throttle the --list enumeration to once per 2s (same budget as the
        // layer-count probe) to avoid a dumpsys fork every 50ms cycle.
        time_t nowList = time(nullptr);
        if (s_listCacheTime == 0 || (nowList - s_listCacheTime) >= 2) {
            s_cachedListOut = aatpeExec("dumpsys SurfaceFlinger --list 2>/dev/null", "SF_GAME_LIST");
            s_listCacheTime = nowList;
        }

        // v4.2.1: manual game-override list hit -> force this package through the
        // relaxed game-layer recognizer regardless of engine heuristics.
        const bool forceGame = aatpe::gamelist::isManualGame(fgPkg);

        aatpe::frameaware::SfFrameSample smp;
        bool binderOk = false;
        // Binder-preferred: direct DUMP_TRANSACTION for the selected layer.
        if (aatpe::frame_binder::isAlive()) {
            std::vector<std::string> layers = aatpe::frameaware::sfListLayers(s_cachedListOut);
            std::string selReason, selCand;
            std::string layer = aatpe::frameaware::selectGameLayer(
                layers, fgPkg, forceGame, &selReason, &selCand);
            smp.reason = selReason;
            smp.candidates = selCand;
            if (!layer.empty()) {
                aatpe::frame_binder::FrameStats st{};
                if (aatpe::frame_binder::refreshLayerLatency(layer, st) && st.valid) {
                    smp.chosenLayer = layer;
                    smp.available = true;
                    smp.samples = st.sampleCount;
                    smp.miss = (int)st.jankCount;
                    smp.hz = st.vsyncPeriodNs > 0 ? 1e9f / (float)st.vsyncPeriodNs : 0.0f;
                    smp.intervalMs = st.vsyncPeriodNs > 0 ? (float)st.vsyncPeriodNs / 1e6f : 0.0f;
                    // P1 round7-rework: real renderFps from present-column span,
                    // identical to dumpsys latency channel. Ring is "one row per
                    // submitted frame", so a 60fps game on a 120Hz panel yields ~60.
                    if (st.sampleCount >= 2 && st.lastPresentNs > st.firstPresentNs) {
                        double spanSec = (double)(st.lastPresentNs - st.firstPresentNs) / 1e9;
                        smp.renderFps = (spanSec > 0)
                            ? (double)(st.sampleCount - 1) / spanSec : 0.0;
                    }
                    binderOk = true;
                    smp.reason.clear();
                } else {
                    smp.frameUnavailable = true;
                    smp.reason = "no_valid_frames";
                }
            } else {
                smp.frameUnavailable = true;   // no game layer
            }
        }
        // dumpsys fallback (also the pure path exercised by the host unit test).
        if (!binderOk) {
            auto fetchCb = [](const std::string& layer) -> std::string {
                // T4/G3-P1: reject layer names outside the tightened whitelist
                // AND run via argv (no shell). The layer is a standalone argv
                // element, so $(...)/backtick/quotes are never expanded.
                if (!aatpe::frameaware::isLayerNameSafe(layer)) return "";
                return aatpeExecArgv({"dumpsys", "SurfaceFlinger", "--latency", layer},
                                     "SF_GAME_LAT");
            };
            smp = aatpe::frameaware::sampleGameLayer(s_cachedListOut, fgPkg, fetchCb, forceGame);
        }

        // v4.2.1 diagnostics: expose the actual fg pkg + candidate layers so the
        // WebUI can show WHY frame sampling produced no data on a real device.
        // FRAME-09: frame_debug_fg is written by frameSetAuthSnapshot() below in
        // every branch (smp.available / tsr.available / gfxinfo fallback / none),
        // so the intermediate write here was redundant and is removed.
        writeFile((std::string(STATE_DIR) + "/frame_debug_candidates").c_str(),
                  (smp.candidates.empty() ? std::string("none") : smp.candidates).c_str());

        writeFile((std::string(STATE_DIR) + "/frame_unavailable").c_str(),
                  smp.frameUnavailable ? 1 : 0);
        writeFile((std::string(STATE_DIR) + "/game_layer").c_str(),
                  smp.chosenLayer.empty() ? "none" : smp.chosenLayer.c_str());
        // v6.0.0 round5: conflict cross-check. Latency (real present timestamps) is
        // the ground truth. If timestats also produced a value but the two disagree
        // beyond tolerance, flag it (WebUI may warn) and KEEP latency. Tolerance:
        // relative ratio gap >20% OR absolute gap >15fps — large enough to ignore
        // ring-window jitter (~15% on a 120Hz 128-ring), small enough to catch the
        // real-device 101 vs 12.4 case (~88% gap). Fully dynamic, no fixed fps.
        bool tsSuspect = false;
        std::string tsSuspectReason;
        if (tsr.available && tsr.activeFrames >= 5 && smp.available && smp.renderFps > 1.0) {
            double latFps = smp.renderFps;
            double tsFps  = tsr.avgFps;
            double relGap = std::abs(latFps - tsFps) / std::max(latFps, tsFps);
            double absGap = std::abs(latFps - tsFps);
            if (relGap > 0.20 || absGap > 15.0) {
                tsSuspect = true;
                char buf[96];
                snprintf(buf, sizeof(buf), "latency=%.1f timestats=%.1f relGap=%.2f",
                         latFps, tsFps, relGap);
                tsSuspectReason = buf;
            }
        }
        writeFile((std::string(STATE_DIR) + "/frame_timestats_suspect").c_str(),
                  tsSuspect ? "1" : "0");
        if (!tsSuspectReason.empty())
            writeFile((std::string(STATE_DIR) + "/frame_timestats_suspect_reason").c_str(),
                      tsSuspectReason.c_str());

        if (smp.available) {
            double missRate = smp.samples > 0 ? (double)smp.miss / (double)smp.samples : 0.0;
            const char* srcName = binderOk ? "latency_binder" : "latency_dumpsys";
            // F-04/F-06: honest fps resolution. renderFps is the measured present
            // rate; smp.hz is the PANEL refresh (a target hint, NEVER shown as the
            // measured fps); GED composer node is the MTK fallback. On a sparse
            // ring we report DEGRADED instead of fabricating the panel hz (which
            // used to show a fake 120fps while the game was at single digits).
            int gedFps = aatpeReadGedComposerFps();
            aatpe::frameaware::GameFpsResolve gr =
                aatpe::frameaware::resolveGameFps(smp.renderFps, (double)smp.hz,
                                                  gedFps, srcName);
            if (gr.degraded) {
                // F-04: layer had rows but no trustworthy present span this beat
                // (layer-switch warmup / static frame). Hold the last good display,
                // mark the channel DEGRADED, and clear this beat's jank input
                // (no data -> clear, never latch). Do NOT write a fake fps.
                frameSetAuthSnapshot(statAttrPkg, "degraded", "insufficient_present_frames");
                aatpeFrameClearMissJankInput();
            } else {
                // F-3: authoritative snapshot BEFORE the vote, so aatpeSetFrameJank's
                // auth-fg lookup resolves to the package we just measured.
                frameSetAuthSnapshot(statAttrPkg, gr.source, "");
                writeFile((std::string(STATE_DIR) + "/game_active").c_str(), 1);
                double gameFps = gr.fps;
                // F-01/F-02: wire the measured per-layer fps into the per-app tables
                // AND push a fresh window (aatpeSetFrameJank pushes the window) so the
                // game row stays young (ageMs<5s) and never ages into background_stale.
                if (!statAttrPkg.empty() && statAttrPkg != "unknown") {
                    std::lock_guard<std::mutex> lk(g_perAppMutex);
                    auto& m = g_perAppFsMetrics[statAttrPkg];
                    m.fps = gameFps;
                    m.targetFps = smp.hz;
                    m.jankyPct = missRate;
                    m.missedVsyncs = (long long)smp.miss;
                    m.totalFrames = smp.samples;
                    m.updatedMs = nowMs;
                }
                writeFile((std::string(STATE_DIR) + "/game_hz").c_str(), (int)lround(gameFps));
                aatpeSetFrameJank(gr.source, smp.samples, missRate, gameFps, 0.0, 0.0);
            }
        } else if (tsr.available && tsr.activeFrames >= 5) {
            // v6.0.0 round5: latency had no valid present frames on this window —
            // fall back to timestats (it was previously the only channel). This is
            // the ONLY path timestats is used for the foreground fps now.
            frameSetAuthSnapshot(statAttrPkg,
                                  vmInfo.valid ? "sf_vm_container" : "timestats",
                                  "");
            writeFile((std::string(STATE_DIR) + "/game_active").c_str(), 1);
            if (!statAttrPkg.empty() && statAttrPkg != "unknown") {
                std::lock_guard<std::mutex> lk(g_perAppMutex);
                auto& m = g_perAppFsMetrics[statAttrPkg];
                m.fps = tsr.avgFps;
                m.targetFps = tsr.targetFps;
                m.jankyPct = tsr.missRate;
                m.missedVsyncs = (long long)(tsr.missRate * tsr.activeFrames);
                m.totalFrames = tsr.activeFrames;
                m.updatedMs = nowMs;
            }
            writeFile((std::string(STATE_DIR) + "/game_hz").c_str(), (int)lround(tsr.avgFps));
            aatpeSetFrameJank(vmInfo.valid ? "sf_vm_container" : "timestats",
                              tsr.totalFrames, tsr.missRate,
                              tsr.avgFps, tsr.p90Fps, tsr.p99Fps);
        } else {
            // fail-closed: 显式撤票 + 上报 frame_source=none.
            // v4.2.1: refined reason (no_fg / sf_list_empty / no_layer_for_pkg /
            // no_valid_frames). "no_game_layer" kept as a legacy alias for the
            // no_layer_for_pkg bucket so older WebUI builds still map it.
            std::string r = smp.reason;
            if (r == "no_layer_for_pkg") r = "no_game_layer";   // legacy alias
            if (r.empty()) r = "no_valid_frames";

            // D4: binder SF + timestats + dumpsys-latency are ALL dead (real-device
            // MIUI su-domain: binder getService(SurfaceFlinger) fails and --timestats
            // returns 0 bytes). The plain `dumpsys gfxinfo <fgPkg>` SUMMARY still
            // works. Use it instead of falling straight to "none": derive fps from
            // its self-describing window and the AOSP janky-frames% directly.
            // Throttled to one dump per 5s (same cadence as the framestats refresh)
            // and the last-good summary is re-reported between refreshes so the
            // frame_source does not flap between "gfxinfo" and "none" every 50ms.
            bool reportedGfx = false;
            bool gfxPkgOk = !fgPkg.empty() && fgPkg != "unknown" && aatpeIsValidPackageName(fgPkg);
            static time_t s_gfxSumLastRun = 0;
            static GfxinfoSummary s_lastGoodGfx;
            static time_t s_lastGoodGfxAt = 0;
            // v6.0.0 round4b: remember WHICH package the cached summary came from.
            // The old code re-reported s_lastGoodGfx regardless of fgPkg, so after a
            // SurfaceView game came to the foreground the panel kept showing the
            // PREVIOUS View UI's 120fps within ~2 refresh windows. A cached summary
            // may only be re-reported for the SAME foreground package.
            static std::string s_lastGoodGfxPkg;
            if (gfxPkgOk) {
                time_t nowGs = time(nullptr);
                if (nowGs - s_gfxSumLastRun >= kFramestatsIntervalSec) {
                    s_gfxSumLastRun = nowGs;
                    std::string out = aatpeExec(
                        ("dumpsys gfxinfo " + fgPkg + " 2>/dev/null").c_str(),
                        "SF_GFX_SUMMARY");
                    GfxinfoSummary gs = parseGfxinfoSummary(out, aatpeUptimeNowNs());
                    if (aatpeGfxinfoSummaryUsable(gs)) {
                        s_lastGoodGfx = gs;
                        s_lastGoodGfxAt = nowGs;
                        s_lastGoodGfxPkg = fgPkg;
                    }
                }
                // Re-report the last good summary ONLY for the same foreground pkg
                // while still fresh (~2 refresh windows). No cross-package reuse.
                if (aatpeGfxinfoSummaryUsable(s_lastGoodGfx) &&
                    s_lastGoodGfxPkg == fgPkg &&
                    (nowGs - s_lastGoodGfxAt) < 2.0 * kFramestatsIntervalSec) {
                    double p90fps = s_lastGoodGfx.p90ms > 0.0 ? 1000.0 / s_lastGoodGfx.p90ms : 0.0;
                    double p99fps = s_lastGoodGfx.p99ms > 0.0 ? 1000.0 / s_lastGoodGfx.p99ms : 0.0;
                    // Refresh-rate-aware jank: gfxinfo janky% already counts late
                    // frames, but on a high-refresh panel a steady low fps (e.g.
                    // 30fps on 120Hz) reports few "janky" frames yet still feels
                    // choppy. Fold that contribution in (mirrors the framestats
                    // low-fps vote at ~0.55x panel rate).
                    double missRate = s_lastGoodGfx.jankyPct;
                    if (rate >= refresh::kMinRate && rate <= refresh::kMaxRate &&
                        s_lastGoodGfx.fps > 1.0) {
                        double lowFpsRatio = s_lastGoodGfx.fps / (double)rate;
                        if (lowFpsRatio < 0.55) {
                            double lowFpsJank = 1.0 - lowFpsRatio / 0.55;  // 0@55% .. 1@0%
                            if (lowFpsJank > missRate) missRate = lowFpsJank;
                        }
                    }
                    aatpeSetFrameJank("gfxinfo", (int)s_lastGoodGfx.totalFrames, missRate,
                                      s_lastGoodGfx.fps, p90fps, p99fps);
                    frameSetAuthSnapshot(fgPkg, "gfxinfo", r);
                    reportedGfx = true;
                }
            }
            if (!reportedGfx) {
                aatpeSetFrameJank("none", 0, 0.0, 0.0, 0.0, 0.0);
                // v4.2.8: authoritative snapshot (all fallback channels failed).
                frameSetAuthSnapshot(fgPkg, "none", r);
            }
        }
    } else {
        // F-01: reached ONLY when foreground is NOT a game AND the global SF-latency
        // channel is live (sfLatValid && !fgIsGame). This is a View/launcher app, not
        // a SurfaceView game. Label the channel honestly "non_game" so the WebUI shows
        // "非游戏，不显示游戏fps" instead of a misleading 0 / single-digit fps.
        writeFile((std::string(STATE_DIR) + "/frame_unavailable").c_str(), 0);
        writeFile((std::string(STATE_DIR) + "/game_active").c_str(), 0);
        std::string gfxFg = aatpeGetForegroundPackage();
        frameSetAuthSnapshot(gfxFg, "non_game", "foreground_is_not_a_game");
        // C-001: on the global SF-latency channel there is NO timestats/binder
        // miss-rate input this cycle (the whole sampler block is skipped because the
        // fg is not a game). Symmetrically clear g_frameMissJankInput so a stale
        // missJank=true left over from a previous timestats/binder window cannot
        // keep fusion anyJank pinned. "无数据即清票": no fresh miss-rate data -> clear.
        aatpeFrameClearMissJankInput();
        // FRAME-02: zero the game_* state files so exiting a SurfaceView game to
        // the launcher/View UI doesn't leave the WebUI showing the old game's
        // fps/miss. The per-app panel still gets real fps from g_perAppFsMetrics.
        aatpeFrameClearGameStats();
    }

    // v4.2.1 T3: throttled `dumpsys gfxinfo <fgPkg> framestats` for real
    // per-frame p50/p95/p99. Runs at most once per 5s on the current fg pkg
    // only. Silent fallback: any error -> no new percentiles, existing ring
    // preserved. Does NOT affect the global frame decision path.
    {
        std::string fgPkg = aatpeGetForegroundPackage();
        // v4.2.7: skip gfxinfo for VM inner games — dumpsys gfxinfo <innerPkg>
        // returns "No process found" because the game runs inside the VM container.
        // SF timestats container layer is the authoritative source for VM games.
        bool pkgOk = !fgPkg.empty() && fgPkg != "unknown" && aatpeIsValidPackageName(fgPkg);
        if (pkgOk && gamelist::isManualGame(fgPkg)) {
            // Check if foreground is a VM container and this manual game is inside it.
            VmContainerInfo vmFg = detectVmContainerGame(aatpeGetForegroundPackage());
            if (vmFg.valid && vmFg.innerGamePkg == fgPkg) {
                pkgOk = false;  // skip gfxinfo — VM inner game
            }
        }
        if (pkgOk) {
            time_t nowFs = time(nullptr);
            if (nowFs - g_framestatsLastRun >= kFramestatsIntervalSec) {
                g_framestatsLastRun = nowFs;
                std::string fsCmd = "dumpsys gfxinfo " + fgPkg + " framestats 2>/dev/null";
                std::string fsOut = aatpeExec(fsCmd.c_str(), "SF_FRAMESTATS");
                auto durs = parseGfxinfoFramestats(fsOut);
                if (!durs.empty()) {
                    std::lock_guard<std::mutex> lk(g_perAppMutex);
                    auto& ring = g_perAppFrameTimes[fgPkg];
                    for (double d : durs) {
                        ring.push_back(d);
                        if (ring.size() > kMaxFrameSamplesPerApp) ring.pop_front();
                    }
                    // v4.2.6: compute authoritative metrics from IntendedVsync.
                    FramestatsMetrics fm = computeFramestatsMetrics(fsOut);
                    if (fm.totalFrames >= 5) {
                        auto& m = g_perAppFsMetrics[fgPkg];
                        m.fps = fm.fps;
                        m.targetFps = fm.targetFps;
                        m.jankyPct = fm.jankyPct;
                        m.missedVsyncs = fm.missedVsyncs;
                        m.totalFrames = fm.totalFrames;
                        // FRAME-03: unify on steady_clock ms (frameNowMs()) so that
                        // updatedMs uses the same monotonic clock as g_perAppWindows
                        // ring.tsMs / ageMs everywhere else. The old code used
                        // wall-clock (time(nullptr)*1000) here, mixing epoch ms with
                        // boot-relative ms in the same field — a future staleness
                        // check built on updatedMs would see a 56-year offset.
                        m.updatedMs = frameNowMs();
                        // FIX(v4.3.1, RC3): the accurate per-app jankyPct now VOTES, not
                        // just displays. Two generic signals, normalized to the panel
                        // refresh rate: (a) >=10% of frames late vs intended cadence;
                        // (b) measured fps < 55% of the panel refresh (steady choppy
                        // low-fps, e.g. 30/60fps on a 120Hz panel — the user's "very
                        // laggy" that computeFramestatsMetrics otherwise scores 0%).
                        bool lateJank = (fm.jankyPct >= 0.10);
                        double lowFpsRatio = (rate >= refresh::kMinRate && rate <= refresh::kMaxRate && fm.fps > 1.0)
                                             ? fm.fps / (double)rate : 1.0;
                        bool lowFps = (lowFpsRatio > 0.0 && lowFpsRatio < 0.55);
                        aatpe::frameaware::g_appFsJankActive.store(lateJank || lowFps, std::memory_order_relaxed);
                    } else {
                        // C-002: a refresh that parsed <5 frames is NOT a jank signal
                        // (launcher / static page / just-switched app). Symmetrically
                        // clear the app-framestats jank vote so it does not latch true
                        // after the foreground moves to a low-frame app (which would
                        // keep fusion anyJank pinned and block release).
                        aatpeFrameClearAppFsJank();
                    }
                    // Also ensure the fg pkg is in the LRU table even if
                    // aatpeSetFrameJank hasn't recorded it (e.g. static app).
                    // v4.4.1 P0 (RC-2a): an LRU push alone is NOT enough — the panel
                    // (aatpe_getPerAppFrameStats) iterates g_perAppLru and SKIPS any
                    // pkg whose g_perAppWindows ring is empty. On devices where the
                    // timestats channel is disabled (MIUI/MTK) and only the gfxinfo
                    // fallback produced real frames, the fg app got fsMetrics but NO
                    // window row -> the panel fell back to a "waiting_first_sample"
                    // stub even though `dumpsys gfxinfo` clearly reports fps. Push a
                    // real window from the authoritative gfxinfo metrics so "any app
                    // with frames always has a panel row".
                    auto winIt = g_perAppWindows.find(fgPkg);
                    if (winIt == g_perAppWindows.end()) {
                        if (g_perAppWindows.size() >= kMaxPerAppStats) {
                            std::string old = g_perAppLru.back(); g_perAppLru.pop_back();
                            g_perAppWindows.erase(old);
                            g_perAppFrameTimes.erase(old);  // P2-2
                            g_perAppFsMetrics.erase(old);
                            evictTsLayerSnapForPkgLocked(old);  // FRAME-01
                        }
                        g_perAppLru.push_front(fgPkg);
                    } else {
                        g_perAppLru.remove(fgPkg);
                        g_perAppLru.push_front(fgPkg);
                    }
                    if (fm.totalFrames >= 5) {
                        auto& ring = g_perAppWindows[fgPkg];
                        PerAppWindow w;
                        w.fps = fm.fps;
                        w.samples = fm.totalFrames;
                        w.dropped = fm.missedVsyncs;
                        w.jankArmed = (fm.jankyPct >= 0.05);
                        w.tsMs = frameNowMs();
                        ring.push_back(w);
                        while (ring.size() > kMaxWindowsPerApp) ring.pop_front();
                    }
                } else {
                    // RT-C-1: zero framestats rows this cycle (empty dump / the fg
                    // app rendered no frames / the gfxinfo fork returned nothing) is
                    // "no data", NOT a jank signal. Previously the whole bookkeeping
                    // block was gated on !durs.empty(), so a stale g_appFsJankActive=
                    // true left over from a previous app persisted for 1..N throttle
                    // cycles before the next non-empty refresh cleared it. "无数据即
                    // 清票": clear the app-framestats jank vote on the SAME cycle so
                    // the fusion release-hysteresis can count down without a stale
                    // app-framestats input. Releasing boost is power-safe (fail-open
                    // only on the release direction, never latching a jank vote on
                    // missing data). No g_perAppMutex needed: a single atomic store.
                    aatpeFrameClearAppFsJank();
                }
            }
        }
    }

    // v4.3.1 hotfix: label warmup runs on a detached background thread started
    // at the top of this function. No APK parsing happens on the decision loop.
}

// ============================================================================
// FOREGROUND APP DETECTION (fix: cache "unknown" too)
// ============================================================================
std::string g_foregroundApp = "unknown";
// FRAME-05: removed g_foregroundCacheTime/g_foregroundCached dead state (written
// at 5 sites, never read in this TU; the D3 cascade uses g_fgDumpsysReadMs /
// g_fgDumpsysResult for staleness). Kept g_foregroundApp (protected by
// g_foregroundMutex) which IS read cross-thread.

// P0-5: g_foregroundApp is a std::string written by this foreground-detection
// path and read cross-thread (gpu_boost.cpp, scoring.cpp, the eBPF onSwitch
// consumer, the WebUI state publisher). A torn std::string read is a SIGSEGV.
// All frame.cpp reads/writes now go through these two helpers, which take the
// shared g_foregroundMutex (defined in aatped_ebpf_agg.cpp — do NOT define a
// second copy here). Other TUs should call the exported
// aatpeGetForegroundPackage() instead of touching g_foregroundApp directly.
std::string aatpeGetForegroundPackage() {
    std::lock_guard<std::mutex> lk(g_foregroundMutex);
    return g_foregroundApp;
}

// v4.2.7: foreground app debouncer.
// Prevents transient focus changes (VM assistant windows, floating windows,
// status bar, launcher, quick switches) from tearing down the current game session.
std::string aatpe::frameaware::FgDebouncer::onRawFg(const std::string& raw, long long nowMs) {
        // 1. No committed app yet → first real package commits immediately.
        if (committed.empty() || committed == "unknown") {
            if (!raw.empty() && raw != "unknown") {
                committed = raw;
                pending.clear();
            }
            return committed;
        }
        // 2. Same as committed → clear pending, return committed.
        if (raw == committed) {
            pending.clear();
            return committed;
        }
        // 3. Raw is empty/unknown while we have a real committed app → transient.
        if (raw.empty() || raw == "unknown") {
            return committed;  // hold current
        }
        // 4. Transient focus (system UI, floating windows, VM assistant) → discard raw.
        // v4.2.8 B3: transient focus check only applies to non-game packages.
        // If the new fg is a manual game, skip the debounce entirely (see step 5).
        bool rawIsManualGame = gamelist::isManualGame(raw);
        if (!rawIsManualGame && isTransientFocus(committed, raw)) {
            return committed;  // hold current, don't set pending
        }
        // 5. v4.2.8 B3: manual game packages commit immediately (skip 3s debounce).
        // User explicitly added these games — they should get frame tracking instantly.
        if (rawIsManualGame) {
            committed = raw;
            pending.clear();
            return committed;
        }
        // 6. Real different non-game app → start/continue pending timer (3s debounce).
        if (pending != raw) {
            pending = raw;
            pendingSinceMs = nowMs;
            return committed;  // not yet committed
        }
        // Same pending → check if held long enough.
        if (nowMs - pendingSinceMs >= kCommitDelayMs) {
            committed = raw;
            pending.clear();
            return committed;
        }
        return committed;
    }

static aatpe::frameaware::FgDebouncer s_fgDebouncer;

static void aatpeSetForegroundLocked(const std::string& pkg) {
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(g_foregroundMutex);
    g_foregroundApp = s_fgDebouncer.onRawFg(pkg, nowMs);
}

// ============================================================================
// Three-tier foreground-app detection (CPU optimization).
//
// Problem: the daemon loop previously called aatpeDetectForegroundApp(true)
// every ~50ms cycle. forceRefresh=true bypassed the 3s wall-clock cache, so on
// devices where the cgroup/top-app cpuset node did not yield a package the call
// cascaded through the /proc scan and straight into detectForegroundFromDumpsys
// -> fork()+sh+`dumpsys ...` every cycle. dumpsys has a 10s cooldown ONLY after
// a timeout; on a device where dumpsys returns promptly, nothing throttled it,
// so the low-CPU target paid a fork+Binder round-trip up to 20x/sec.
//
// Three tiers, cheapest-first:
//   Tier 1 (cgroup/top-app fast path): read /dev/cpuset/top-app/tasks or the
//          cgroup v2 equivalent (openat+read, NO fork). Result cached for a
//          short TTL. Within the TTL and absent any invalidation event the
//          caller gets the cached package with ZERO syscalls.
//   Tier 2 (inotify event-driven): one non-blocking inotify fd watches the
//          top-app/foreground cpuset directories AND their tasks/cgroup.procs
//          files. Any change marks the Tier-1 cache dirty. On a quiet system
//          the drain is a single non-blocking read() that returns EAGAIN.
//   Tier 3 (low-frequency dumpsys fallback): ONLY when Tier-1/2 yield nothing
//          AND the rate limit (>=1000ms since the last dumpsys) has elapsed.
//          This caps dumpsys at <=1/sec even on a device where cpuset reads
//          always fail.
//
// Response-time guarantee: worst case (inotify silent) the Tier-1 TTL forces a
// fresh cgroup read within 200ms; with inotify the next loop tick (<=50ms)
// re-reads. Both are well under the 500ms foreground-switch budget.
// ============================================================================
static constexpr long long kFgCgroupTtlMs    [[maybe_unused]] = 200;   // Tier-1 cache TTL (100-500ms window)
static constexpr long long kFgDumpsysMinGapMs [[maybe_unused]] = 1000;  // Tier-3 min interval (1000-3000ms window)
// LED-050 (R4-006): these cgroup TTL/dirty/dumpsys throttle globals are read and
// written from multiple threads (main loop, detectGameProcess, gpuBoostTick all
// call aatpeDetectForegroundApp). They were plain long long/bool — a C++ data
// race. Promote to atomics (relaxed ordering is fine: they are throttle hints,
// not guarding data).
static std::atomic<long long> g_fgCgroupReadMs{0};
static std::atomic<bool>      g_fgCgroupDirty{true};
static std::atomic<long long> g_fgDumpsysLastMs[[maybe_unused]]{0};
// D3: dumpsys activity activities is now the AUTHORITATIVE foreground source (see
// aatpeFgSourceDecision). Its result is cached for kFgDumpsysTtlMs and re-queried at
// most once per kFgDumpsysMinGapMs (~1Hz). g_fgDumpsysResult is the last package the
// dumpsys parser produced ("" = none this probe); it is guarded by g_foregroundMutex
// because it is a std::string read/written cross-thread. A result older than the TTL
// is STALE and is never served: the cascade re-probes (when the 1Hz gap allows) or
// drops to the cgroup fallback / "unknown" rather than reporting a stale value.
static constexpr long long kFgDumpsysTtlMs = 2000;   // authoritative result freshness (2s)
static std::atomic<long long> g_fgDumpsysReadMs{0};
static std::string g_fgDumpsysResult;   // guarded by g_foregroundMutex
// g_fgInotifyFd is accessed from multiple threads (main loop, detectGameProcess,
// gpuBoostTick). Use atomic<int> for cross-thread visibility, plus a dedicated
// init mutex to prevent two threads from racing through fgInotifyInit() and
// double-creating the inotify fd.
static std::atomic<int> g_fgInotifyFd{-1};  // -1=uninit, -2=permanently failed, >=0=open
static std::mutex g_fgInotifyInitMtx;

static inline long long fgNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Lazy one-time inotify setup. IN_CLOEXEC is mandatory: aatpeExec() forks+execs
// /system/bin/sh on this thread, and we must not leak the watch fd into every
// dumpsys grandchild. Best-effort: any watched path that does not exist on this
// device is skipped; the 200ms TTL in Tier 1 backstops a totally dead inotify.
static void fgInotifyInit() {
    int fd = g_fgInotifyFd.load(std::memory_order_acquire);
    if (fd >= 0 || fd == -2) return;  // already done

    // Double-checked locking: only one thread creates the inotify fd.
    std::lock_guard<std::mutex> lk(g_fgInotifyInitMtx);
    fd = g_fgInotifyFd.load(std::memory_order_acquire);
    if (fd >= 0 || fd == -2) return;  // another thread finished first

    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        g_fgInotifyFd.store(-2, std::memory_order_release);
        return;
    }
    // Watch the cpuset group directories (catches entry add/remove/move) AND the
    // concrete tasks/cgroup.procs files (catches in-place task migration writes).
    static const char* watchPaths[] = {
        "/dev/cpuset/top-app",            "/dev/cpuset/top-app/tasks",
        "/dev/cpuset/foreground",         "/dev/cpuset/foreground/tasks",
        "/sys/fs/cgroup/cpuset/top-app",  "/sys/fs/cgroup/cpuset/top-app/cgroup.procs",
        "/sys/fs/cgroup/cpuset/foreground","/sys/fs/cgroup/cpuset/foreground/cgroup.procs",
        nullptr
    };
    uint32_t mask = IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                    IN_MOVED_TO | IN_MOVED_FROM;
    for (int i = 0; watchPaths[i] != nullptr; i++) {
        // Failure (ENOENT/EACCES on this device) is fine: skip, others may exist.
        inotify_add_watch(fd, watchPaths[i], mask);
    }
    g_fgInotifyFd.store(fd, std::memory_order_release);
}

// Non-blocking drain of the inotify fd. Any pending event -> mark the cgroup
// cache dirty so the next call re-reads cpuset. We do NOT care which event it
// is; "something under top-app/foreground changed" is enough to invalidate.
static void fgInotifyDrain() {
    int fd = g_fgInotifyFd.load(std::memory_order_acquire);
    if (fd < 0) {
        fgInotifyInit();
        fd = g_fgInotifyFd.load(std::memory_order_acquire);
        if (fd < 0) return;  // unavailable on this build/device
    }
    char buf[256];
    bool sawEvent = false;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) { sawEvent = true; continue; }
        break;  // n==0 (shouldn't happen) or n<0 (EAGAIN drained / EINTR)
    }
    if (sawEvent) g_fgCgroupDirty = true;
}

// Package-name whitelist validator (defense-in-depth for shell sinks).
//
// Android app package names are composed of dot-separated identifiers, each
// starting with a letter and containing only letters / digits / underscore.
// We do NOT rely on PackageManager as the only line of defence: the extracted
// string is concatenated into `dumpsys gfxinfo <pkg>` which is handed to
// /system/bin/sh -c (see aatpeExec). Any shell metacharacter (; | & $ ` etc.)
// reaching that string would be command-injection. The same codebase already
// enforces a strict whitelist on WebUI /control commands; this brings the
// foreground-app path up to parity.
//
// Rules (mirrors Android package-name spec + the user's v94-sec hardening list):
//   - length 1..256 (AndroidManifest maxPackageNameLength = 256)
//   - first char must be an ASCII letter
//   - every char must be in [A-Za-z0-9._]
//   - must not start or end with '.'
//   - must not contain ".."
//
// Returns true iff the string is a syntactically safe package name.
static bool aatpeIsValidPackageName(const std::string& p) {
    static const size_t kMaxPkgLen = 256;
    if (p.empty() || p.size() > kMaxPkgLen) return false;
    if (!isalpha((unsigned char)p[0])) return false;
    if (p[0] == '.' || p[p.size() - 1] == '.') return false;
    if (p.find("..") != std::string::npos) return false;
    for (char ch : p) {
        if (isalnum((unsigned char)ch)) continue;   // A-Z a-z 0-9
        if (ch == '.' || ch == '_') continue;
        return false;                               // rejects ; | & $ ` space / newline etc.
    }
    return true;
}

static std::string aatpeExtractPackage(const std::string& component) {
    std::string s = component;
    trim(s);
    size_t ob = s.find('{');
    size_t cb = s.find('}');
    if (ob != std::string::npos && cb != std::string::npos && cb > ob) {
        s = s.substr(ob + 1, cb - ob - 1);
    }
    // fix: handle window ID prefix (e.g. "abc123 u0 com.android.settings/.Settings")
    // Find the token containing '/' which is the package/activity component
    size_t slash = s.find('/');
    if (slash != std::string::npos) {
        // Find the start of the token containing '/' (split by spaces)
        size_t tokenStart = s.rfind(' ', slash);
        if (tokenStart == std::string::npos) tokenStart = 0;
        else tokenStart++; // skip the space
        s = s.substr(tokenStart, slash - tokenStart);
    }
    // fix: removed fragile uppercase-stripping heuristic.
    // Package names like "com.miHoYo.GenshinImpact" must be returned as-is.
    trim(s);
    // reject anything that is not a syntactically valid Android package
    // name. Callers already treat "" as "no package found" and fall through to
    // the next detection layer, so returning "" here is safe and does not alter
    // the fallback ORDER — it simply prevents a garbage/tampered token from
    // ever reaching a shell command.
    if (!aatpeIsValidPackageName(s)) {
        LOG_WARN("FG_APP: extracted package failed whitelist (len=%zu) -> dropping", s.size());
        return std::string();
    }
    return s;
}

// Extract the component value from a dumpsys line.
// Android 10 and many AOSP builds output "mResumedActivity: ActivityRecord{...}"
// (colon separator), while newer outputs use "mResumedActivity=ActivityRecord{...}".
// This handles both separators for Android 10-17 compatibility.
static std::string aatpeExtractDumpsysValue(const std::string& o) {
    size_t eq = o.find('=');
    size_t colon = o.find(':');
    size_t pos;
    if (eq != std::string::npos) pos = eq;
    else if (colon != std::string::npos) pos = colon;
    else return "";
    return o.substr(pos + 1);
}

// Read PIDs/TIDs from a cpuset group across cgroup v1 and v2 layouts.
//   cgroup v1: /dev/cpuset/<group>/tasks
//   cgroup v2: /sys/fs/cgroup/cpuset/<group>/cgroup.procs | cgroup.threads | tasks
// Returns the first non-empty readable source. Empty vector on failure.
static std::vector<int> aatpeReadCpusetPids(const std::string& group) {
    std::vector<int> pids;
    const char* candidates[][2] = {
        {"/dev/cpuset/",            "/tasks"},          // cgroup v1 (Android 10-14 / OEM)
        {"/sys/fs/cgroup/cpuset/",  "/cgroup.procs"},   // cgroup v2 processes
        {"/sys/fs/cgroup/cpuset/",  "/cgroup.threads"}, // cgroup v2 threads
        {"/sys/fs/cgroup/cpuset/",  "/tasks"},          // cgroup v2 legacy tasks
    };
    for (auto& cand : candidates) {
        std::string path = std::string(cand[0]) + group + cand[1];
        std::ifstream f(path);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            trim(line);
            if (line.empty()) continue;
            int pid = 0;
            try { pid = std::stoi(line); } catch (...) { continue; }
            if (pid > 0) pids.push_back(pid);
        }
        if (!pids.empty()) break;  // use the first source that yields PIDs
    }
    return pids;
}

// ============================================================================
// //   The original ~205-line function mixed 5 fallback layers inline. It is now
//   decomposed into per-layer helpers. PURELY mechanical: every statement,
//   shell command string, cache TTL and log line is copied verbatim. The
//   fallback ORDER is unchanged: APK -> sysfs(cgroup) -> /proc scan ->
//   dumpsys -> final fallback. The 3s foreground cache check stays in the
//   entry function. Shell commands remain hardcoded literals (no injection).
// ============================================================================

// Layer 1: Helper APK takeover. Reads the APK-written foreground_app file
// (multi-path, 30s freshness). On success sets the cache globals and returns
// the package immediately. Returns "" when the APK did not produce a usable
// value (missing/empty/unknown/stale) so the caller falls through.
std::string detectForegroundFromApk(time_t now) {
    // APK-controlled foreground detection.
    // If Helper APK (com.aatpe.helper) is installed, the APK writes the
    // foreground app package name to /dev/aatpe/state/foreground_app.
    // The module reads it directly instead of running its own detection.
    // Fallback: if the file is missing, empty, "unknown", or stale (>10s),
    // the module falls back to its own detection and marks source=module_fallback.
    if (g_helperApkInstalled) {
        // APK foreground file only changes on a real app
        // switch. The main loop calls aatpeDetectForegroundApp(true) every cycle
        // (forceRefresh bypasses the 3s cache), which used to re-read the APK
        // file, rewrite 4 state files and re-log the identical line hundreds of
        // times/sec whenever fork/exec events kept waking the loop -> 127% CPU.
        //
        // Fast path (mtime gate): if the file mtime is unchanged since the last
        // handled revision, the content cannot have changed -> return the cached
        // package with ZERO readFile/writeFile/log. A genuine app switch changes
        // mtime and is picked up on the very next cycle (no blind 1s delay that
        // would re-introduce the missed-rapid-app-switch bug).
        static time_t s_apkHandledMtime = -1;
        static std::string s_apkHandledPkg;
        // Content-dedup log throttle: same (package, source) within 5s logs once.
        static std::string s_apkLastLoggedPkg;
        static long long   s_apkLastLogMs = 0;
        // Multi-path attempt. APK may write to either state/ or root dir.
        static const char* apkFgPaths[] = {
            "/dev/aatpe/state/foreground_app", // primary (APK writes here)
            "/dev/aatpe/foreground_app",          // fallback (older APK versions)
            nullptr
        };
        // FRAME-04: removed dead `apkHandled` flag. The loop either returns on
        // success (valid content) or falls through to the "missing" diagnostic
        // after all paths are exhausted; a flag set just before `return` was
        // never observed by the post-loop check.
        for (int pi = 0; apkFgPaths[pi] != nullptr; pi++) {
            const char* apkFgPath = apkFgPaths[pi];
            struct stat st;
            if (stat(apkFgPath, &st) == 0) {
                time_t fileAge = now - st.st_mtime;
                // P3-12: if the APK's mtime is in the future (clock skew,
                // freshly flashed image with wrong epoch), fileAge goes
                // negative and the `fileAge <= 30` fast-path below would
                // silently accept a stale file as current. Clamp to 0.
                if (fileAge < 0) fileAge = 0;
                // Extended timeout from 10s to 30s. APK may only write
                // on foreground change, not every second. 10s was too aggressive
                // and caused frequent false fallbacks to module detection.
                if (fileAge <= 30) {
                    // unchanged file revision -> fast path,
                    // no readFile, no state-file rewrites, no log spam.
                    if (st.st_mtime == s_apkHandledMtime && !s_apkHandledPkg.empty()) {
                        aatpeSetForegroundLocked(s_apkHandledPkg);
                        return s_apkHandledPkg;
                    }
                    std::string apkFg = readFile(apkFgPath);
                    trim(apkFg);
                    if (!apkFg.empty() && apkFg != "unknown") {
                        aatpeSetForegroundLocked(apkFg);
                        // P1-5: DO NOT write back to APK's file (read-only).
                        writeFile((std::string(STATE_DIR) + "/foreground_app_source").c_str(), "apk");
                        // Diagnostic state files for APK takeover health
                        writeFile((std::string(STATE_DIR) + "/fg_apk_status").c_str(), "ok");
                        writeFile((std::string(STATE_DIR) + "/fg_apk_age").c_str(), (int)fileAge);
                        writeFile((std::string(STATE_DIR) + "/fg_apk_path").c_str(), apkFgPath);
                        // dedup log — same package within 5s
                        // is logged once only (log line itself is file I/O and
                        // was a major CPU contributor under event wakeups).
                        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (apkFg != s_apkLastLoggedPkg || (nowMs - s_apkLastLogMs) >= 5000) {
                            FRAME_LOG_INFO("FG_APP: [APK] foreground=%s (age=%lds path=%s)", apkFg.c_str(), fileAge, apkFgPath);
                            s_apkLastLoggedPkg = apkFg;
                            s_apkLastLogMs = nowMs;
                        }
                        // Mark this file revision handled -> subsequent wakeups
                        // with identical mtime take the zero-I/O fast path above.
                        s_apkHandledMtime = st.st_mtime;
                        s_apkHandledPkg = apkFg;
                        return apkFg;
                    } else {
                        // File exists but content is empty or "unknown"
                        writeFile((std::string(STATE_DIR) + "/fg_apk_status").c_str(), "empty_or_unknown");
                        writeFile((std::string(STATE_DIR) + "/fg_apk_age").c_str(), (int)fileAge);
                        LOG_WARN("FG_APP: [APK] file=%s content empty/unknown (age=%lds)", apkFgPath, fileAge);
                    }
                } else {
                    // File exists but stale (>30s)
                    writeFile((std::string(STATE_DIR) + "/fg_apk_status").c_str(), "stale");
                    writeFile((std::string(STATE_DIR) + "/fg_apk_age").c_str(), (int)fileAge);
                    writeFile((std::string(STATE_DIR) + "/fg_apk_path").c_str(), apkFgPath);
                    LOG_WARN("FG_APP: [APK] file=%s stale (age=%lds > 30s)", apkFgPath, fileAge);
                }
            }
        }
        // All paths missing/exhausted -> APK not writing foreground_app.
        writeFile((std::string(STATE_DIR) + "/fg_apk_status").c_str(), "missing");
        LOG_WARN("FG_APP: [APK] foreground_app file not found in any path, falling back to module detection");
    }
    return "";
}

// Bug1 (real-device on Redmi Note10 Pro / Dimensity 1100): the top-app cpuset
// group does NOT contain only the resumed foreground activity. It also always
// contains system_server (and a handful of other persistent system daemons).
// /dev/cpuset/top-app/tasks is emitted in ascending PID order, so the OLD loop
// took the FIRST slash-free cmdline it saw — which was system_server (low PID)
// — and broke immediately. Result: foreground_app was stuck on "system_server"
// forever even after the user switched to a real app.
//
// These persistent system process names are NEVER the user's foreground activity,
// no matter that they live in top-app. Their cmdline is a bare token with no dot
// (system_server, surfaceflinger, ...), which we use as a second discriminator
// below.
static bool aatpeIsSystemDaemonName(const std::string& name) {
    static const char* kSystemDaemons[] = {
        "system_server", "surfaceflinger", "surfaceflinger2", "servicemanager",
        "zygote", "zygote64", "init", "ueventd", "logd", "logdreaders", "netd",
        "vold", "installd", "adbd", "lmkd", "healthd", "thermalserviced",
        "audioserver", "mediaserver", "cameraserver", "drmserver", "mediacodec",
        "mediaswcodec", "mediaextractor", "keystore", "gatekeeperd", "storaged",
        "incidentd", "statsd", "traced", "traced_probes", "perfetto", "kmkd",
        "muxreportd", "thermald", "powerhal", "android.hardware", "vendor",
        NULL
    };
    for (int i = 0; kSystemDaemons[i] != nullptr; i++) {
        if (name == kSystemDaemons[i]) return true;
    }
    return false;
}

// Layer 2: sysfs/cgroup nodes. Tries top-app cpuset first, then foreground
// cpuset (both cgroup v1 /dev/cpuset and v2 /sys/fs/cgroup/cpuset).
//
// Correctness rule (Bug1): the cgroup fast path may only return a package when it
// is CONFIDENT. A confident candidate is a slash-free cmdline that
//   (a) is NOT a known persistent system daemon (system_server/surfaceflinger/...),
//   (b) looks like a real Android application package name (contains a '.'),
//   (c) passes the shell-injection whitelist.
// If exactly ONE such candidate is found in top-app (preferred) / foreground, it
// is the resumed activity and we return it. If ZERO or MORE THAN ONE candidates are
// found we return "" so the caller falls through to the rate-limited dumpsys path
// (the authoritative source) instead of guessing — the fast path must NEVER
// hard-code "system_server".
static std::string aatpeCgroupConfidentPkg(const std::string& group) {
    std::vector<int> pids = aatpeReadCpusetPids(group);
    std::string candidate;
    int hits = 0;
    for (int pid : pids) {
        std::string cmdline = readFile(("/proc/" + std::to_string(pid) + "/cmdline").c_str());
        if (cmdline.empty()) continue;  // conversion failed -> let dumpsys decide
        // cmdline is NUL-separated; argv[0] (first token) is the process name.
        size_t nullPos = cmdline.find('\0');
        if (nullPos != std::string::npos) cmdline = cmdline.substr(0, nullPos);
        trim(cmdline);
        if (cmdline.empty()) continue;
        if (cmdline.find('/') != std::string::npos) continue;       // binary path, not a pkg
        if (aatpeIsSystemDaemonName(cmdline)) continue;              // never the fg activity
        if (cmdline.find('.') == std::string::npos) continue;         // bare token, not a pkg
        if (!aatpeIsValidPackageName(cmdline)) continue;              // injection guard
        if (hits == 0) candidate = cmdline;
        hits++;
        if (hits > 1) break;  // ambiguous -> caller must use dumpsys
    }
    return (hits == 1) ? candidate : std::string();
}

std::string detectForegroundFromSysfs() {
    // top-app is the authoritative Android foreground cpuset; foreground is the
    // secondary group. Prefer a confident top-app package, then foreground.
    std::string pkg = aatpeCgroupConfidentPkg("top-app");
    if (pkg.empty()) pkg = aatpeCgroupConfidentPkg("foreground");
    // pkg is "" on conversion failure / ambiguity -> caller falls through to
    // dumpsys. We deliberately do NOT synthesize "system_server" here.
    return pkg;
}

// FIX(v4.3.1, RC1): oom foreground-band rule, extracted as a PURE function so the
// hermetic host test can drive it. The real foreground app sits at oom_score_adj ~= 0
// (FOREGROUND_APP_ADJ=0, OEM top-app ~ -100); persistent/account/push services of OTHER
// apps sit at PERSISTENT_PROC_ADJ=-800 and cached apps at >= 900. We keep only the
// foreground band and prefer the valid app CLOSEST to 0 — never the most-negative one.
bool aatpeFgBandCandidate(int oom, int* outPreference) {
    static const int kPersistentAdjCeiling = -700; // PERSISTENT=-800 / SYSTEM=-900 below -> exclude
    static const int kBackgroundAdjFloor   = 400;  // VISIBLE=100/PERCEPTIBLE=200 fg-ish; cached >=900 exclude
    if (oom <= kPersistentAdjCeiling || oom >= kBackgroundAdjFloor) return false;
    if (outPreference) *outPreference = oom;  // smaller (closer to 0) = more foreground-like
    return true;
}

// v4.3.1 (P3-a): deterministic foreground candidate selection. Pure + hermetically
// testable. Ranking: 1) in top-app cgroup (strongest FG signal) > 2) oom closest to 0
// (|oom|) within the RC1 foreground band > 3) pid ascending tie-break. NO dependence on
// readdir order. Returns index into cands, or -1 if none.
struct FgCandidate {
    int oom = 0;
    int pid = 0;
    std::string pkg;
    bool topApp = false;
};
int aatpePickForegroundCandidate(const std::vector<FgCandidate>& cands) {
    int best = -1;
    for (int i = 0; i < (int)cands.size(); i++) {
        if (best < 0) { best = i; continue; }
        const FgCandidate& a = cands[i];
        const FgCandidate& b = cands[best];
        // Tier 1: top-app cgroup wins.
        if (a.topApp != b.topApp) {
            if (a.topApp) best = i;
            continue;
        }
        // Tier 2: |oom| closer to 0 wins.
        int da = std::abs(a.oom), db = std::abs(b.oom);
        if (da != db) {
            if (da < db) best = i;
            continue;
        }
        // Tier 3: deterministic pid tie-break (ascending).
        if (a.pid != b.pid) {
            if (a.pid < b.pid) best = i;
        }
    }
    return best;
}

// On-device: is pid in a top-app / foreground cgroup? (reads /proc/<pid>/cgroup).
bool procInTopAppCgroup(int pid) {
    std::string cg = readFile(("/proc/" + std::to_string(pid) + "/cgroup").c_str());
    if (cg.find("top-app") != std::string::npos) return true;
    if (cg.find("foreground") != std::string::npos) return true;
    return false;
}

// Layer 3: /proc scan for the process with oom_score_adj < 0 (foreground).
// Capped at 256 entries. Returns the best package or "" if none qualifies.
std::string detectForegroundFromProcessScan() {
    std::string pkg;
    {
        // Path 3: scan /proc for foreground-band app processes, then pick
        // DETERMINISTICALLY (top-app cgroup > |oom|~0 > pid tie-break). We now read ALL
        // candidates up to a generous sanity cap (8192) rather than truncating at 256 by
        // readdir order — the real foreground may be the 300th entry.
        DIR* procDir = opendir("/proc");
        if (procDir) {
            struct dirent* entry;
            std::vector<FgCandidate> cands;
            static const int kMaxProcScan = 8192;  // generous sanity cap, not a readdir truncation
            int scanned = 0;
            while ((entry = readdir(procDir)) != nullptr && scanned < kMaxProcScan) {
                int pid = parseIntSafe(entry->d_name);
                if (pid <= 0) continue;
                scanned++;
                std::string oomPath = "/proc/" + std::to_string(pid) + "/oom_score_adj";
                int oom = readInt(oomPath.c_str(), 999);
                int pref = 0;
                if (!aatpeFgBandCandidate(oom, &pref)) continue;  // excludes ≤-700 / ≥400
                std::string cmdline = readFile(("/proc/" + std::to_string(pid) + "/cmdline").c_str());
                if (cmdline.empty()) continue;
                size_t nullPos = cmdline.find('\0');
                if (nullPos != std::string::npos) cmdline = cmdline.substr(0, nullPos);
                trim(cmdline);
                if (cmdline.empty() || cmdline.find('/') != std::string::npos ||
                    cmdline.find('.') == std::string::npos) continue;
                if (aatpeIsSystemDaemonName(cmdline)) continue;
                if (!aatpeIsValidPackageName(cmdline)) continue;
                FgCandidate c;
                c.oom = oom; c.pid = pid; c.pkg = cmdline;
                c.topApp = procInTopAppCgroup(pid);
                cands.push_back(std::move(c));
            }
            closedir(procDir);
            int best = aatpePickForegroundCandidate(cands);
            if (best >= 0) pkg = cands[best].pkg;
            // fail-safe: if no valid candidate, pkg stays "" (caller tries next layer).
        }
    }
    return pkg;
}

// Layer 4: dumpsys activity/windows fallbacks (5 hardcoded commands, tried in
// order, each gated on pkg still empty). Returns the discovered package or "".
// D3/R8: parse the FULL `dumpsys activity activities` stdout for the focused
// foreground package. Resumed-activity lines look like:
//   mResumedActivity: ActivityRecord{7f3a u0 bin.mt.plus/.MainActivity}
//   topResumedActivity=ActivityRecord{... com.xiaomi.mirror/.MirrorActivity}
//
// R8 (split-screen / PiP): there can be MULTIPLE resumed activities (one per
// stack). The focused, focusable RENDERING task is the one the system itself
// tracks as `topResumedActivity` (stack-top, last-touched); a generic
// `mResumedActivity` can name a non-focused split or a background PiP window.
// We therefore collect `topResumedActivity` packages FIRST and fall back to the
// first valid `mResumedActivity` only when no topResumedActivity parsed. Pure +
// host-testable (no fork, no file IO). Zero package-name hard-coding.
std::string aatpeParseDumpsysFocusedPkg(const std::string& activitiesOut) {
    if (activitiesOut.empty()) return std::string();
    std::istringstream iss(activitiesOut);
    std::string line;
    std::string firstTop, firstResumed;
    while (std::getline(iss, line)) {
        std::string lower = line;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        bool isTop = lower.find("topresumedactivity") != std::string::npos;
        bool isResumed = !isTop && lower.find("mresumedactivity") != std::string::npos;
        if (!isTop && !isResumed) continue;
        std::string val = aatpeExtractDumpsysValue(line);
        if (val.empty()) continue;
        std::string pkg = aatpeExtractPackage(val);
        if (pkg.empty()) continue;
        if (isTop && firstTop.empty()) firstTop = pkg;
        if (isResumed && firstResumed.empty()) firstResumed = pkg;
    }
    if (!firstTop.empty()) return firstTop;     // focused task wins (R8)
    return firstResumed;
}

// D3: foreground-source precedence decision. dumpsys (mResumedActivity /
// topResumedActivity) is the AUTHORITATIVE resumed-activity signal; the cgroup
// top-app "confident" read and the /proc oom scan are only FALLBACK hints — on MIUI
// the top-app cgroup wrongly contains persistent services and mirror-casting
// (com.xiaomi.mirror) while the real resumed app (bin.mt.plus) also lives there, so
// a hits==1 cgroup read short-circuited dumpsys and stuck the foreground on mirror.
// RULE: when dumpsys yields a package it wins, even if cgroup disagrees. Pure +
// host-testable.
std::string aatpeFgSourceDecision(const std::string& dumpsysPkg,
                                  const std::string& cgroupPkg,
                                  const std::string& procPkg) {
    if (!dumpsysPkg.empty() && dumpsysPkg != "unknown") return dumpsysPkg;
    if (!cgroupPkg.empty() && cgroupPkg != "unknown") return cgroupPkg;
    if (!procPkg.empty() && procPkg != "unknown") return procPkg;
    return std::string();
}

// D3: is the dumpsys foreground result fetched at fetchedMs still fresh at nowMs?
// Once false, the cascade must NOT serve the cached dumpsys package: it re-probes
// (when the 1Hz gap allows) or drops to the cgroup fallback / "unknown" so a stale
// foreground cannot linger. Pure + host-testable.
bool aatpeFgDumpsysResultFresh(long long nowMs, long long fetchedMs) {
    return (nowMs - fetchedMs) < kFgDumpsysTtlMs;
}

// v4.4.1 P0: pure parse of `dumpsys activity activities` for the WINDOWING mode of
// the focused/resumed activity. Returns one of: "fullscreen", "freeform",
// "pictureInPicture", "splitScreen", "unknown". The module's fg recovery used to
// be purely package-name-edge triggered, so entering/exiting a small window
// (PiP/freeform) of the SAME app produced no edge and the fg threads stayed stuck
// in the background cpuset. This exposes the windowing mode as an observable state
// (frame_windowing_mode); the edge-independent periodic cgroup heal in placement.cpp
// does the actual recovery. Pure + host-testable: no fork, no file IO.
// WindowConfiguration constants (verified against AOSP): FULLSCREEN=1,
// PINNED=2 (picture-in-picture), SPLIT_SCREEN_PRIMARY=3, FREEFORM=5. Real dumpsys
// prints symbolic names too: mWindowingMode=pinned / freeform / split-screen-primary
// / fullscreen. Match BOTH symbols and numeric codes (red-team MINOR-1).
static std::string classifyWindowingLine(const std::string& line) {
    std::string s = line;
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    // PINNED = picture-in-picture (symbol "pinned", older dumpsys "pictureInPicture",
    // numeric code 2).
    if (s.find("pinned") != std::string::npos ||
        s.find("pictureinpicture") != std::string::npos ||
        s.find("windowingmode=2") != std::string::npos) return "pictureInPicture";
    // FREEFORM = freeform window (symbol "freeform", numeric code 5).
    if (s.find("freeform") != std::string::npos ||
        s.find("windowingmode=5") != std::string::npos) return "freeform";
    // SPLIT SCREEN (symbol "split-screen-primary"/"splitscreen", numeric code 3).
    if (s.find("split-screen") != std::string::npos ||
        s.find("splitscreen") != std::string::npos ||
        s.find("windowingmode=3") != std::string::npos) return "splitScreen";
    // FULLSCREEN (symbol "fullscreen", numeric code 1).
    if (s.find("fullscreen") != std::string::npos ||
        s.find("windowingmode=1") != std::string::npos) return "fullscreen";
    return "";
}
std::string aatpeParseWindowingMode(const std::string& activitiesOut) {
    if (activitiesOut.empty()) return "unknown";
    std::istringstream iss(activitiesOut);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(iss, line)) lines.push_back(line);
    // Locate the focused resumed activity: prefer topResumedActivity, else the
    // first mResumedActivity.
    long long focusIdx = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string l = lines[i];
        for (auto& c : l) c = (char)tolower((unsigned char)c);
        if (l.find("topresumedactivity") != std::string::npos) { focusIdx = (long long)i; break; }
    }
    if (focusIdx < 0) {
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string l = lines[i];
            for (auto& c : l) c = (char)tolower((unsigned char)c);
            if (l.find("mresumedactivity") != std::string::npos) { focusIdx = (long long)i; break; }
        }
    }
    // Scan a window of ~40 lines after the focused activity for an explicit mode.
    if (focusIdx >= 0) {
        size_t hi = std::min(lines.size(), (size_t)focusIdx + 40);
        for (size_t i = (size_t)focusIdx; i < hi; ++i) {
            std::string m = classifyWindowingLine(lines[i]);
            if (!m.empty()) return m;
        }
    }
    // Global fallback scan.
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string m = classifyWindowingLine(lines[i]);
        if (!m.empty()) return m;
    }
    return "unknown";
}

std::string detectForegroundFromDumpsys() {
    // D3/R8 PRIMARY: ONE `dumpsys activity activities` dump, parsed for the focused
    // resumed activity (mResumedActivity/topResumedActivity, multi-window aware).
    // This replaces the two `grep mResumedActivity | head -n 1` / `grep
    // topResumedActivity | head -n 1` fan-out (which forked twice and grabbed the
    // first resumed line, wrong in split-screen).
    std::string out = aatpeExec("dumpsys activity activities 2>/dev/null", "FG_ACT2");
    std::string pkg = aatpeParseDumpsysFocusedPkg(out);
    // v4.4.1 P0: expose the focused activity's windowing mode (PiP/freeform) as an
    // observable state. The periodic cgroup heal in placement.cpp already runs
    // edge-independent; this lets the user/doctor see WHY a same-package small-window
    // transition happened (no package-name edge).
    writeFile((std::string(STATE_DIR) + "/frame_windowing_mode").c_str(),
              aatpeParseWindowingMode(out).c_str());
    // Secondary fallbacks (only when the activities dump yields nothing).
    if (pkg.empty() || pkg == "unknown") {
        std::string o = aatpeExec("dumpsys window windows 2>/dev/null | grep -i mCurrentFocus | head -n 1", "FG_FOCUS");
        if (!o.empty()) {
            std::string val = aatpeExtractDumpsysValue(o);
            if (!val.empty()) pkg = aatpeExtractPackage(val);
        }
    }
    if (pkg.empty() || pkg == "unknown") {
        std::string o = aatpeExec("dumpsys window windows 2>/dev/null | grep -i mFocusedApp | head -n 1", "FG_FOCAPP");
        if (!o.empty()) {
            std::string val = aatpeExtractDumpsysValue(o);
            if (!val.empty()) pkg = aatpeExtractPackage(val);
        }
    }
    if (pkg.empty() || pkg == "unknown") {
        std::string o = aatpeExec("dumpsys activity top 2>/dev/null | grep -i ACTIVITY | head -n 1", "FG_TOPACT");
        if (!o.empty()) {
            size_t sp = o.find("ACTIVITY");
            if (sp != std::string::npos) pkg = aatpeExtractPackage(o.substr(sp + 8));
        }
    }
    return pkg;
}

// v4.2.6: fallback foreground detection for manual game overrides.
// When standard foreground detection fails (MIUI/SukiSU edge cases), check if
// any user-added game is currently the top/resumed activity. Returns the first
// matching manual game package, or empty string.
std::string gamelist::detectManualGameForeground() {
    std::lock_guard<std::mutex> lk(g_ovMutex);
    gameOverridesReloadLocked(false);
    if (g_ovCache.empty()) return "";
    std::string o = aatpeExec("dumpsys activity activities 2>/dev/null | grep -iE 'topResumedActivity|mResumedActivity' | head -n 3", "FG_MANUAL");
    if (o.empty()) return "";
    std::istringstream iss(o);
    std::string line;
    while (std::getline(iss, line)) {
        std::string val = aatpeExtractDumpsysValue(line);
        if (val.empty()) continue;
        std::string pkg = aatpeExtractPackage(val);
        if (!pkg.empty() && g_ovCache.count(pkg) > 0) return pkg;
    }
    return "";
}

// Layer 5: final fallback. Logs success/failure, sets the cache globals,
// writes the foreground_app + foreground_app_source state files, returns pkg.
std::string detectForegroundFallback(std::string pkg, time_t now) {
    (void)now;  // reserved for future rate-limit logging; unused for now
    if (pkg.empty() || pkg == "unknown") {
        pkg = "unknown";
        FRAME_LOG_INFO("FG_APP: detection FAILED (all 5 methods returned empty)");
    } else if (pkg != aatpeGetForegroundPackage()) {
        // only log when the foreground package actually CHANGES.
        // Previously this logged every detection (3-5x/sec on the same app),
        // wasting log CPU under a storm.
        FRAME_LOG_INFO("FG_APP: foreground=%s", pkg.c_str());
    }
    aatpeSetForegroundLocked(pkg);
    writeFile((std::string(STATE_DIR) + "/foreground_app").c_str(), pkg);
    // write foreground detection source indicator.
    // If APK installed but we reached here, it means APK data was invalid/stale
    // and we fell back to module detection -> "module_fallback".
    // If APK not installed, normal module detection -> "module".
    if (g_helperApkInstalled) {
        writeFile((std::string(STATE_DIR) + "/foreground_app_source").c_str(), "module_fallback");
    } else {
        writeFile((std::string(STATE_DIR) + "/foreground_app_source").c_str(), "module");
    }
    return pkg;
}

std::string aatpeDetectForegroundApp(bool forceRefresh) {
    time_t now = time(nullptr);
    long long nowMs = fgNowMs();

    // Drain the inotify fd (cpuset change hint). cgroup is now only a fallback;
    // the drain is kept for its side-effect of initialising the watch fd cheaply.
    fgInotifyDrain();

    // Layer 1: Helper APK takeover (trusted, early-return; own mtime gate).
    std::string apkResult = detectForegroundFromApk(now);
    if (!apkResult.empty()) {
        g_fgCgroupReadMs = nowMs;
        g_fgCgroupDirty = false;
        return apkResult;
    }

    // D3: PRIMARY = dumpsys activity activities (mResumedActivity/topResumedActivity).
    // Re-probe when forced or the 2s result TTL expired, but honour the 1Hz min gap
    // so we never fork()+sh+dumpsys on every 50ms loop tick. The fork runs OUTSIDE
    // g_foregroundMutex; the result is stored under the lock afterwards.
    long long lastReadMs = g_fgDumpsysReadMs.load();
    bool dumpsysFresh = (nowMs - lastReadMs) < kFgDumpsysTtlMs;
    if (forceRefresh || !dumpsysFresh) {
        if (nowMs - lastReadMs >= kFgDumpsysMinGapMs) {
            std::string fresh = detectForegroundFromDumpsys();   // may fork + binder
            {
                std::lock_guard<std::mutex> lk(g_foregroundMutex);
                g_fgDumpsysResult = fresh;
            }
            lastReadMs = nowMs;
            g_fgDumpsysReadMs.store(nowMs);
        }
        // Recompute freshness. If we could NOT re-probe (still inside the 1Hz gap),
        // the cached dumpsys value is now STALE -> we must NOT serve it; drop to the
        // cgroup fallback / "unknown" instead (no stale foreground residue).
        dumpsysFresh = (nowMs - lastReadMs) < kFgDumpsysTtlMs;
    }

    std::string dumpsysPkg;
    {
        std::lock_guard<std::mutex> lk(g_foregroundMutex);
        dumpsysPkg = g_fgDumpsysResult;
    }

    // Authoritative dumpsys result, still fresh -> it WINS (overrides any cgroup /
    // cached value; conflict trusts dumpsys). Same committed package: cheap return
    // without rewriting the state files / re-logging every 50ms.
    if (dumpsysFresh && !dumpsysPkg.empty() && dumpsysPkg != "unknown") {
        if (dumpsysPkg == aatpeGetForegroundPackage()) {
            return dumpsysPkg;
        }
        return detectForegroundFallback(dumpsysPkg, now);
    }

    // FALLBACK: dumpsys unavailable, stale, or empty. cgroup top-app (cheap, no
    // fork), then /proc oom scan. Non-authoritative hints only.
    std::string cgroupPkg = detectForegroundFromSysfs();
    std::string procPkg;
    if (cgroupPkg.empty() || cgroupPkg == "unknown") procPkg = detectForegroundFromProcessScan();

    // Precedence decision (dumpsys already failed here): best available fallback.
    std::string pkg = aatpeFgSourceDecision(dumpsysPkg, cgroupPkg, procPkg);
    return detectForegroundFallback(pkg, now);
}

bool detectGameProcess() {
    // P1-1: snapshot g_cfg to avoid torn read racing config hot-reload.
    Config gameCfg;
    snapshotConfig(gameCfg);
    if (!gameCfg.gameDetection) return false;
    std::string fg = aatpeDetectForegroundApp();
    if (!fg.empty() && fg != "unknown") {
        // First check user-configured GAME_PACKAGES list (unified with scoring.cpp)
        if (isPackageInGameList(fg)) return true;
        // Built-in exact game package matching (popular games)
        static const char* gamePkgs[] = {
            "com.activision.callofduty.shooter", "com.garena.game.codm", "com.tencent.tmgp.cod",
            "com.miHoYo.GenshinImpact", "com.miHoYo.ys", "com.HoYoverse.hkrpgoversea",
            "com.miHoYo.bh3", "com.kurogame.wutheringwaves", "com.epicgames.fortnite",
            "com.supercell.clashofclans", "com.supercell.clashroyale", "com.supercell.brawlstars",
            "com.riotgames.league.wildrift", "com.tencent.lolm", "com.dts.freefireth",
            "com.dts.freefiremax", "com.pubg.imobile", "com.tencent.tmgp.pubgmhd",
            "com.mojang.minecraftpe", "com.roblox.client", "com.ea.gp.apexlegendsmobilefps",
            "com.tencent.tmgp.sgame", "com.nianticlabs.pokemongo",
            nullptr
        };
        for (int i = 0; gamePkgs[i]; i++) {
            if (fg == gamePkgs[i]) return true;
        }
        // Check for known game keywords in package name (more precise)
        std::string fgLower = fg;
        // P3-11: std::transform must cast to unsigned char before tolower()
        // — passing a negative char (e.g. UTF-8 bytes >0x7F) is UB.
        std::transform(fgLower.begin(), fgLower.end(), fgLower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        static const char* gameKeywords[] = {
            ".game", "games.", "mihoyo", "hoyoverse", "supercell", "riotgames",
            "tencent.tmgp", "garena.game", "pubg", "genshin", "honkai", "codm",
            "fortnite", "minecraft", "roblox", "brawlstars", "clashofclans",
            "clashroyale", "wildrift", "freefire", "wutheringwaves", "apexlegends",
            nullptr
        };
        for (int i = 0; gameKeywords[i]; i++) {
            if (fgLower.find(gameKeywords[i]) != std::string::npos) return true;
        }
    }
    return false;
}

int detectBatteryState() {
    int level = readInt("/sys/class/power_supply/battery/capacity", 50);
    std::string status = readFile("/sys/class/power_supply/battery/status");
    if (status.find("Charging") != std::string::npos || status.find("charging") != std::string::npos) return 0;
    // P1-1: snapshot g_cfg for torn-read safety.
    Config battCfg;
    snapshotConfig(battCfg);
    if (level <= battCfg.batterySaverThreshold) return 2;
    return 1;
}

void autoSelectProfile() {
    // P1-1: snapshot g_cfg to avoid torn read racing config hot-reload.
    Config profCfg;
    snapshotConfig(profCfg);
    // the AUTO layer now VOTES instead of calling
    // loadProfile(). When auto is disabled or suppressed (emergency / manual lock /
    // Helper-APK runtime suppression) it abstains so a higher source
    // (MANUAL/EMERGENCY) or GLOBAL takes over.
    if (!profCfg.autoProfile || g_helperAutoSuppressed.load() || g_thermalEmergency || g_profileLock.load()) {
        aatpe_profile::profileRelease(aatpe_profile::ProfileSource::AUTO);
        return;
    }
    std::string newProfile = "balanced";
    if (detectGameProcess()) {
        newProfile = "gaming";
    } else {
        int batState = detectBatteryState();
        if (batState == 2) newProfile = "battery";
    }
    int maxTemp = readInt((std::string(STATE_DIR) + "/thermal_cpu_max").c_str(), 0);
    if (maxTemp >= profCfg.thermalProfileThreshold) newProfile = "thermal";
    // The arbiter decides whether this vote actually wins (APP/MANUAL/EMERGENCY
    // outrank AUTO); loadProfile() runs only if the winner changed.
    aatpe_profile::profileRequest(newProfile, aatpe_profile::ProfileSource::AUTO);
}

// ============================================================================

// ============================================================================
// Frame-Synchronized Scheduling
// VSYNC-aligned lightweight decision for critical threads.
// Triggers 2ms before frame boundary, only migrates UI/RENDER/GAME threads.
// ============================================================================

namespace aatpe {
namespace frameaware {

// ROUND-007c: bounded sustained-jank boost vote (set by the frame-latency loop).
// placement/spillover reads this to arm an extra critical-thread promotion while
// the foreground app janks. Clears as soon as jank recovers.
std::atomic<bool> g_jankBoostVote{false};
bool jankBoostVoteActive() { return g_jankBoostVote.load(std::memory_order_relaxed); }

// FIX(v4.3.1, RC3): per-app framestats jank signal. The accurate per-app jankyPct
// (from gfxinfo framestats IntendedVsync) used to only feed the WebUI panel, while
// the boost vote relied solely on the GLOBAL SF latency average — which is diluted by
// the smooth system-UI/status-bar layers. This atomic lets the per-app signal VOTE too.
// Set by the throttled framestats block when the FOREGROUND app shows >=jankPct or a
// measured fps well below the panel refresh rate.
std::atomic<bool> g_appFsJankActive{false};
// P2-fix: per-cycle missRate>=0.05 input from aatpeSetFrameJank (timestats/binder
// channels). This is an INPUT to the single fusion voter (aatpeFusionJankVote), NOT a
// direct vote — aatpeSetFrameJank no longer writes g_jankBoostVote itself.
std::atomic<bool> g_frameMissJankInput{false};

struct FrameAwareState {
    std::atomic<int> refreshRate{60};      // Hz, default 60
    std::atomic<int> frameIntervalMs{16};   // 1000/refreshRate
    std::atomic<long long> decisions{0};     // frame-aware decision count
    std::atomic<long long> migrations{0};    // frame-aware migration count
    std::atomic<bool> enabled{true};
    std::atomic<long long> lastDecisionMs{0};
    std::atomic<int> overrunCount{0};        // times decision exceeded 50% of frame interval
    std::atomic<long long> lastOverrunMs{0};   // timestamp of last overrun for recovery
};

static FrameAwareState g_frameAware;

// ============================================================================
// #28 event-driven refresh-rate refresh 
// ----------------------------------------------------------------------------
// The daemon historically re-read the refresh rate on a fixed TTL (peak=10s,
// current=3s). When the user/game switches display mode (e.g. 60->120Hz) the
// scheduler kept pacing to the old frame interval until the next poll. We now
// ALSO watch a small set of on-device display nodes with inotify: any write to
// them flips g_refreshRateDirty, and aatpeDetectRefreshRate() then bypasses the
// cache and re-detects immediately. The TTL poll stays as the backstop — if the
// device exposes none of the watched nodes, this is a no-op and polling wins.
// ============================================================================
std::atomic<bool> g_refreshRateDirty{false};
static std::atomic<bool> g_refreshWatchThreadRunning{false};
static std::thread g_refreshWatchThread;

static void refreshRateWatchLoop() {
    // Candidate nodes that SurfaceFlinger / the display HAL touch on mode change.
    // We watch their parent DIRECTORY so create/delete/write all wake us.
    static const char* kWatchDirs[] = {
        "/sys/class/drm",
        "/sys/class/graphics/fb0",
        "/data/system/display_settings.xml",
    };
    int inofd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inofd < 0) return;
    bool any = false;
    for (const char* d : kWatchDirs) {
        // Watch directories; for a plain file watch its parent.
        std::string path = d;
        struct stat st;
        if (stat(d, &st) != 0) continue;
        bool isDir = S_ISDIR(st.st_mode);
        std::string watchTarget = isDir ? path : path.substr(0, path.rfind('/'));
        if (watchTarget.empty()) watchTarget = "/";
        int wd = inotify_add_watch(inofd, watchTarget.c_str(),
                                  IN_MODIFY | IN_CREATE | IN_DELETE | IN_CLOSE_WRITE);
        if (wd >= 0) any = true;
    }
    if (!any) { close(inofd); return; }
    char buf[4096] __attribute__((aligned(16)));
    while (g_refreshWatchThreadRunning.load(std::memory_order_relaxed)) {
        struct pollfd pfd{inofd, POLLIN, 0};
        int pr = poll(&pfd, 1, 500);  // 500ms wake so we can exit promptly
        if (pr <= 0) continue;
        ssize_t n = read(inofd, buf, sizeof(buf));
        if (n > 0) {
            // Parse inotify events: only invalidate cache when the changed file
            // is display-mode related (modes/status/connector). This avoids
            // unnecessary dumpsys calls from unrelated DRM node churn.
            bool relevant = false;
            for (char* ptr = buf; ptr + sizeof(struct inotify_event) <= buf + n; ) {
                const struct inotify_event* ev = (const struct inotify_event*)ptr;
                if (ev->len > 0) {
                    std::string name(ev->name, strnlen(ev->name, ev->len));
                    // Relevant display-mode filenames.
                    if (name.find("modes") != std::string::npos ||
                        name.find("status") != std::string::npos ||
                        name.find("connector") != std::string::npos ||
                        name.find("display") != std::string::npos ||
                        name.find("RefreshRate") != std::string::npos ||
                        name.find("refresh") != std::string::npos) {
                        relevant = true;
                        break;
                    }
                } else {
                    // Event on the watched directory itself (no name) — treat
                    // as relevant to be safe.
                    relevant = true;
                    break;
                }
                ptr += sizeof(struct inotify_event) + ev->len;
            }
            if (relevant) {
                g_refreshRateDirty.store(true, std::memory_order_relaxed);
            }
        }
    }
    close(inofd);
}

void refreshRateEventStart() {
    bool expected = false;
    if (!g_refreshWatchThreadRunning.compare_exchange_strong(expected, true)) return;
    // item 13: keep the thread joinable (no detach). Daemon exit calls
    // refreshRateEventStop() which clears the running flag and joins it.
    g_refreshWatchThread = std::thread(refreshRateWatchLoop);
}

void refreshRateEventStop() {
    g_refreshWatchThreadRunning.store(false, std::memory_order_relaxed);
    // The watch loop polls with a 500ms timeout, so it exits promptly after the
    // flag clears. Join to reclaim the thread instead of leaking a detached one.
    if (g_refreshWatchThread.joinable()) {
        g_refreshWatchThread.join();
    }
}

// Detect current refresh rate and update frame interval
void updateRefreshRate() {
    int rate = aatpeDetectRefreshRate();
    // FIX(v4.3.1, F4): on a transient probe failure do NOT snap to 60Hz. A 120Hz panel
    // whose probe momentarily fails would otherwise get its frame budget / decision
    // dwell doubled (16.7ms vs 8.3ms) every cycle, stuttering the very app we protect.
    // Keep the last known GOOD rate; only fall back to a conservative 60Hz when we have
    // never observed a valid rate (and that first-fallback is inherently "unknown").
    static int s_lastGoodRate = 0;
    bool valid = (rate >= refresh::kMinRate && rate <= refresh::kMaxRate);
    if (valid) {
        s_lastGoodRate = rate;
    } else if (s_lastGoodRate > 0) {
        rate = s_lastGoodRate;   // hold last known value instead of locking 60Hz
    } else {
        rate = refresh::kDefaultRate;  // first-ever probe failure: conservative, unknown
    }
    g_frameAware.refreshRate.store(rate);
    g_frameAware.frameIntervalMs.store(1000 / rate);
}

int getRefreshRate() { return g_frameAware.refreshRate.load(); }
int getFrameIntervalMs() { return g_frameAware.frameIntervalMs.load(); }
long long getDecisionCount() { return g_frameAware.decisions.load(); }
long long getMigrationCount() { return g_frameAware.migrations.load(); }

// Check if it's time for a frame-aware lightweight decision.
// This paces decisions to the estimated frame period (refresh rate), NOT to a
// real kernel VSYNC event.
bool shouldTriggerFrameAware(long long nowMs) {
    // Recovery: if disabled due to overruns, re-enable after 30s without overruns.
    if (!g_frameAware.enabled.load()) {
        long long lastOverrun = g_frameAware.lastOverrunMs.load();
        if (lastOverrun > 0 && nowMs - lastOverrun > 30000) {
            g_frameAware.overrunCount.store(0);
            g_frameAware.enabled.store(true);
            FRAME_LOG_INFO("Frame-Aware: re-enabled after 30s without overruns");
            g_frameAware.lastDecisionMs.store(0);  // reset cooldown so recovery triggers immediately
        } else {
            FRAME_LOG_DEBUG("Frame-Aware: disabled, recovery check: lastOverrun=%lld elapsed=%lldms (need >30000ms)",
                      lastOverrun, nowMs - lastOverrun);
            return false;
        }
    }

    int interval = g_frameAware.frameIntervalMs.load();
    if (interval <= 0) return false;

    // Cooldown: don't trigger more than once per frame interval
    long long last = g_frameAware.lastDecisionMs.load();
    if (nowMs - last < interval) return false;

    return true;
}

// Execute frame-aware lightweight decision.
// Only migrates critical threads (UI/RENDER/GAME), skips expensive operations.
void executeFrameAwareDecision() {
    long long startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    g_frameAware.decisions.fetch_add(1);
    g_frameAware.lastDecisionMs.store(startMs);

    // Call lightweight migration (implemented in placement.cpp)
    int migrated = lightweightMigrateCriticalThreads();
    if (migrated > 0) {
        g_frameAware.migrations.fetch_add(migrated);
    }

    // Overrun protection: if decision took >50% of frame interval, count it.
    // v4.2.7: VM container games are exempt — host vsync is decoupled from guest
    // rendering, so the host frame interval is meaningless for VM inner games.
    long long endMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int elapsed = (int)(endMs - startMs);
    int interval = g_frameAware.frameIntervalMs.load();
    std::string fgPkg = aatpeGetForegroundPackage();
    VmContainerInfo vmCheck = detectVmContainerGame(fgPkg);
    bool isVmGame = vmCheck.valid;
    if (!isVmGame && elapsed > interval / 2) {
        g_frameAware.overrunCount.fetch_add(1);
        g_frameAware.lastOverrunMs.store(endMs);
        // If overruns happen frequently, temporarily disable frame-aware pacing.
        // (Exempt for VM container games — host vsync decoupled from guest.)
        if (g_frameAware.overrunCount.load() > 10) {
            g_frameAware.enabled.store(false);
            LOG_WARN("Frame-Aware: disabled due to excessive overruns (>10)");
        }
    } else if (isVmGame) {
        // Reset overrun count when running a VM game (host interval is irrelevant).
        g_frameAware.overrunCount.store(0);
    }
}

// Write frame-aware stats to state files.
// On-disk filenames keep the legacy "vsync_*" names (stable WebUI/CLI contract).
void writeStats(const std::string& stateDir) {
    writeFile((stateDir + "/vsync_refresh_rate").c_str(), g_frameAware.refreshRate.load());
    writeFile((stateDir + "/vsync_frame_interval").c_str(), g_frameAware.frameIntervalMs.load());
    writeFile((stateDir + "/vsync_decisions").c_str(), (long long)g_frameAware.decisions.load());
    writeFile((stateDir + "/vsync_migrations").c_str(), (long long)g_frameAware.migrations.load());
    writeFile((stateDir + "/vsync_enabled").c_str(), g_frameAware.enabled.load() ? "1" : "0");
    writeFile((stateDir + "/vsync_overruns").c_str(), g_frameAware.overrunCount.load());
}

} // namespace frameaware
} // namespace aatpe
