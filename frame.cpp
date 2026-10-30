#include <cmath>
#include "aatped.h"
#include "frame_binder.h"
#include "aatped_apklabel.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <sys/prctl.h>
#include <sys/inotify.h>
#include <unistd.h>

#define FRAME_LOG_INFO(...)  do { if (g_cfg.logFrame) logMsg(2, __VA_ARGS__); } while(0)
#ifdef AATPE_DEBUG
  #define FRAME_LOG_DEBUG(...) do { if (g_cfg.logFrame) do { if ((g_debugSampleCounter.fetch_add(1,std::memory_order_relaxed)%100)==0) logMsg(3,__VA_ARGS__); } while(0); } while(0)
#else
  #define FRAME_LOG_DEBUG(...) ((void)0)
#endif

#include <poll.h>
#include <cctype>
#include <dirent.h>
using namespace aatpe;
using namespace aatpe_ml;

static bool aatpeIsValidPackageName(const std::string& p);

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

static float aatpeExtractFloatAfterWordBoundary(const std::string& haystack, const char* key) {
    size_t pos = 0;
    std::string k(key);
    while ((pos = haystack.find(k, pos)) != std::string::npos) {
        if (pos > 0) {
            char c = haystack[pos - 1];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                pos += k.size();
                continue;
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

static int aatpeParseCurrentFromSurfaceFlinger(const std::string& sfOutput) {
    if (sfOutput.empty()) return 0;
    std::istringstream iss(sfOutput);
    std::string ln;

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

    iss.clear();
    iss.seekg(0, std::ios::beg);
    while (std::getline(iss, ln)) {
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

static int aatpeParseCurrentFromDisplayBaseInfo(const std::string& displayOutput) {
    if (displayOutput.empty()) return 0;
    std::istringstream iss(displayOutput);
    std::string ln;
    while (std::getline(iss, ln)) {
        size_t bdiPos = ln.find("mBaseDisplayInfo=");
        if (bdiPos == std::string::npos) continue;
        if (bdiPos > 0 && (isalnum((unsigned char)ln[bdiPos-1]) || ln[bdiPos-1] == '_')) continue;

        int currentModeId = -1;
        {
            size_t modePos = 0;
            while ((modePos = ln.find("mode ", modePos)) != std::string::npos) {
                if (modePos > 0) {
                    char c = ln[modePos - 1];
                    if (isalnum((unsigned char)c) || c == '_') {
                        modePos += 5;
                        continue;
                    }
                }
                size_t numStart = modePos + 5;
                if (numStart < ln.size() && isdigit((unsigned char)ln[numStart])) {
                    int id = 0;
                    size_t i = numStart;
                    while (i < ln.size() && isdigit((unsigned char)ln[i])) {
                        id = id * 10 + (ln[i] - '0');
                        if (id > 100000) break;
                        i++;
                    }
                    currentModeId = id;
                    break;
                }
                modePos += 5;
            }
        }

        if (currentModeId >= 0) {
            size_t modesPos = ln.find("modes [");
            if (modesPos == std::string::npos) modesPos = ln.find("modes[");
            if (modesPos != std::string::npos) {
                size_t bracketStart = ln.find('[', modesPos);
                if (bracketStart != std::string::npos) {
                    size_t bracketEnd = ln.find(']', bracketStart);
                    if (bracketEnd == std::string::npos) bracketEnd = ln.size();
                    std::string modesList = ln.substr(bracketStart + 1, bracketEnd - bracketStart - 1);

                    size_t entryStart = 0;
                    while (entryStart < modesList.size()) {
                        size_t ob = modesList.find('{', entryStart);
                        if (ob == std::string::npos) break;
                        size_t cb = modesList.find('}', ob);
                        if (cb == std::string::npos) break;
                        std::string entry = modesList.substr(ob, cb - ob + 1);

                        int entryId = -1;
                        size_t idPos = entry.find("id=");
                        if (idPos != std::string::npos) {
                            size_t idNumStart = idPos + 3;
                            if (idNumStart < entry.size() && isdigit((unsigned char)entry[idNumStart])) {
                                int id = 0;
                                size_t i = idNumStart;
                                while (i < entry.size() && isdigit((unsigned char)entry[i])) {
                                    id = id * 10 + (entry[i] - '0');
                                    if (id > 100000) break;
                                    i++;
                                }
                                entryId = id;
                            }
                        }

                        if (entryId == currentModeId) {
                            float f = aatpeExtractFloatAfterWordBoundary(entry, "fps");
                            if (f >= (float)refresh::kMinRate && f <= (float)refresh::kMaxRate) return (int)lroundf(f);
                            float f2 = aatpeExtractFloatAfterWordBoundary(entry, "refreshRate");
                            if (f2 >= (float)refresh::kMinRate && f2 <= (float)refresh::kMaxRate) return (int)lroundf(f2);
                        }
                        entryStart = cb + 1;
                    }
                }
            }
        }

        {
            size_t rfrPos = ln.find("renderFrameRate");
            if (rfrPos != std::string::npos) {
                if (rfrPos == 0 || !(isalnum((unsigned char)ln[rfrPos-1]) || ln[rfrPos-1] == '_')) {
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

static int aatpeParseRefreshFromLatency(const std::string& latencyOutput) {
    if (latencyOutput.empty()) return 0;
    std::istringstream iss(latencyOutput);
    std::string ln;
    while (std::getline(iss, ln)) {
        trim(ln);
        if (ln.empty()) continue;
        long long v[3];
        int n = sscanf(ln.c_str(), "%lld %lld %lld", &v[0], &v[1], &v[2]);
        if (n == 3) {
            if (v[2] >= refresh::kMinVsyncPeriodNs && v[2] <= refresh::kMaxVsyncPeriodNs) {
                int r = (int)(refresh::kOneSecondNs / v[2]);
                if (r >= refresh::kMinRate && r <= refresh::kMaxRate) return r;
            }
        } else if (n == 1) {
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
            std::string status = readFile((basePath + "/status").c_str());
            if (!status.empty() && status.find("connected") == std::string::npos) continue;
            std::string modesPath = basePath + "/modes";
            std::ifstream f(modesPath.c_str());
            if (!f) continue;
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty() || line[0] != '*') continue;
                int r = parseModeRefresh(line.substr(1));
                if (r > activeBest) activeBest = r;
                break;
            }
        }
        closedir(drmDir);
        if (activeBest >= refresh::kMinRate && activeBest <= refresh::kMaxRate) {
            return activeBest;
        }
    }

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
            if (v > 1000000) break;
            v = v * 10 + (line[i] - '0');
            i++;
        }
        if (v >= refresh::kMinRate && v <= refresh::kMaxRate && v > best) best = v;
    }
    return best;
}

bool detectGameProcess();

static int aatpeDetectPeakRefreshRate() {
    int best = 0;
    {
        int r = aatpeReadSysfsRefreshRate();
        if (r > 0) best = r;
    }
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
    if (best == 0) {
        std::string o = aatpeExec("dumpsys window 2>/dev/null | grep -i -E 'refresh|fps' | head -n 5", "VSYNC_WIN");
        int r = aatpeFirstInt(o);
        if (r >= refresh::kMinRate && r <= refresh::kMaxRate) best = r;
    }
    if (best == 0) best = refresh::kDefaultRate;
    return best;
}

static int aatpeDetectCurrentRefreshRate() {
    {
        int r = aatpeReadSysfsCurrentRefreshRate();
        if (r >= refresh::kMinRate && r <= refresh::kMaxRate) {
            FRAME_LOG_DEBUG("Current refresh rate (sysfs DRM): %dHz", r);
            return r;
        }
    }
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

    FRAME_LOG_DEBUG("Current refresh rate unavailable (all AOSP sources failed), falling back to peak");
    return aatpeDetectPeakRefreshRate();
}

namespace aatpe { namespace frameaware { extern std::atomic<bool> g_refreshRateDirty; } }
namespace aatpe { namespace frameaware { extern std::atomic<bool> g_jankBoostVote; } }
namespace aatpe { namespace frameaware { extern std::atomic<bool> g_appFsJankActive; } }
namespace aatpe { namespace frameaware { extern std::atomic<bool> g_frameMissJankInput; } }
namespace aatpe { namespace frameaware { bool aatpeFusionJankVote(bool, int&, int&, int, int, bool); } }
int aatpeDetectRefreshRate() {
    Config cfg; snapshotConfig(cfg);
    static int s_cachedRate = 0;
    static time_t s_cacheTime = 0;
    static std::string s_lastMode = "";
    time_t now = time(nullptr);
    const std::string mode = cfg.refreshRateMode;
    {
        std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
        if (s_lastMode != mode) {
            s_cachedRate = 0;
            s_cacheTime = 0;
            s_lastMode = mode;
            FRAME_LOG_INFO("Refresh rate mode changed to '%s', cache cleared", mode.c_str());
        }
        int cacheTtl = (g_refreshRateModeEnum == RefreshRateMode::CURRENT) ? refresh::kCurrentCacheTtl : refresh::kPeakCacheTtl;
        bool dirty = aatpe::frameaware::g_refreshRateDirty.exchange(false, std::memory_order_relaxed);
        if (dirty) {
            static std::atomic<long long> s_lastForcedRedetectMs{0};
            long long nowRdMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            long long prevRd = s_lastForcedRedetectMs.load(std::memory_order_relaxed);
            if (prevRd != 0 && (nowRdMs - prevRd) < 5000) {
                dirty = false;
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
    {
        std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
        s_cachedRate = best;
        s_cacheTime = now;
    }
    return best;
}

int aatpeDetectSFLatency(int refreshRate, int& outMaxLat, bool& outValid) {
    outValid = false;
    static int s_cachedAvg = 0;
    static int s_cachedMax = 0;
    static bool s_cachedValid = false;
    static time_t s_cacheTime = 0;
    time_t now = time(nullptr);
    {
        std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
        if (s_cachedAvg > 0 && (now - s_cacheTime) < aatpe::frame_latency::kSfLatCacheTtlSec) {
            outMaxLat = s_cachedMax;
            outValid = s_cachedValid;
            return s_cachedAvg;
        }
    }
    {
        static long long s_lastBinderTryMs = 0;
        static int s_binderRetryCount = 0;
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!aatpe::frame_binder::isAlive()) {
            long long backoffMs = 5000LL << std::min(s_binderRetryCount, 3);
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
    if (!outValid) {
        static const char* kTimelineNodes[] = {
            "/sys/kernel/debug/surfaceflinger/frame_timeline",
            "/sys/kernel/debug/surfaceflinger/FrameTimeline",
        };
        for (const char* node : kTimelineNodes) {
            std::string raw = readFile(node);
            if (raw.empty()) continue;
            std::vector<long long> ts;
            std::istringstream iss(raw);
            long long v;
            while (iss >> v) ts.push_back(v);
            if (ts.size() >= 4) {
                std::vector<long long> gaps;
                for (size_t i = 1; i < ts.size(); ++i) {
                    long long g = ts[i] - ts[i-1];
                    if (g > 0 && g < 200000000LL) gaps.push_back(g);
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
            break;
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
            if (!actualLatUs.empty()) {
                long long sum = 0;
                for (long long us : actualLatUs) sum += us;
                avgLat = (int)(sum / (long long)actualLatUs.size());
                long long mx = 0;
                for (long long us : actualLatUs) if (us > mx) mx = us;
                maxLat = (int)mx;
                bool hasRealVariance = false;
                if (avgLat > 0) {
                    for (long long us : actualLatUs) {
                        double dev = std::abs((double)us - (double)avgLat) / (double)avgLat;
                        if (dev > 0.25) { hasRealVariance = true; break; }
                    }
                }
                double panelPeriodUs = (refreshRate >= refresh::kMinRate && refreshRate <= refresh::kMaxRate)
                                       ? (double)refresh::kOneSecondUs / (double)refreshRate
                                       : 16667.0;
                bool steadyOverBudget = (avgLat >= (long long)(panelPeriodUs * 1.5));
                outValid = hasRealVariance || steadyOverBudget;
            } else if (baseUs > 0) {
                avgLat = baseUs; maxLat = baseUs * 2;
            } else if (s_cachedMinIntervalUs > 0) {
                avgLat = s_cachedMinIntervalUs; maxLat = avgLat * sf_latency::kMaxMultiplier;
            }
        }
    }
    if (!outValid) {
        std::string fgPkg = aatpeGetForegroundPackage();
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
                    if (gline.find("percentile:") != std::string::npos) {
                        int valMs = 0;
                        size_t colonPos = gline.find(':');
                        if (colonPos != std::string::npos) {
                            std::string numPart = gline.substr(colonPos + 1);
                            trim(numPart);
                            valMs = (int)aatpeFirstFloat(numPart);
                        }
                        if (gline.rfind("50th", 0) == 0) p50 = valMs;
                        else if (gline.rfind("95th", 0) == 0) p95 = valMs;
                        else if (gline.rfind("99th", 0) == 0) p99 = valMs;
                    }
                }
                if (p50 > 0) {
                    avgLat = p50 * 1000;
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
        maxLat = avgLat * sf_latency::kMaxMultiplier;
    }
    if (maxLat < LAT_MIN_US || maxLat > LAT_MAX_US * 2) maxLat = avgLat;
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
    if (periodNs <= 0 || periodNs < 5000000LL || periodNs > 200000000LL) return r;
    r.available = true;
    r.hz = 1e9f / (float)periodNs;
    r.intervalMs = (float)periodNs / 1e6f;
    std::istringstream ss(out);
    std::string line; std::getline(ss, line);
    const long long kInt64Max = 9223372036854775807LL;
    long long firstPresent = 0, lastPresent = 0;
    int presentedFrames = 0;
    while (std::getline(ss, line)) {
        long long a = 0, b = 0, c = 0;
        int ncol = sscanf(line.c_str(), "%lld %lld %lld", &a, &b, &c);
        if (ncol != 2 && ncol != 3) continue;
        if (a == -32768 || b == -32768) continue;
        const long long kTsMaxNs = 2000000000000LL;
        long long end = (ncol == 3) ? c : b;
        if (ncol == 3 && (a == -32768 || c == -32768)) continue;
        if (end <= 0 || end >= kInt64Max || end > kTsMaxNs) continue;
        if (a <= 0 || a > kTsMaxNs) continue;
        r.samples++;
        if (firstPresent == 0) firstPresent = end;
        lastPresent = end;
        presentedFrames++;
        if ((end - a) > periodNs) r.miss++;
    }
    if (presentedFrames >= 2 && lastPresent > firstPresent) {
        double spanSec = (double)(lastPresent - firstPresent) / 1e9;
        if (spanSec > 0.05) {
            r.renderFps = (double)(presentedFrames - 1) / spanSec;
            if (r.renderFps < 0.0) r.renderFps = 0.0;
        }
    }
    return r;
}

namespace aatpe {
namespace frameaware {

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

bool gameSamplingRequired(bool sfLatValid, bool fgIsGame) {
    return fgIsGame || !sfLatValid;
}

struct FrameLatencyAlarm { bool latCritical; bool jank; };
FrameLatencyAlarm aatpeFrameLatencyAlarm(
        bool sfLatValid, int avgLatUs, int maxLatUs, int effectiveThresholdUs,
        int frameTimeUs, float jankThresholdPct, bool alarmAllowed) {
    FrameLatencyAlarm r{false, false};
    if (!alarmAllowed) return r;
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

struct GameFpsResolve {
    double fps = 0.0;
    const char* source = "degraded";
    bool degraded = true;
};
GameFpsResolve resolveGameFps(double renderFps, double , int gedFps,
                              const char* measuredSource) {
    GameFpsResolve r;
    if (renderFps > 1.0) {
        r.fps = renderFps;
        r.source = (measuredSource && *measuredSource) ? measuredSource : "latency";
        r.degraded = false;
        return r;
    }
    if (gedFps >= 1 && gedFps <= 240) {
        r.fps = (double)gedFps;
        r.source = "ged_composer";
        r.degraded = false;
        return r;
    }
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

std::string extractBracketedPkg(const std::string& layer) {
    auto l = layer.find('[');
    auto r = layer.find(']');
    if (l == std::string::npos || r == std::string::npos || r <= l) return "";
    return layer.substr(l + 1, r - l - 1);
}

static std::string normalizeCore(std::string core) {
    trim(core);
    size_t rp;
    while ((rp = core.rfind(')')) != std::string::npos) {
        size_t lp = core.rfind('(', rp);
        if (lp == std::string::npos) break;
        core.erase(lp, rp - lp + 1);
    }
    trim(core);
    size_t h = core.rfind('#');
    if (h != std::string::npos) core.erase(h);
    trim(core);
    if (!core.empty() && core.back() == '@') core.pop_back();
    trim(core);
    return core;
}

static std::string extractLayerCore(const std::string& layer) {
    std::string br = extractBracketedPkg(layer);
    if (!br.empty()) return normalizeCore(br);
    auto dash = layer.rfind(" - ");
    if (dash != std::string::npos) {
        std::string tail = layer.substr(dash + 3);
        if (tail.find('/') != std::string::npos || tail.find('.') != std::string::npos)
            return normalizeCore(tail);
    }
    return normalizeCore(layer);
}

bool layerReferencesPkg(const std::string& core, const std::string& fgPkg) {
    if (core.empty() || fgPkg.empty()) return false;
    auto slash = core.find('/');
    if (slash != std::string::npos) {
        return core.substr(0, slash) == fgPkg;
    }
    if (core == fgPkg) return true;
    return core.size() > fgPkg.size() &&
           core.compare(0, fgPkg.size(), fgPkg) == 0 &&
           core[fgPkg.size()] == '.';
}

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

static bool isSurfaceLike(const std::string& layer) {
    return layer.find("SurfaceView") != std::string::npos ||
           layer.find("SurfaceTexture") != std::string::npos ||
           layer.find("TextureView") != std::string::npos ||
           layer.find("(BLAST)") != std::string::npos;
}

static std::string sanitizeCandidate(std::string s) {
    std::string out;
    for (char c : s) {
        if (c >= 0x20 && c < 0x7f) out.push_back(c);
        if (out.size() >= 96) break;
    }
    return out;
}

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

        if (candCount < 8) {
            if (candCount) candList += " | ";
            candList += sanitizeCandidate(ly);
            candCount++;
        }

        int score = 0;
        if (isGameEngineClass(core)) score += 100;
        if (isSurfaceLike(ly)) score += 10;
        if (ly.find("(BLAST)") != std::string::npos) score += 5;
        if (forceGame && score == 0) score = 1;

        if (score > bestScore) { bestScore = score; best = ly; }
    }

    if (outCandidates) *outCandidates = candList.empty() ? "none" : candList;

    if (best.empty()) {
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
        if (!r.available || r.samples == 0) continue;
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
    out.frameUnavailable = true;
    out.reason = "no_valid_frames";
    return out;
}

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
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        trim(line);
        if (line.empty()) continue;
        if (aatpeValidPkgCharName(line)) out.insert(line);
    }
    return out;
}

std::vector<std::string> parsePmPackageList(const std::string& pmOutput) {
    std::vector<std::string> out;
    std::istringstream ss(pmOutput);
    std::string line;
    while (std::getline(ss, line)) {
        trim(line);
        const char* prefix = "package:";
        if (line.compare(0, 8, prefix) != 0) continue;
        std::string pkg = line.substr(8);
        trim(pkg);
        size_t sp = pkg.find_first_of(" \t");
        if (sp != std::string::npos) pkg.erase(sp);
        if (aatpeValidPkgCharName(pkg)) out.push_back(pkg);
    }
    return out;
}

static double snapToStandardPeriod(double observedMs) {
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
    if (bestDiff / best < 0.2) return best;
    return observedMs;
}

static void finishTimestatsBlock(const std::string& layer, long long totalFrames,
                                 double avgFpsFromDump,
                                 const std::vector<std::pair<double, long long>>& hist,
                                 TimestatsResult& res) {
    (void)avgFpsFromDump;
    if (totalFrames <= 0 || hist.empty()) return;
    res.available = true;
    res.layer = layer;
    res.totalFrames = (int)totalFrames;

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

    double longGapThresholdMs = std::max(250.0, 5.0 * targetPeriodMs);
    long long activeFrames = 0;
    double activeWeightedSum = 0.0;
    long long jankyFrames = 0;
    for (auto& b : hist) {
        if (b.first > longGapThresholdMs) continue;
        activeFrames += b.second;
        activeWeightedSum += b.first * b.second;
        if (b.first >= 1.5 * targetPeriodMs) {
            jankyFrames += b.second;
        }
    }
    res.activeFrames = (int)activeFrames;

    double activeAvgInterval = (activeFrames > 0) ? (activeWeightedSum / activeFrames) : 16.67;
    double activeFps = (activeAvgInterval > 0.01) ? (1000.0 / activeAvgInterval) : 0.0;
    res.avgFps = activeFps;

    double jankRate = (activeFrames > 0) ? ((double)jankyFrames / (double)activeFrames) : 0.0;
    if (jankRate < 0.0) jankRate = 0.0;
    if (jankRate > 1.0) jankRate = 1.0;
    res.missRate = jankRate;
    res.onCadenceRatio = (activeFrames > 0) ? (1.0 - jankRate) : 0.0;

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
    res.hist = hist;
}

static long long s_timestatsParsePasses = 0;
long long aatpeDebugTimestatsParsePasses() { return s_timestatsParsePasses; }
void aatpeDebugTimestatsResetParseCounter() { s_timestatsParsePasses = 0; }

struct TimestatsBlockInfo {
    std::string pkg;
    TimestatsResult res;
};

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
            flushBlock();
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
    flushBlock();
    return out;
}

static TimestatsResult pickBestTimestatsCandidate(const std::vector<TimestatsResult>& candidates) {
    TimestatsResult res;
    if (candidates.empty()) return res;
    size_t best = 0;
    std::vector<size_t> valid;
    for (size_t i = 0; i < candidates.size(); i++) {
        double tf = candidates[i].targetFps;
        bool snaps = (tf >= 25.0 && tf <= (double)refresh::kMaxRate + 1e-6);
        if (snaps) valid.push_back(i);
    }
    if (valid.empty()) valid.push_back(0);
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

bool isLayerNameSafe(const std::string& s) {
    if (s.empty() || s.size() > 512) return false;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) { ++i; continue; }
        int need = 0; unsigned int minv = 0;
        if      (c <= 0xBF) return false;
        else if (c < 0xE0) { need = 1; minv = 0x80;  }
        else if (c < 0xF0) { need = 2; minv = 0x800; }
        else if (c < 0xF5) { need = 3; minv = 0x10000; }
        else return false;
        if (i + need >= s.size()) return false;
        unsigned long cp = c & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
        for (int k = 1; k <= need; ++k) {
            unsigned char d = (unsigned char)s[i + k];
            if (d < 0x80 || d > 0xBF) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if (cp < minv) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;
        i += need + 1;
    }
    return true;
}

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
        empty.reason = lastReason;
        return empty;
    }
    if (!enabled) {
        forkCalls++;
        fetch("dumpsys SurfaceFlinger --timestats -clear -enable 2>/dev/null");
        enabled = true;
        enabledAtMs = nowMs;
        return empty;
    }
    if (nowMs - enabledAtMs < 1000) return empty;
    forkCalls++;
    std::string dump = fetch("dumpsys SurfaceFlinger --timestats -dump 2>/dev/null");
    static constexpr size_t kMaxTimestatsDumpSize = size_t(1024) * 1024;
    if (dump.size() > kMaxTimestatsDumpSize) {
        dump.resize(kMaxTimestatsDumpSize);
    }
    lastDumpAtMs = nowMs;
    lastDumpContent = dump;
    TimestatsResult r = parseTimestatsBlock(dump, fgPkg);
    if (!r.available) {
        std::string reason = classifyTimestatsReason(dump, fgPkg);
        r.reason = reason;
        lastReason = reason;
        unavailableUntilMs = nowMs + timestatsBlindTtlMs(reason);
    } else {
        lastReason.clear();
        if (r.layer == snapLayer && snapTotalFrames >= 0 &&
            r.totalFrames >= snapTotalFrames) {
            long long deltaFrames = (long long)r.totalFrames - snapTotalFrames;
            long long deltaWallMs = nowMs - snapDumpMs;
            if (deltaFrames > 0 && deltaWallMs >= 200) {
                double instFps = (double)deltaFrames / ((double)deltaWallMs / 1000.0);
                if (instFps > 0.0 && instFps <= 300.0) {
                    r.avgFps = instFps;
                    r.activeFrames = (int)deltaFrames;
                }
            } else if (deltaFrames == 0) {
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

}
}

namespace {
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
    if (!force && mt == g_ovMtime) return;
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
}

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
std::vector<std::string> manualGameListSnapshot() {
    std::lock_guard<std::mutex> lk(g_ovMutex);
    gameOverridesReloadLocked(false);
    return std::vector<std::string>(g_ovCache.begin(), g_ovCache.end());
}
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
    if (g_ovCache.count(pkg) > 0) return true;
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
    if (g_ovCache.erase(pkg) == 0) return true;
    std::string path = gameOverridesPath();
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    for (auto it = g_ovCache.begin(); it != g_ovCache.end(); ++it)
        fprintf(f, "%s\n", it->c_str());
    fclose(f);
    struct stat st; if (stat(path.c_str(), &st) == 0) g_ovMtime = st.st_mtime;
    return true;
}
}

namespace apps {
static std::vector<std::string> scanDataAppPackages() {
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::vector<std::string> roots = {"/data/app"};
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
    static std::mutex sInstalledPkgsMtx;
    static std::vector<std::string> s_cache;
    static time_t s_cacheT = 0;
    time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> lk(sInstalledPkgsMtx);
        if (!s_cache.empty() && (now - s_cacheT) < 30) return s_cache;
    }

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
    {
        std::vector<std::string> all = aatpe::apklabel::listInstalledPackagesAllUsers();
        for (const auto& p : all) {
            if (std::find(fresh.begin(), fresh.end(), p) == fresh.end())
                fresh.push_back(p);
        }
    }
    if (fresh.empty()) {
        fresh = scanDataAppPackages();
    }

    std::lock_guard<std::mutex> lk(sInstalledPkgsMtx);
    if (!fresh.empty()) {
        s_cache = fresh;
        s_cacheT = now;
        return fresh;
    }
    return s_cache;
}
}
}

struct PerAppWindow {
    double fps;
    double p99Fps;
    long   samples;
    long   dropped;
    bool   jankArmed;
    long long tsMs;
};
static constexpr size_t kMaxWindowsPerApp = 15;
static constexpr size_t kMaxFrameSamplesPerApp = 600;
static constexpr long long kActiveGapMaxMs = 4000;
constexpr size_t kMaxPerAppStats = 24;

extern std::mutex g_perAppMutex;
static std::unordered_map<std::string, std::deque<PerAppWindow>> g_perAppWindows;
static std::unordered_map<std::string, std::deque<double>> g_perAppFrameTimes;
struct PerAppFsMetrics {
    double fps = 0.0;
    double targetFps = 0.0;
    double jankyPct = 0.0;
    long missedVsyncs = 0;
    long totalFrames = 0;
    long long updatedMs = 0;
};
static std::unordered_map<std::string, PerAppFsMetrics> g_perAppFsMetrics;
std::list<std::string> g_perAppLru;

struct TimestatsLayerSnap {
    long long totalFrames = 0;
    std::unordered_map<double, long long> cumHist;
    long long lastDumpMs = 0;
    bool has = false;
};
static std::unordered_map<std::string, TimestatsLayerSnap> g_tsLayerSnap;

static long long frameNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
void aatpeDebugFsMetricsInject(const std::string& p, double fps, double jankyPct,
                               long totalFrames, long missedVsyncs) {
    std::lock_guard<std::mutex> lk(g_perAppMutex);
    auto& m = g_perAppFsMetrics[p];
    m.fps = fps; m.targetFps = fps; m.jankyPct = jankyPct;
    m.totalFrames = totalFrames; m.missedVsyncs = missedVsyncs;
    m.updatedMs = frameNowMs();
}

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

static std::mutex g_frameAuthMutex;
static std::string g_frameAuthFg = "unknown";
static std::string g_frameAuthSource = "none";
static std::string g_frameAuthReason = "";

void frameSetAuthSnapshot(const std::string& fg, const std::string& source, const std::string& reason) {
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
    cursor = i;
    return out;
}

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

static void aatpeLabelWarmupThreadFn() {
    nice(15);
    while (!g_shouldExit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        if (g_shouldExit.load(std::memory_order_relaxed)) break;
        if (g_thermalEmergency) continue;
        (void)aatpeLabelWarmupTick(2);
    }
}

static void aatpeStartLabelWarmupThread() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        std::thread(aatpeLabelWarmupThreadFn).detach();
    });
}

struct VmContainerInfo;

static void processManualGameLayers(const std::string& dump,
                                     const std::string& fgPkg,
                                     const std::string& fgVmInner,
                                     long long contentTsMs);

bool aatpe::frameaware::sfLatencySamplesValid(const std::vector<long long>& us) {
    if (us.empty()) return false;
    long long sum = 0;
    for (long long v : us) sum += v;
    long long avg = sum / (long long)us.size();
    if (avg <= 0) return false;
    for (long long v : us) {
        double dev = std::abs((double)v - (double)avg) / (double)avg;
        if (dev > 0.25) return true;
    }
    return false;
}

long long aatpe::frameaware::frameBinderBackoffMs(int retryCount) {
    if (retryCount <= 0) return 5000;
    long long ms = 5000LL << retryCount;
    if (ms > 30000) ms = 30000;
    return ms;
}

struct VmContainerInfo {
    std::string containerPkg;
    std::string innerGamePkg;
    std::string innerPids;
    bool valid = false;
};

static std::vector<std::string> g_frameVmContainerPrefixes;
static std::mutex g_frameVmContainerMtx;
static bool g_frameVmContainerListLoaded = false;

static void loadFrameVmContainerPrefixesLocked() {
    g_frameVmContainerPrefixes.clear();
    std::string path = std::string(STATE_DIR) + "/vm_containers.list";
    std::ifstream f(path.c_str());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t b = line.find_first_not_of(" \t\r");
        size_t e = line.find_last_not_of(" \t\r");
        if (b == std::string::npos) continue;
        line = line.substr(b, e - b + 1);
        if (line.empty() || line[0] == '#') continue;
        g_frameVmContainerPrefixes.push_back(line);
    }
}

#ifdef AATPE_HOST_TEST
void setFrameVmContainerPrefixesForTest(const std::vector<std::string>& pkgs) {
    std::lock_guard<std::mutex> lk(g_frameVmContainerMtx);
    g_frameVmContainerPrefixes = pkgs;
    g_frameVmContainerListLoaded = true;
}
#endif

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
        if (pkg.rfind(p + ".", 0) == 0) return true;
    }
    return false;
}

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

static VmContainerInfo detectVmContainerGame(const std::string& fgPkg);

static void processManualGameLayers(const std::string& dump,
                                     const std::string& fgPkg,
                                     const std::string& fgVmInner,
                                     long long contentTsMs) {
    if (dump.empty()) return;
    std::string manualList = gamelist::currentManualGameList();
    std::istringstream iss(manualList);
    std::string pkg;

    std::map<std::string, std::vector<aatpe::frameaware::TimestatsResult>> grouped;
    for (const auto& bi : aatpe::frameaware::parseTimestatsAllBlocks(dump)) {
        grouped[bi.pkg].push_back(bi.res);
    }
    (void)0;

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
        if (attrPkg == fgPkg) continue;

        auto git = grouped.find(matchPkg);
        if (git == grouped.end() || git->second.empty()) continue;

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

        sn.totalFrames = best->totalFrames;
        sn.lastDumpMs = contentTsMs;
        sn.cumHist.clear();
        for (auto& b : best->hist) sn.cumHist[b.first] = b.second;

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

        auto wit = g_perAppWindows.find(attrPkg);
        if (wit == g_perAppWindows.end()) {
            if (g_perAppWindows.size() >= kMaxPerAppStats) {
                std::string old = g_perAppLru.back(); g_perAppLru.pop_back();
                g_perAppWindows.erase(old);
                g_perAppFrameTimes.erase(old);
                g_perAppFsMetrics.erase(old);
                evictTsLayerSnapForPkgLocked(old);
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
        w.tsMs = contentTsMs;
        ring.push_back(w);
        while (ring.size() > kMaxWindowsPerApp) ring.pop_front();
    }
}

#ifdef AATPE_HOST_TEST
void aatpeDebugProcessManualLayersForTest(const std::string& dump,
                                          const std::string& fgPkg,
                                          long long contentTsMs) {
    processManualGameLayers(dump, fgPkg, "", contentTsMs);
}
#endif

bool aatpe::frameaware::isTransientFocus(const std::string& prevFg, const std::string& newFg) {
    if (newFg.empty() || newFg == "unknown") return true;
    static const char* transientPkgs[] = {
        "com.android.systemui",
        "com.android.launcher",
        "com.miui.home",
        "com.miui.securitycenter",
        "com.omarea.vtools",
        "com.android.inputmethod",
        "org.lsposed.lspatch",
        "com.dergoogler.mmrl",
        nullptr
    };
    for (int i = 0; transientPkgs[i]; i++) {
        if (newFg == transientPkgs[i]) return true;
    }
    if (frameSameVmContainerGroup(prevFg, newFg)) {
        return true;
    }
    return false;
}

static long getPkgUidImpl(const std::string& pkg, const char* path) {
    char line[512];
    if (pkg.size() >= sizeof(line)) return -1;
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    long uid = -1;
    while (fgets(line, sizeof(line), f)) {
        size_t pkgLen = pkg.size();
        if (strncmp(line, pkg.c_str(), pkgLen) == 0 && line[pkgLen] == ' ') {
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
extern "C" long aatpeTestGetPkgUid(const char* pkg, const char* pkglistPath) {
    if (!pkg || !*pkg) return -1;
    const char* path = (pkglistPath && *pkglistPath) ? pkglistPath : "/data/system/packages.list";
    return getPkgUidImpl(std::string(pkg), path);
}
#endif

struct VmCacheEntry {
    VmContainerInfo info;
    long long checkedMs = 0;
};
static std::mutex s_vmDetectMtx;
static std::unordered_map<std::string, VmCacheEntry> s_vmDetectCache;
static constexpr long long kVmCacheTtlMs = 3000;

static VmContainerInfo detectVmContainerGame(const std::string& fgPkg) {
    VmContainerInfo info;
    if (fgPkg.empty() || fgPkg == "unknown") return info;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> lk(s_vmDetectMtx);
        auto cit = s_vmDetectCache.find(fgPkg);
        if (cit != s_vmDetectCache.end() && (nowMs - cit->second.checkedMs) < kVmCacheTtlMs) {
            return cit->second.info;
        }
    }
    bool isContainer = aatpeFrameIsVmContainerPkg(fgPkg);
    if (!isContainer) return info;

    long containerUid = getPkgUid(fgPkg);
    if (containerUid <= 0) return info;

    std::string overrides = gamelist::currentManualGameList();
    if (overrides.empty()) return info;
    std::istringstream iss(overrides);
    std::string innerPkg;
    std::vector<std::string> runningNoLayerCandidates;
    while (std::getline(iss, innerPkg)) {
        if (innerPkg.empty() || innerPkg == fgPkg) continue;
        std::string pidOut = aatpeExecArgv({"pidof", innerPkg}, "VM_PIDOF");
        if (pidOut.empty()) continue;
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
            writeFile((std::string(STATE_DIR) + "/vm_inner_detect_reason").c_str(), "uid_match");
            return info;
        }
        runningNoLayerCandidates.push_back(innerPkg);
    }
    if (runningNoLayerCandidates.size() == 1) {
        info.containerPkg = fgPkg;
        info.innerGamePkg = runningNoLayerCandidates[0];
        info.valid = true;
        writeFile((std::string(STATE_DIR) + "/vm_inner_detect_reason").c_str(), "single_candidate_heuristic");
    }
    if (!info.valid) {
        writeFile((std::string(STATE_DIR) + "/vm_inner_detect_reason").c_str(),
                  runningNoLayerCandidates.empty() ? "no_candidates" : "ambiguous_multi");
    }
    {
        std::lock_guard<std::mutex> lk(s_vmDetectMtx);
        s_vmDetectCache[fgPkg] = {info, nowMs};
    }
    return info;
}

static constexpr int kFramestatsIntervalSec = 5;
static time_t g_framestatsLastRun = 0;

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

static int findHeaderCol(const std::vector<std::string>& header, const char* name) {
    for (size_t i = 0; i < header.size(); i++) {
        if (header[i] == name) return (int)i;
    }
    return -1;
}

static int headerCompletedCol(const std::vector<std::string>& hdr) {
    int c = findHeaderCol(hdr, "FrameCompleted");
    if ((int)hdr.size() >= 14 && c >= 14) return c;
    return -1;
}

static int findLastPositiveIntCol(const std::vector<std::string>& cols) {
    for (int i = (int)cols.size() - 1; i >= 0; i--) {
        if (cols[i].empty()) continue;
        char* endp = nullptr;
        long long v = strtoll(cols[i].c_str(), &endp, 10);
        if (endp != cols[i].c_str() && v > 0) return i;
    }
    return -1;
}

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
            completedCol = headerCompletedCol(hdr);
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
        long long vsyncNs = strtoll(cols[3].c_str(), &endp, 10);
        long long completedNs = strtoll(completedStr.c_str(), &endp, 10);
        if (completedNs <= 0 || vsyncNs <= 0) continue;
        if (flags & 0x20000000LL) continue;
        long long durNs = completedNs - vsyncNs;
        if (durNs < 0 || durNs > 50000000LL) continue;
        durations.push_back((double)durNs / 1000000.0);
    }
    return durations;
}

struct FramestatsMetrics {
    double fps;
    double targetPeriodMs;
    double droppedPct;
    double jankyPct;
    long totalFrames;
    long missedVsyncs;
    long jankyFrames;
    double targetFps;
};

static double medianOfSorted(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 1) return v[n/2];
    return (v[n/2 - 1] + v[n/2]) / 2.0;
}

static constexpr double kLateEpsilonMs = 0.5;

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
            completedCol = headerCompletedCol(hdr);
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
        long long intendedVs = strtoll(cols[2].c_str(), &endp, 10);
        long long completedNs = strtoll(completedStr.c_str(), &endp, 10);
        if (completedNs <= 0 || intendedVs <= 0) continue;
        if (flags & 0x20000000LL) continue;
        intendedVsyncNs.push_back(intendedVs);
        frameCompletedNs.push_back(completedNs);
        flagsList.push_back(flags);
    }
    int n = (int)intendedVsyncNs.size();
    if (n < 5) return m;
    m.totalFrames = n;

    std::vector<double> gaps;
    for (int i = 1; i < n; i++) {
        double gapMs = (double)(intendedVsyncNs[i] - intendedVsyncNs[i-1]) / 1000000.0;
        if (gapMs > 1.0 && gapMs < 100.0) gaps.push_back(gapMs);
    }
    m.targetPeriodMs = medianOfSorted(gaps);
    if (m.targetPeriodMs < 4.0 || m.targetPeriodMs > 50.0) m.targetPeriodMs = 16.67;
    m.targetFps = 1000.0 / m.targetPeriodMs;

    double spanMs = (double)(intendedVsyncNs[n-1] - intendedVsyncNs[0]) / 1000000.0;
    if (spanMs > 0) m.fps = (double)(n - 1) * 1000.0 / spanMs;

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

