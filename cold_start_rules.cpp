#include "cold_start_rules.h"
#include "aatped.h"
#include "aatped_utils.h"
#include "aatped_priority.h"
#include "aatped_config.h"
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

static constexpr long long kColdStartMinMs = 15000;
static constexpr long long kColdStartMaxMs = 30000;
static constexpr int kConvergeStreak = 3;
static constexpr long long kSwitchDebounceMs = 1000;
static constexpr long long kWarmBoostMinMs = 0;
static constexpr long long kWarmBoostMaxMs = 5000;

namespace {
std::mutex  s_coldMutex;
std::string s_coldPkg;
long long   s_coldStartMs = 0;
bool        s_active = false;
bool        s_thermalFreeze = false;
bool        s_ioPressure = false;
int         s_convergeStreak = 0;
std::string s_lastDisarmPkg;
long long   s_lastDisarmMs = 0;
#include <unordered_map>
#include <deque>
static constexpr long long kArmWindowMs = 60000;
static constexpr int kArmsPerWindow = 3;
std::unordered_map<std::string, std::deque<long long>> s_armHistory;
long long s_coldRejected = 0;

struct ProcInstance { int pid = 0; unsigned long long start_time = 0; };
std::unordered_map<std::string, ProcInstance> s_lastInstance;
bool s_isCold = false;

#ifdef AATPE_HOST_TEST
static long long (*g_nowOverrideForTest)() = nullptr;
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

#ifdef AATPE_HOST_TEST
static bool (*g_jankVoteForTest)() = nullptr;
static std::string (*g_displayedForTest)() = nullptr;
#endif
static bool coldStartJankActive() {
#ifdef AATPE_HOST_TEST
    if (g_jankVoteForTest) return g_jankVoteForTest();
#endif
    return aatpe::frameaware::jankBoostVoteActive();
}

struct Preset { ThreadRole role; int level; };
Preset presetForThread(const ThreadInfo& t) {
    std::string n = t.tname;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (n.find("renderthread") != std::string::npos) {
        return {ROLE_S2_RENDER, 3};
    }
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
    if (t.tid == t.pid) {
        return {ROLE_S3_GAME, 3};
    }
    return {ROLE_S0_SYSTEM, 0};
}

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
}

void coldStartSetThermalFreeze(bool f) {
    std::lock_guard<std::mutex> lk(s_coldMutex);
    s_thermalFreeze = f;
}
void coldStartSetIoPressure(bool f) {
    std::lock_guard<std::mutex> lk(s_coldMutex);
    s_ioPressure = f;
}

#ifdef AATPE_HOST_TEST
void coldStartSetNowForTest(long long (*fn)()) {
    g_nowOverrideForTest = fn;
}
void coldStartSetJankForTest(bool (*fn)()) { g_jankVoteForTest = fn; }
void coldStartSetDisplayedForTest(std::string (*fn)()) { g_displayedForTest = fn; }

long long coldStartRejectedCountForTest() {
    std::lock_guard<std::mutex> lk(s_coldMutex);
    return s_coldRejected;
}

void coldStartSetProcIdentityForTest(const std::string& pkg, int pid,
                                     unsigned long long start_time) {
    g_testIdentityActive = true;
    if (pid <= 0) s_testIdentity.erase(pkg);
    else          s_testIdentity[pkg] = {pid, start_time};
}

bool coldStartIsColdForTest() {
    std::lock_guard<std::mutex> lk(s_coldMutex);
    return s_isCold;
}
#endif

static void writeColdStartWindow(const char* stage, const std::string& pkg, long long remMs) {
    std::string v = std::string(stage) + " pkg=" + (pkg.empty() ? std::string("-") : pkg) +
                    " remain_ms=" + std::to_string(remMs);
    writeFile((std::string(STATE_DIR) + "/cold_start_window").c_str(), v);
}