struct GfxinfoSummary {
    bool valid = false;
    long totalFrames = 0;
    double jankyPct = 0.0;
    double fps = 0.0;
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
            statsSinceNs = (long long)aatpeFirstFloat(rhs);
        } else if (t.rfind("Total frames rendered:", 0) == 0) {
            totalFrames = (long)aatpeFirstInt(rhs);
        } else if (t.rfind("Janky frames:", 0) == 0) {
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
    if (totalFrames < 5 || statsSinceNs <= 0 || nowUptimeNs <= statsSinceNs) return g;
    double elapsedSec = (double)(nowUptimeNs - statsSinceNs) / 1e9;
    if (elapsedSec <= 0.0) return g;
    g.fps = (double)totalFrames / elapsedSec;
    g.valid = true;
    return g;
}

bool aatpeGfxinfoSummaryUsable(const GfxinfoSummary& g) {
    return g.valid && g.totalFrames >= 5 && g.fps > 1.0;
}

void aatpeSetFrameJank(const char* source, int totalSamples, double missRate,
                      double avgFps, double p90Fps, double p99Fps) {
    std::string src = source ? source : "none";
    writeFile((std::string(STATE_DIR) + "/frame_source").c_str(), src.c_str());
    if (src == "none" || totalSamples < 5) {
        aatpe::frameaware::g_frameMissJankInput.store(false, std::memory_order_relaxed);
        return;
    }
    if (missRate < 0.0) missRate = 0.0;
    if (missRate > 1.0) missRate = 1.0;
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
    bool missJank = (missRate >= 0.05);
    aatpe::frameaware::g_frameMissJankInput.store(missJank, std::memory_order_relaxed);

    {
        std::string pkg = aatpeGetForegroundPackage();
        if (pkg.empty() || pkg == "unknown") return;
        long long nowMs = frameNowMs();
        std::lock_guard<std::mutex> lk(g_perAppMutex);
        auto it = g_perAppWindows.find(pkg);
        if (it == g_perAppWindows.end()) {
            if (g_perAppWindows.size() >= kMaxPerAppStats) {
                std::string old = g_perAppLru.back(); g_perAppLru.pop_back();
                g_perAppWindows.erase(old);
                g_perAppFrameTimes.erase(old);
                g_perAppFsMetrics.erase(old);
                evictTsLayerSnapForPkgLocked(old);
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
        w.jankArmed = missJank;
        w.tsMs = nowMs;
        ring.push_back(w);
        while (ring.size() > kMaxWindowsPerApp) ring.pop_front();
    }
}

void aatpeFrameClearMissJankInput() {
    aatpe::frameaware::g_frameMissJankInput.store(false, std::memory_order_relaxed);
}

void aatpeFrameClearAppFsJank() {
    aatpe::frameaware::g_appFsJankActive.store(false, std::memory_order_relaxed);
}

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
    std::string src, reason;
    frameGetAuthSnapshot(currentPkg, src, reason);
    std::string authoritativeFg = aatpeGetForegroundPackage();
    if (!authoritativeFg.empty() && authoritativeFg != "unknown") {
        currentPkg = authoritativeFg;
    }
    long long nowMs = frameNowMs();
    long long nowSysMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    long long nowSteadyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long bootEpochMs = nowSysMs - nowSteadyMs;
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
        if (stale && !onManual) continue;
        AatpePerAppFrameStat st;
        st.pkg = pkg;
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
        auto ftIt = g_perAppFrameTimes.find(pkg);
        if (ftIt != g_perAppFrameTimes.end() && !ftIt->second.empty()) {
            std::vector<double> filtered;
            for (double d : ftIt->second) {
                if (d <= 250.0) filtered.push_back(d);
            }
            if (filtered.empty()) filtered.assign(ftIt->second.begin(), ftIt->second.end());
            std::sort(filtered.begin(), filtered.end());
            size_t n = filtered.size();
            st.ft_p50 = filtered[(size_t)(n * 0.50)];
            st.ft_p95 = filtered[(size_t)(n * 0.95)];
            double fsP99 = filtered[(size_t)(n * 0.99)];
            st.ft_p99 = (fsP99 > 0.5) ? fsP99 : st.ft_p99;
        } else {
            st.ft_p50 = 0.0;
            st.ft_p95 = 0.0;
        }
        auto fsIt = g_perAppFsMetrics.find(pkg);
        const PerAppFsMetrics* fmPtr =
            (fsIt != g_perAppFsMetrics.end() && fsIt->second.totalFrames >= 5)
                ? &fsIt->second : nullptr;
        if (fmPtr) {
            const auto& fm = *fmPtr;
            st.fps = fm.fps;
            st.target_fps = fm.targetFps;
            long jankyFrames = (long)(fm.jankyPct * fm.totalFrames);
            if (jankyFrames < 0) jankyFrames = 0;
            if (jankyFrames > (long)fm.totalFrames) jankyFrames = (long)fm.totalFrames;
            st.jank = jankyFrames;
            st.dropped = jankyFrames;
            st.missed_vsyncs = fm.missedVsyncs;
            st.total_frames = fm.totalFrames;
            st.frameSource = "timestats";
        }
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
        if (ageMs > 5000) {
            st.fps = 0.0;
            st.ft_avg_ms = 0.0;
            st.reason = "background_stale";
        }
        if (st.reason.empty()) st.reason = stale ? "background" : "";
        st.isCurrent = (pkg == currentPkg);
        st.lastSeenMs = ring.back().tsMs;
        st.lastSeenEpochMs = bootEpochMs + ring.back().tsMs;
        emitted.insert(pkg);
        out.push_back(std::move(st));
    }
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
        st.lastSeenEpochMs = -1;
        out.push_back(std::move(st));
    }
    return (int)out.size();
}

void readSurfaceFlinger() {
    Config sfCfg;
    snapshotConfig(sfCfg);
    aatpeStartLabelWarmupThread();
    if (!sfCfg.frameAware) return;
    int rate = aatpeDetectRefreshRate();
    writeFile((std::string(STATE_DIR) + "/vsync_rate").c_str(), rate);
    int maxLat = 0;
    bool sfLatValid = false;
    int avgLat = 0;
    static int s_cachedAvgLat = 0;
    static int s_cachedMaxLat = 0;
    static bool s_cachedSfLatValid = false;
    static auto s_lastSfLatCheck = std::chrono::steady_clock::now();
    auto nowSf = std::chrono::steady_clock::now();
    int sfLatCacheMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowSf - s_lastSfLatCheck).count();
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
    writeFile((std::string(STATE_DIR) + "/sf_latency_valid").c_str(), sfLatValid ? 1 : 0);
    int layers = 0;
    {
        static int s_cachedLayers = 0;
        static time_t s_layerCacheTime = 0;
        time_t now2 = time(nullptr);
        {
            std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
            if (s_layerCacheTime > 0 && (now2 - s_layerCacheTime) < 2) {
                layers = s_cachedLayers;
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
            {
                std::lock_guard<std::recursive_mutex> cacheLock(g_frameCacheMutex);
                s_cachedLayers = layers;
                s_layerCacheTime = now2;
            }
        }
    }
    writeFile((std::string(STATE_DIR) + "/layer_count").c_str(), layers);
    int targetFrameTime;
    if (sfCfg.targetFrameTime > 0) {
        targetFrameTime = sfCfg.targetFrameTime;
    } else if (rate >= refresh::kMinRate && rate <= refresh::kMaxRate) {
        targetFrameTime = (refresh::kOneSecondUs + rate / 2) / rate;
    } else {
        targetFrameTime = 16667;
    }
    if (g_currentAppConfig.valid && g_currentAppConfig.maxFpsHint > 0) {
        int appTargetUs = 1000000 / g_currentAppConfig.maxFpsHint;
        if (appTargetUs > 0) targetFrameTime = appTargetUs;
    }
    int actualFrameTime = (rate >= refresh::kMinRate && rate <= refresh::kMaxRate)
        ? (refresh::kOneSecondUs + rate / 2) / rate : targetFrameTime;
    int frameTimeUs = (rate >= refresh::kMinRate && rate <= refresh::kMaxRate) ? actualFrameTime : targetFrameTime;
    int dynamicThreshold = (int)(frameTimeUs * refresh::kFrameCriticalMultiplier);
    int effectiveThreshold = (dynamicThreshold > sfCfg.frameCriticalThreshold) ? dynamicThreshold : sfCfg.frameCriticalThreshold;
    std::string fgPkg = aatpeGetForegroundPackage();
    if (fgPkg.empty() || fgPkg == "unknown") {
        std::string manualFg = gamelist::detectManualGameForeground();
        if (!manualFg.empty()) fgPkg = manualFg;
    }
    const bool fgIsGame = !fgPkg.empty() && fgPkg != "unknown" &&
                          (aatpe::gamelist::isManualGame(fgPkg) ||
                           isPackageInGameList(fgPkg));
    aatpe::frameaware::FrameLatencyAlarm fa = aatpe::frameaware::aatpeFrameLatencyAlarm(
        sfLatValid, avgLat, maxLat, effectiveThreshold, frameTimeUs,
        sfCfg.jankThresholdPercent, fgIsGame);
    bool latCritical = fa.latCritical;
    bool layerCritical = (layers > sfCfg.layerCriticalThreshold);
    bool frameCritical = latCritical || layerCritical;
    bool jankDetected = fa.jank;
    writeFile((std::string(STATE_DIR) + "/jank_detected").c_str(), jankDetected ? 1 : 0);

    static constexpr int kJankBoostReleaseConsecutive = 4;
    static int s_jankConsecutive = 0;
    static int s_jankClearStreak = 0;
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
        g_safetyLatencyActive.store(false, std::memory_order_relaxed);
        writeFile((std::string(STATE_DIR) + "/safety_latency_active").c_str(), 0);
    }
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
    writeFile((std::string(STATE_DIR) + "/target_frame_time").c_str(), frameTimeUs);

    static aatpe::frameaware::TimestatsSession s_tsSession;
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
        aatpeSetFrameJank("none", 0, 0.0, 0.0, 0.0, 0.0);
        aatpeFrameClearAppFsJank();
        std::string curFg = aatpeGetForegroundPackage();
        frameSetAuthSnapshot(curFg.empty() ? "unknown" : curFg, "none", "screen_off");
        writeFile((std::string(STATE_DIR) + "/frame_unavailable").c_str(), 1);
        return;
    }

    if (!sfLatValid || fgIsGame) {
        static std::string s_lastFgPkg;
        static std::string s_cachedListOut;
        static time_t s_listCacheTime = 0;
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
        if (s_lastFgPkg != fgPkg) {
            s_lastFgPkg = fgPkg;
            s_cachedListOut.clear();
            s_listCacheTime = 0;
            s_tsSession.unavailableUntilMs = 0;
            s_tsSession.enabledAtMs = nowMs;
        }
        auto tsFetch = [](const std::string& c) { return aatpeExec(c.c_str(), "SF_TIMESTATS"); };
        VmContainerInfo vmInfo = detectVmContainerGame(fgPkg);
        std::string sfMatchPkg = fgPkg;
        std::string statAttrPkg = fgPkg;
        if (vmInfo.valid) {
            sfMatchPkg = vmInfo.containerPkg;
            statAttrPkg = vmInfo.innerGamePkg;
            writeFile((std::string(STATE_DIR) + "/vm_container_pkg").c_str(), vmInfo.containerPkg.c_str());
            writeFile((std::string(STATE_DIR) + "/vm_inner_game").c_str(), vmInfo.innerGamePkg.c_str());
            writeFile((std::string(STATE_DIR) + "/vm_inner_pids").c_str(), vmInfo.innerPids.c_str());
        }
        aatpe::frameaware::TimestatsResult tsr = s_tsSession.tick(nowMs, sfMatchPkg, tsFetch);
        std::string tsrReason;
        {
            tsrReason = !tsr.reason.empty() ? tsr.reason : s_tsSession.lastReason;
            writeFile((std::string(STATE_DIR) + "/frame_timestats_reason").c_str(),
                      tsrReason.c_str());
        }

        processManualGameLayers(s_tsSession.lastDumpContent, statAttrPkg,
                                 vmInfo.innerGamePkg, s_tsSession.lastDumpAtMs);

        time_t nowList = time(nullptr);
        if (s_listCacheTime == 0 || (nowList - s_listCacheTime) >= 2) {
            s_cachedListOut = aatpeExec("dumpsys SurfaceFlinger --list 2>/dev/null", "SF_GAME_LIST");
            s_listCacheTime = nowList;
        }

        const bool forceGame = aatpe::gamelist::isManualGame(fgPkg);

        aatpe::frameaware::SfFrameSample smp;
        bool binderOk = false;
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
                smp.frameUnavailable = true;
            }
        }
        if (!binderOk) {
            auto fetchCb = [](const std::string& layer) -> std::string {
                if (!aatpe::frameaware::isLayerNameSafe(layer)) return "";
                return aatpeExecArgv({"dumpsys", "SurfaceFlinger", "--latency", layer},
                                     "SF_GAME_LAT");
            };
            smp = aatpe::frameaware::sampleGameLayer(s_cachedListOut, fgPkg, fetchCb, forceGame);
        }

        writeFile((std::string(STATE_DIR) + "/frame_debug_candidates").c_str(),
                  (smp.candidates.empty() ? std::string("none") : smp.candidates).c_str());

        writeFile((std::string(STATE_DIR) + "/frame_unavailable").c_str(),
                  smp.frameUnavailable ? 1 : 0);
        writeFile((std::string(STATE_DIR) + "/game_layer").c_str(),
                  smp.chosenLayer.empty() ? "none" : smp.chosenLayer.c_str());
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
            int gedFps = aatpeReadGedComposerFps();
            aatpe::frameaware::GameFpsResolve gr =
                aatpe::frameaware::resolveGameFps(smp.renderFps, (double)smp.hz,
                                                  gedFps, srcName);
            if (gr.degraded) {
                frameSetAuthSnapshot(statAttrPkg, "degraded", "insufficient_present_frames");
                aatpeFrameClearMissJankInput();
            } else {
                frameSetAuthSnapshot(statAttrPkg, gr.source, "");
                writeFile((std::string(STATE_DIR) + "/game_active").c_str(), 1);
                double gameFps = gr.fps;
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
            std::string r = smp.reason;
            if (r == "no_layer_for_pkg") r = "no_game_layer";
            if (r.empty()) r = "no_valid_frames";

            bool reportedGfx = false;
            bool gfxPkgOk = !fgPkg.empty() && fgPkg != "unknown" && aatpeIsValidPackageName(fgPkg);
            static time_t s_gfxSumLastRun = 0;
            static GfxinfoSummary s_lastGoodGfx;
            static time_t s_lastGoodGfxAt = 0;
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
                if (aatpeGfxinfoSummaryUsable(s_lastGoodGfx) &&
                    s_lastGoodGfxPkg == fgPkg &&
                    (nowGs - s_lastGoodGfxAt) < 2.0 * kFramestatsIntervalSec) {
                    double p90fps = s_lastGoodGfx.p90ms > 0.0 ? 1000.0 / s_lastGoodGfx.p90ms : 0.0;
                    double p99fps = s_lastGoodGfx.p99ms > 0.0 ? 1000.0 / s_lastGoodGfx.p99ms : 0.0;
                    double missRate = s_lastGoodGfx.jankyPct;
                    if (rate >= refresh::kMinRate && rate <= refresh::kMaxRate &&
                        s_lastGoodGfx.fps > 1.0) {
                        double lowFpsRatio = s_lastGoodGfx.fps / (double)rate;
                        if (lowFpsRatio < 0.55) {
                            double lowFpsJank = 1.0 - lowFpsRatio / 0.55;
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
                frameSetAuthSnapshot(fgPkg, "none", r);
            }
        }
    } else {
        writeFile((std::string(STATE_DIR) + "/frame_unavailable").c_str(), 0);
        writeFile((std::string(STATE_DIR) + "/game_active").c_str(), 0);
        std::string gfxFg = aatpeGetForegroundPackage();
        frameSetAuthSnapshot(gfxFg, "non_game", "foreground_is_not_a_game");
        aatpeFrameClearMissJankInput();
        aatpeFrameClearGameStats();
    }

    {
        std::string fgPkg = aatpeGetForegroundPackage();
        bool pkgOk = !fgPkg.empty() && fgPkg != "unknown" && aatpeIsValidPackageName(fgPkg);
        if (pkgOk && gamelist::isManualGame(fgPkg)) {
            VmContainerInfo vmFg = detectVmContainerGame(aatpeGetForegroundPackage());
            if (vmFg.valid && vmFg.innerGamePkg == fgPkg) {
                pkgOk = false;
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
                    FramestatsMetrics fm = computeFramestatsMetrics(fsOut);
                    if (fm.totalFrames >= 5) {
                        auto& m = g_perAppFsMetrics[fgPkg];
                        m.fps = fm.fps;
                        m.targetFps = fm.targetFps;
                        m.jankyPct = fm.jankyPct;
                        m.missedVsyncs = fm.missedVsyncs;
                        m.totalFrames = fm.totalFrames;
                        m.updatedMs = frameNowMs();
                        bool lateJank = (fm.jankyPct >= 0.10);
                        double lowFpsRatio = (rate >= refresh::kMinRate && rate <= refresh::kMaxRate && fm.fps > 1.0)
                                             ? fm.fps / (double)rate : 1.0;
                        bool lowFps = (lowFpsRatio > 0.0 && lowFpsRatio < 0.55);
                        aatpe::frameaware::g_appFsJankActive.store(lateJank || lowFps, std::memory_order_relaxed);
                    } else {
                        aatpeFrameClearAppFsJank();
                    }
                    auto winIt = g_perAppWindows.find(fgPkg);
                    if (winIt == g_perAppWindows.end()) {
                        if (g_perAppWindows.size() >= kMaxPerAppStats) {
                            std::string old = g_perAppLru.back(); g_perAppLru.pop_back();
                            g_perAppWindows.erase(old);
                            g_perAppFrameTimes.erase(old);
                            g_perAppFsMetrics.erase(old);
                            evictTsLayerSnapForPkgLocked(old);
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
                    aatpeFrameClearAppFsJank();
                }
            }
        }
    }

}

std::string g_foregroundApp = "unknown";

std::string aatpeGetForegroundPackage() {
    std::lock_guard<std::mutex> lk(g_foregroundMutex);
    return g_foregroundApp;
}

std::string aatpe::frameaware::FgDebouncer::onRawFg(const std::string& raw, long long nowMs) {
        if (committed.empty() || committed == "unknown") {
            if (!raw.empty() && raw != "unknown") {
                committed = raw;
                pending.clear();
            }
            return committed;
        }
        if (raw == committed) {
            pending.clear();
            return committed;
        }
        if (raw.empty() || raw == "unknown") {
            return committed;
        }
        bool rawIsManualGame = gamelist::isManualGame(raw);
        if (!rawIsManualGame && isTransientFocus(committed, raw)) {
            return committed;
        }
        if (rawIsManualGame) {
            committed = raw;
            pending.clear();
            return committed;
        }
        if (pending != raw) {
            pending = raw;
            pendingSinceMs = nowMs;
            return committed;
        }
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

static constexpr long long kFgCgroupTtlMs    [[maybe_unused]] = 200;
static constexpr long long kFgDumpsysMinGapMs [[maybe_unused]] = 1000;
static std::atomic<long long> g_fgCgroupReadMs{0};
static std::atomic<bool>      g_fgCgroupDirty{true};
static std::atomic<long long> g_fgDumpsysLastMs[[maybe_unused]]{0};
static constexpr long long kFgDumpsysTtlMs = 2000;
static std::atomic<long long> g_fgDumpsysReadMs{0};
static std::string g_fgDumpsysResult;
static std::atomic<int> g_fgInotifyFd{-1};
static std::mutex g_fgInotifyInitMtx;

static inline long long fgNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void fgInotifyInit() {
    int fd = g_fgInotifyFd.load(std::memory_order_acquire);
    if (fd >= 0 || fd == -2) return;

    std::lock_guard<std::mutex> lk(g_fgInotifyInitMtx);
    fd = g_fgInotifyFd.load(std::memory_order_acquire);
    if (fd >= 0 || fd == -2) return;

    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        g_fgInotifyFd.store(-2, std::memory_order_release);
        return;
    }
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
        inotify_add_watch(fd, watchPaths[i], mask);
    }
    g_fgInotifyFd.store(fd, std::memory_order_release);
}