bool coldStartDisplayedPresent(const std::string& out) {
    if (out.empty()) return false;
    if (out.find("Displayed ") != std::string::npos) return true;
    size_t pos = out.find("mDisplayed=");
    if (pos != std::string::npos) {
        pos += 11;
        while (pos < out.size() && out[pos]==' ') pos++;
        if (pos+1 < out.size() && (out[pos]=='0') &&
            (out[pos+1]=='x' || out[pos+1]=='X')) pos += 2;
        while (pos < out.size() && out[pos]=='0') pos++;
        if (pos < out.size() && out[pos] >= '1' && out[pos] <= '9') return true;
        if (pos < out.size() && ((out[pos]>='a'&&out[pos]<='f')||(out[pos]>='A'&&out[pos]<='F'))) return true;
    }
    return false;
}

static std::string coldStartFetchDisplayed() {
    return aatpeExecArgv({"/system/bin/dumpsys", "activity", "activities"},
                         "coldstart-displayed");
}

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
        size_t nul = cmdline.find('\0');
        if (nul != std::string::npos) cmdline.resize(nul);
        size_t a = cmdline.find_first_not_of(" \t\r\n");
        size_t b = cmdline.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        cmdline = cmdline.substr(a, b - a + 1);
        if (cmdline == pkg) { exactPid = pid; break; }
        if (prefixPid == 0 && cmdline.size() > namedPrefix.size() &&
            cmdline.compare(0, namedPrefix.size(), namedPrefix) == 0) {
            prefixPid = pid;
        }
    }
    closedir(d);
    int foundPid = (exactPid > 0) ? exactPid : prefixPid;
    if (foundPid <= 0) return false;

    std::string stat =
        readFile(("/proc/" + std::to_string(foundPid) + "/stat").c_str());
    size_t rp = stat.rfind(')');
    if (rp == std::string::npos) return false;
    std::istringstream iss(stat.substr(rp + 1));
    std::string field; int fn = 0;
    unsigned long long start = 0; bool got = false;
    while (iss >> field) {
        if (fn == 19) {
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
        int curPid = 0;
        unsigned long long curStart = 0;
        bool haveId = resolveProcIdentity(pkg, curPid, curStart);

        std::lock_guard<std::mutex> lk(s_coldMutex);
        if (s_active && s_coldPkg == pkg) return;
        long long now = nowMs();
        if (!s_active && pkg == s_lastDisarmPkg && (now - s_lastDisarmMs) < kSwitchDebounceMs) {
            s_coldStartMs = now;
            return;
        }
        bool isCold = true;
        if (haveId) {
            auto it = s_lastInstance.find(pkg);
            if (it != s_lastInstance.end() &&
                it->second.pid == curPid && it->second.start_time == curStart) {
                isCold = false;
            }
        }
        auto itHist = s_armHistory.find(pkg);
        if (itHist != s_armHistory.end()) {
            auto& hist = itHist->second;
            while (!hist.empty() && (now - hist.front()) > kArmWindowMs) hist.pop_front();
            if (hist.empty()) { s_armHistory.erase(itHist); itHist = s_armHistory.end(); }
        }
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
        s_convergeStreak = 0;
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
            releaseColdBoost(s_coldPkg);
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

    const long long winMinMs = isCold ? kColdStartMinMs : kWarmBoostMinMs;
    const long long winMaxMs = isCold ? kColdStartMaxMs : kWarmBoostMaxMs;

    long long age = nowMs() - startMs;
    bool noJank = !coldStartJankActive();
    if (noJank) s_convergeStreak++; else s_convergeStreak = 0;

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
        writeColdStartWindow("converged", pkg, 0);
        releaseColdBoost(pkg);
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

    long long remain = winMaxMs - age;
    if (freeze)      writeColdStartWindow("skipped_thermal", pkg, remain);
    else if (ioPress) writeColdStartWindow("skipped_io", pkg, remain);
    else             writeColdStartWindow("displayed_wait", pkg, remain);

    if (freeze) return;
    if (ioPress) return;

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
    return s_coldPkg;
}

}