static void fgInotifyDrain() {
    int fd = g_fgInotifyFd.load(std::memory_order_acquire);
    if (fd < 0) {
        fgInotifyInit();
        fd = g_fgInotifyFd.load(std::memory_order_acquire);
        if (fd < 0) return;
    }
    char buf[256];
    bool sawEvent = false;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) { sawEvent = true; continue; }
        break;
    }
    if (sawEvent) g_fgCgroupDirty = true;
}

static bool aatpeIsValidPackageName(const std::string& p) {
    static const size_t kMaxPkgLen = 256;
    if (p.empty() || p.size() > kMaxPkgLen) return false;
    if (!isalpha((unsigned char)p[0])) return false;
    if (p[0] == '.' || p[p.size() - 1] == '.') return false;
    if (p.find("..") != std::string::npos) return false;
    for (char ch : p) {
        if (isalnum((unsigned char)ch)) continue;
        if (ch == '.' || ch == '_') continue;
        return false;
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
    size_t slash = s.find('/');
    if (slash != std::string::npos) {
        size_t tokenStart = s.rfind(' ', slash);
        if (tokenStart == std::string::npos) tokenStart = 0;
        else tokenStart++;
        s = s.substr(tokenStart, slash - tokenStart);
    }
    trim(s);
    if (!aatpeIsValidPackageName(s)) {
        LOG_WARN("FG_APP: extracted package failed whitelist (len=%zu) -> dropping", s.size());
        return std::string();
    }
    return s;
}

static std::string aatpeExtractDumpsysValue(const std::string& o) {
    size_t eq = o.find('=');
    size_t colon = o.find(':');
    size_t pos;
    if (eq != std::string::npos) pos = eq;
    else if (colon != std::string::npos) pos = colon;
    else return "";
    return o.substr(pos + 1);
}

static std::vector<int> aatpeReadCpusetPids(const std::string& group) {
    std::vector<int> pids;
    const char* candidates[][2] = {
        {"/dev/cpuset/",            "/tasks"},
        {"/sys/fs/cgroup/cpuset/",  "/cgroup.procs"},
        {"/sys/fs/cgroup/cpuset/",  "/cgroup.threads"},
        {"/sys/fs/cgroup/cpuset/",  "/tasks"},
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
        if (!pids.empty()) break;
    }
    return pids;
}

std::string detectForegroundFromApk(time_t now) {
    if (g_helperApkInstalled) {
        static time_t s_apkHandledMtime = -1;
        static std::string s_apkHandledPkg;
        static std::string s_apkLastLoggedPkg;
        static long long   s_apkLastLogMs = 0;
        static const char* apkFgPaths[] = {
            "/dev/aatpe/state/foreground_app",
            "/dev/aatpe/foreground_app",
            nullptr
        };
        for (int pi = 0; apkFgPaths[pi] != nullptr; pi++) {
            const char* apkFgPath = apkFgPaths[pi];
            struct stat st;
            if (stat(apkFgPath, &st) == 0) {
                time_t fileAge = now - st.st_mtime;
                if (fileAge < 0) fileAge = 0;
                if (fileAge <= 30) {
                    if (st.st_mtime == s_apkHandledMtime && !s_apkHandledPkg.empty()) {
                        aatpeSetForegroundLocked(s_apkHandledPkg);
                        return s_apkHandledPkg;
                    }
                    std::string apkFg = readFile(apkFgPath);
                    trim(apkFg);
                    if (!apkFg.empty() && apkFg != "unknown") {
                        aatpeSetForegroundLocked(apkFg);
                        writeFile((std::string(STATE_DIR) + "/foreground_app_source").c_str(), "apk");
                        writeFile((std::string(STATE_DIR) + "/fg_apk_status").c_str(), "ok");
                        writeFile((std::string(STATE_DIR) + "/fg_apk_age").c_str(), (int)fileAge);
                        writeFile((std::string(STATE_DIR) + "/fg_apk_path").c_str(), apkFgPath);
                        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (apkFg != s_apkLastLoggedPkg || (nowMs - s_apkLastLogMs) >= 5000) {
                            FRAME_LOG_INFO("FG_APP: [APK] foreground=%s (age=%lds path=%s)", apkFg.c_str(), fileAge, apkFgPath);
                            s_apkLastLoggedPkg = apkFg;
                            s_apkLastLogMs = nowMs;
                        }
                        s_apkHandledMtime = st.st_mtime;
                        s_apkHandledPkg = apkFg;
                        return apkFg;
                    } else {
                        writeFile((std::string(STATE_DIR) + "/fg_apk_status").c_str(), "empty_or_unknown");
                        writeFile((std::string(STATE_DIR) + "/fg_apk_age").c_str(), (int)fileAge);
                        LOG_WARN("FG_APP: [APK] file=%s content empty/unknown (age=%lds)", apkFgPath, fileAge);
                    }
                } else {
                    writeFile((std::string(STATE_DIR) + "/fg_apk_status").c_str(), "stale");
                    writeFile((std::string(STATE_DIR) + "/fg_apk_age").c_str(), (int)fileAge);
                    writeFile((std::string(STATE_DIR) + "/fg_apk_path").c_str(), apkFgPath);
                    LOG_WARN("FG_APP: [APK] file=%s stale (age=%lds > 30s)", apkFgPath, fileAge);
                }
            }
        }
        writeFile((std::string(STATE_DIR) + "/fg_apk_status").c_str(), "missing");
        LOG_WARN("FG_APP: [APK] foreground_app file not found in any path, falling back to module detection");
    }
    return "";
}

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

static std::string aatpeCgroupConfidentPkg(const std::string& group) {
    std::vector<int> pids = aatpeReadCpusetPids(group);
    std::string candidate;
    int hits = 0;
    for (int pid : pids) {
        std::string cmdline = readFile(("/proc/" + std::to_string(pid) + "/cmdline").c_str());
        if (cmdline.empty()) continue;
        size_t nullPos = cmdline.find('\0');
        if (nullPos != std::string::npos) cmdline = cmdline.substr(0, nullPos);
        trim(cmdline);
        if (cmdline.empty()) continue;
        if (cmdline.find('/') != std::string::npos) continue;
        if (aatpeIsSystemDaemonName(cmdline)) continue;
        if (cmdline.find('.') == std::string::npos) continue;
        if (!aatpeIsValidPackageName(cmdline)) continue;
        if (hits == 0) candidate = cmdline;
        hits++;
        if (hits > 1) break;
    }
    return (hits == 1) ? candidate : std::string();
}

std::string detectForegroundFromSysfs() {
    std::string pkg = aatpeCgroupConfidentPkg("top-app");
    if (pkg.empty()) pkg = aatpeCgroupConfidentPkg("foreground");
    return pkg;
}

bool aatpeFgBandCandidate(int oom, int* outPreference) {
    static const int kPersistentAdjCeiling = -700;
    static const int kBackgroundAdjFloor   = 400;
    if (oom <= kPersistentAdjCeiling || oom >= kBackgroundAdjFloor) return false;
    if (outPreference) *outPreference = oom;
    return true;
}

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
        if (a.topApp != b.topApp) {
            if (a.topApp) best = i;
            continue;
        }
        int da = std::abs(a.oom), db = std::abs(b.oom);
        if (da != db) {
            if (da < db) best = i;
            continue;
        }
        if (a.pid != b.pid) {
            if (a.pid < b.pid) best = i;
        }
    }
    return best;
}

bool procInTopAppCgroup(int pid) {
    std::string cg = readFile(("/proc/" + std::to_string(pid) + "/cgroup").c_str());
    if (cg.find("top-app") != std::string::npos) return true;
    if (cg.find("foreground") != std::string::npos) return true;
    return false;
}

std::string detectForegroundFromProcessScan() {
    std::string pkg;
    {
        DIR* procDir = opendir("/proc");
        if (procDir) {
            struct dirent* entry;
            std::vector<FgCandidate> cands;
            static const int kMaxProcScan = 8192;
            int scanned = 0;
            while ((entry = readdir(procDir)) != nullptr && scanned < kMaxProcScan) {
                int pid = parseIntSafe(entry->d_name);
                if (pid <= 0) continue;
                scanned++;
                std::string oomPath = "/proc/" + std::to_string(pid) + "/oom_score_adj";
                int oom = readInt(oomPath.c_str(), 999);
                int pref = 0;
                if (!aatpeFgBandCandidate(oom, &pref)) continue;
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
        }
    }
    return pkg;
}

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
    if (!firstTop.empty()) return firstTop;
    return firstResumed;
}

std::string aatpeFgSourceDecision(const std::string& dumpsysPkg,
                                  const std::string& cgroupPkg,
                                  const std::string& procPkg) {
    if (!dumpsysPkg.empty() && dumpsysPkg != "unknown") return dumpsysPkg;
    if (!cgroupPkg.empty() && cgroupPkg != "unknown") return cgroupPkg;
    if (!procPkg.empty() && procPkg != "unknown") return procPkg;
    return std::string();
}

bool aatpeFgDumpsysResultFresh(long long nowMs, long long fetchedMs) {
    return (nowMs - fetchedMs) < kFgDumpsysTtlMs;
}

static std::string classifyWindowingLine(const std::string& line) {
    std::string s = line;
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    if (s.find("pinned") != std::string::npos ||
        s.find("pictureinpicture") != std::string::npos ||
        s.find("windowingmode=2") != std::string::npos) return "pictureInPicture";
    if (s.find("freeform") != std::string::npos ||
        s.find("windowingmode=5") != std::string::npos) return "freeform";
    if (s.find("split-screen") != std::string::npos ||
        s.find("splitscreen") != std::string::npos ||
        s.find("windowingmode=3") != std::string::npos) return "splitScreen";
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
    if (focusIdx >= 0) {
        size_t hi = std::min(lines.size(), (size_t)focusIdx + 40);
        for (size_t i = (size_t)focusIdx; i < hi; ++i) {
            std::string m = classifyWindowingLine(lines[i]);
            if (!m.empty()) return m;
        }
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string m = classifyWindowingLine(lines[i]);
        if (!m.empty()) return m;
    }
    return "unknown";
}

std::string detectForegroundFromDumpsys() {
    std::string out = aatpeExec("dumpsys activity activities 2>/dev/null", "FG_ACT2");
    std::string pkg = aatpeParseDumpsysFocusedPkg(out);
    writeFile((std::string(STATE_DIR) + "/frame_windowing_mode").c_str(),
              aatpeParseWindowingMode(out).c_str());
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

std::string detectForegroundFallback(std::string pkg, time_t now) {
    (void)now;
    if (pkg.empty() || pkg == "unknown") {
        pkg = "unknown";
        FRAME_LOG_INFO("FG_APP: detection FAILED (all 5 methods returned empty)");
    } else if (pkg != aatpeGetForegroundPackage()) {
        FRAME_LOG_INFO("FG_APP: foreground=%s", pkg.c_str());
    }
    aatpeSetForegroundLocked(pkg);
    writeFile((std::string(STATE_DIR) + "/foreground_app").c_str(), pkg);
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

    fgInotifyDrain();

    std::string apkResult = detectForegroundFromApk(now);
    if (!apkResult.empty()) {
        g_fgCgroupReadMs = nowMs;
        g_fgCgroupDirty = false;
        return apkResult;
    }

    long long lastReadMs = g_fgDumpsysReadMs.load();
    bool dumpsysFresh = (nowMs - lastReadMs) < kFgDumpsysTtlMs;
    if (forceRefresh || !dumpsysFresh) {
        if (nowMs - lastReadMs >= kFgDumpsysMinGapMs) {
            std::string fresh = detectForegroundFromDumpsys();
            {
                std::lock_guard<std::mutex> lk(g_foregroundMutex);
                g_fgDumpsysResult = fresh;
            }
            lastReadMs = nowMs;
            g_fgDumpsysReadMs.store(nowMs);
        }
        dumpsysFresh = (nowMs - lastReadMs) < kFgDumpsysTtlMs;
    }

    std::string dumpsysPkg;
    {
        std::lock_guard<std::mutex> lk(g_foregroundMutex);
        dumpsysPkg = g_fgDumpsysResult;
    }

    if (dumpsysFresh && !dumpsysPkg.empty() && dumpsysPkg != "unknown") {
        if (dumpsysPkg == aatpeGetForegroundPackage()) {
            return dumpsysPkg;
        }
        return detectForegroundFallback(dumpsysPkg, now);
    }

    std::string cgroupPkg = detectForegroundFromSysfs();
    std::string procPkg;
    if (cgroupPkg.empty() || cgroupPkg == "unknown") procPkg = detectForegroundFromProcessScan();

    std::string pkg = aatpeFgSourceDecision(dumpsysPkg, cgroupPkg, procPkg);
    return detectForegroundFallback(pkg, now);
}

bool detectGameProcess() {
    Config gameCfg;
    snapshotConfig(gameCfg);
    if (!gameCfg.gameDetection) return false;
    std::string fg = aatpeDetectForegroundApp();
    if (!fg.empty() && fg != "unknown") {
        if (isPackageInGameList(fg)) return true;
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
        std::string fgLower = fg;
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
    Config battCfg;
    snapshotConfig(battCfg);
    if (level <= battCfg.batterySaverThreshold) return 2;
    return 1;
}

void autoSelectProfile() {
    Config profCfg;
    snapshotConfig(profCfg);
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
    aatpe_profile::profileRequest(newProfile, aatpe_profile::ProfileSource::AUTO);
}

namespace aatpe {
namespace frameaware {

std::atomic<bool> g_jankBoostVote{false};
bool jankBoostVoteActive() { return g_jankBoostVote.load(std::memory_order_relaxed); }

std::atomic<bool> g_appFsJankActive{false};
std::atomic<bool> g_frameMissJankInput{false};

struct FrameAwareState {
    std::atomic<int> refreshRate{60};
    std::atomic<int> frameIntervalMs{16};
    std::atomic<long long> decisions{0};
    std::atomic<long long> migrations{0};
    std::atomic<bool> enabled{true};
    std::atomic<long long> lastDecisionMs{0};
    std::atomic<int> overrunCount{0};
    std::atomic<long long> lastOverrunMs{0};
};

static FrameAwareState g_frameAware;

std::atomic<bool> g_refreshRateDirty{false};
static std::atomic<bool> g_refreshWatchThreadRunning{false};
static std::thread g_refreshWatchThread;

static void refreshRateWatchLoop() {
    static const char* kWatchDirs[] = {
        "/sys/class/drm",
        "/sys/class/graphics/fb0",
        "/data/system/display_settings.xml",
    };
    int inofd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inofd < 0) return;
    bool any = false;
    for (const char* d : kWatchDirs) {
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
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;
        ssize_t n = read(inofd, buf, sizeof(buf));
        if (n > 0) {
            bool relevant = false;
            for (char* ptr = buf; ptr + sizeof(struct inotify_event) <= buf + n; ) {
                const struct inotify_event* ev = (const struct inotify_event*)ptr;
                if (ev->len > 0) {
                    std::string name(ev->name, strnlen(ev->name, ev->len));
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
    g_refreshWatchThread = std::thread(refreshRateWatchLoop);
}

void refreshRateEventStop() {
    g_refreshWatchThreadRunning.store(false, std::memory_order_relaxed);
    if (g_refreshWatchThread.joinable()) {
        g_refreshWatchThread.join();
    }
}

void updateRefreshRate() {
    int rate = aatpeDetectRefreshRate();
    static int s_lastGoodRate = 0;
    bool valid = (rate >= refresh::kMinRate && rate <= refresh::kMaxRate);
    if (valid) {
        s_lastGoodRate = rate;
    } else if (s_lastGoodRate > 0) {
        rate = s_lastGoodRate;
    } else {
        rate = refresh::kDefaultRate;
    }
    g_frameAware.refreshRate.store(rate);
    g_frameAware.frameIntervalMs.store(1000 / rate);
}

int getRefreshRate() { return g_frameAware.refreshRate.load(); }
int getFrameIntervalMs() { return g_frameAware.frameIntervalMs.load(); }
long long getDecisionCount() { return g_frameAware.decisions.load(); }
long long getMigrationCount() { return g_frameAware.migrations.load(); }

bool shouldTriggerFrameAware(long long nowMs) {
    if (!g_frameAware.enabled.load()) {
        long long lastOverrun = g_frameAware.lastOverrunMs.load();
        if (lastOverrun > 0 && nowMs - lastOverrun > 30000) {
            g_frameAware.overrunCount.store(0);
            g_frameAware.enabled.store(true);
            FRAME_LOG_INFO("Frame-Aware: re-enabled after 30s without overruns");
            g_frameAware.lastDecisionMs.store(0);
        } else {
            FRAME_LOG_DEBUG("Frame-Aware: disabled, recovery check: lastOverrun=%lld elapsed=%lldms (need >30000ms)",
                      lastOverrun, nowMs - lastOverrun);
            return false;
        }
    }

    int interval = g_frameAware.frameIntervalMs.load();
    if (interval <= 0) return false;

    long long last = g_frameAware.lastDecisionMs.load();
    if (nowMs - last < interval) return false;

    return true;
}

void executeFrameAwareDecision() {
    long long startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    g_frameAware.decisions.fetch_add(1);
    g_frameAware.lastDecisionMs.store(startMs);

    int migrated = lightweightMigrateCriticalThreads();
    if (migrated > 0) {
        g_frameAware.migrations.fetch_add(migrated);
    }

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
        if (g_frameAware.overrunCount.load() > 10) {
            g_frameAware.enabled.store(false);
            LOG_WARN("Frame-Aware: disabled due to excessive overruns (>10)");
        }
    } else if (isVmGame) {
        g_frameAware.overrunCount.store(0);
    }
}

void writeStats(const std::string& stateDir) {
    writeFile((stateDir + "/vsync_refresh_rate").c_str(), g_frameAware.refreshRate.load());
    writeFile((stateDir + "/vsync_frame_interval").c_str(), g_frameAware.frameIntervalMs.load());
    writeFile((stateDir + "/vsync_decisions").c_str(), (long long)g_frameAware.decisions.load());
    writeFile((stateDir + "/vsync_migrations").c_str(), (long long)g_frameAware.migrations.load());
    writeFile((stateDir + "/vsync_enabled").c_str(), g_frameAware.enabled.load() ? "1" : "0");
    writeFile((stateDir + "/vsync_overruns").c_str(), g_frameAware.overrunCount.load());
}

}
}
