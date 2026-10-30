#include "aatped.h"
#include "aatpe_recovery.h"
#include "aatped_threadutil.h"
#include <mutex>
#include <shared_mutex>
#include <chrono>

namespace aatpe_mech { void aatpeUclampFloorReap(int tid); }
using namespace aatpe;
using namespace aatpe_ml;

static std::shared_mutex g_rulesMutex;

static bool isValidPackageName(const std::string& pname) {
    if (pname.empty()) return false;
    if (pname.find(' ') != std::string::npos) return false;
    if (pname.find('/') != std::string::npos) return false;
    if (pname.find('[') != std::string::npos) return false;
    size_t dotPos = pname.find('.');
    if (dotPos == std::string::npos) return false;
    if (!isalpha((unsigned char)pname[0])) return false;
    std::string segment;
    for (size_t i = 0; i <= pname.size(); i++) {
        if (i == pname.size() || pname[i] == '.') {
            if (segment.empty()) return false;
            if (!isalpha((unsigned char)segment[0])) return false;
            for (char ch : segment) { if (!isalnum((unsigned char)ch) && ch != '_') return false; }
            segment.clear();
        } else { segment += pname[i]; }
    }
    return true;
}

static bool isPackageWhitelisted(const std::string& pname) {
    Config wlCfg;
    snapshotConfig(wlCfg);
    const std::string& wl = wlCfg.packageWhitelist;
    if (wl.empty()) return true;
    if (pname.empty()) return false;
    size_t start = 0;
    while (start <= wl.size()) {
        size_t comma = wl.find(',', start);
        std::string entry = (comma == std::string::npos) ? wl.substr(start) : wl.substr(start, comma - start);
        while (!entry.empty() && (entry.front()==' '||entry.front()=='\t')) entry.erase(entry.begin());
        while (!entry.empty() && (entry.back()==' '||entry.back()=='\t')) entry.pop_back();
        if (!entry.empty() && entry == pname) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}
static long readPidStatusUid(int pid) {
    if (pid <= 0) return -1;
    std::ifstream f(("/proc/" + std::to_string(pid) + "/status").c_str());
    if (!f) return -1;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Uid:", 0) == 0) {
            std::istringstream ss(line);
            std::string tag; long realUid = -1;
            ss >> tag >> realUid;
            return realUid;
        }
    }
    return -1;
}
static bool isSystemProcessToSkip(int pid) {
    long uid = readPidStatusUid(pid);
    if (uid < 0) return false;
    return uid < 10000;
}

static int readThreadNiceValue(int pid, int tid) {
    std::string statPath = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat";
    std::ifstream f(statPath);
    if (!f) return 0;
    std::string line; std::getline(f, line);
    if (line.empty()) return 0;
    size_t parenPos = line.rfind(')');
    if (parenPos == std::string::npos) return 0;
    std::string rest = line.substr(parenPos + 1);
    std::istringstream iss(rest);
    std::string field; int fn = 0;
    while (iss >> field) {
        if (fn == 16) { try { return std::stoi(field); } catch(...) { return 0; } }
        fn++;
        if (fn > 25) break;
    }
    return 0;
}

void loadThreadRules() {
    std::string rulesFile = std::string(CONF_DIR) + "/rules/thread_roles.conf";
    std::ifstream f(rulesFile);
    if (!f) return;
    std::vector<ThreadRule> built;
    std::string line;
    while (std::getline(f, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> toks;
        {
            size_t pos = 0;
            while (true) {
                size_t c = line.find(':', pos);
                if (c == std::string::npos) { toks.push_back(line.substr(pos)); break; }
                toks.push_back(line.substr(pos, c - pos));
                pos = c + 1;
            }
        }
        if (toks.size() < 5) continue;
        std::string roleStr = toks[toks.size() - 3];
        std::string subtype = toks[toks.size() - 2];
        std::string prioStr = toks[toks.size() - 1];
        std::string procPattern = toks[0];
        std::string threadPattern;
        for (size_t i = 1; i + 3 < toks.size(); ++i) threadPattern += toks[i];
        ThreadRule r;
        r.procPattern = procPattern;
        r.threadPattern = threadPattern;
        r.subtype = subtype;
        try { r.priority = std::stoi(prioStr); } catch (...) { r.priority = thread_classify::kDefaultPriority; }
        if (roleStr == "S0") r.role = ROLE_S0_SYSTEM;
        else if (roleStr == "S1") r.role = ROLE_S1_UI;
        else if (roleStr == "S2") r.role = ROLE_S2_RENDER;
        else if (roleStr == "S3") r.role = ROLE_S3_GAME;
        else if (roleStr == "S4") r.role = ROLE_S4_THROUGHPUT;
        else r.role = ROLE_S5_BACKGROUND;
        trim(r.procPattern);
        trim(r.threadPattern);
        built.push_back(r);
    }
    {
        std::unique_lock<std::shared_mutex> wl(g_rulesMutex);
        g_rules.swap(built);
    }
    LOG_INFO("Thread rules loaded: %zu", g_rules.size());
}

bool matchPattern(const std::string& text, const std::string& pattern) {
    if (pattern == "*") return true;
    if (!pattern.empty() && pattern[0] == '=') {
        return text == pattern.substr(1);
    }
    if (!pattern.empty() && pattern[0] == '^') {
        std::string prefix = pattern.substr(1);
        if (text.size() < prefix.size()) return false;
        return text.compare(0, prefix.size(), prefix) == 0;
    }
    if (!pattern.empty() && pattern.back() == '$') {
        std::string suffix = pattern.substr(0, pattern.size() - 1);
        if (text.size() < suffix.size()) return false;
        return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    size_t star = pattern.find('*');
    if (star != std::string::npos) {
        std::vector<std::string> segs;
        size_t start = 0;
        while (start <= pattern.size()) {
            size_t pos = pattern.find('*', start);
            if (pos == std::string::npos) {
                segs.push_back(pattern.substr(start));
                break;
            }
            segs.push_back(pattern.substr(start, pos - start));
            start = pos + 1;
        }
        size_t textPos = 0;
        for (size_t i = 0; i < segs.size(); i++) {
            const std::string& seg = segs[i];
            if (seg.empty()) continue;
            size_t found = text.find(seg, textPos);
            if (found == std::string::npos) return false;
            if (i == 0 && pattern[0] != '*' && found != 0) return false;
            if (i == segs.size() - 1 && pattern.back() != '*' &&
                found + seg.size() != text.size()) return false;
            textPos = found + seg.size();
        }
        return true;
    }
    return text.find(pattern) != std::string::npos;
}

static bool isGameThreadName(const std::string& tname) {
    auto wordMatch = [](const std::string& s, const char* tok) -> bool {
        size_t n = std::strlen(tok);
        size_t pos = 0;
        while ((pos = s.find(tok, pos)) != std::string::npos) {
            bool leftOk = (pos == 0) || !std::isalnum((unsigned char)s[pos - 1]);
            bool rightOk = (pos + n >= s.size()) || !std::isalnum((unsigned char)s[pos + n]);
            if (leftOk && rightOk) return true;
            pos += n;
        }
        return false;
    };
    static const char* kGameNamePatterns[] = {
        "Unity", "Unreal", "Game", "Render", "Physics", "PhysX",
        "bgfx", "Vulkan", "GLThread", "FMOD", "Wwise",
        "JobSystem", "Worker Thread", "Netcode", "NetworkUpdate"
    };
    for (const char* pat : kGameNamePatterns) {
        if (wordMatch(tname, pat)) return true;
    }
    if (wordMatch(tname, "AI"))   return true;
    if (wordMatch(tname, "GPU"))   return true;
    if (wordMatch(tname, "Gfx"))   return true;
    return false;
}

static bool isBinderThreadName(const std::string& tname, const std::string& pname) {
    if (tname.rfind("Binder:", 0) == 0) return true;
    if (tname.rfind("HwBinder:", 0) == 0) return true;
    if (tname.rfind("binder:", 0) == 0) return true;
    if (tname.find("AIDL") != std::string::npos) return true;
    static const char* kVendorBinderNames[] = {
        "mtkPower", "PowerHAL", "powerHAL", "TelBinder", "AudioBinder",
        "CodecBinder", "sensor@", "android.hardware", "vendor."
    };
    for (const char* pat : kVendorBinderNames) {
        if (tname.find(pat) != std::string::npos) return true;
        if (!pname.empty() && pname.find(pat) != std::string::npos) return true;
    }
    return false;
}

static ThreadRole classifyThreadImpl(int pid, int tid, const std::string& tname, const std::string& pname, int& outPriority, std::string& outSubtype, int* outConfidence);

static unsigned long long readThreadStarttime(int pid, int tid);

ThreadRole classifyThread(int pid, int tid, const std::string& tname, const std::string& pname, int& outPriority, std::string& outSubtype, int* outConfidence, unsigned long long cachedStarttime) {
    unsigned long long st = (cachedStarttime != 0) ? cachedStarttime : readThreadStarttime(pid, tid);
    std::string cacheKey;
    if (st != 0) {
        cacheKey = std::to_string(tid) + "|" + std::to_string(st) + "|" + tname + "|" + pname;
    } else {
        cacheKey = std::to_string(tid) + "|" + tname + "|" + pname;
    }
    static std::unordered_map<std::string, std::tuple<ThreadRole, int, std::string, int, long long>> s_classifyCache;
    static std::mutex s_classifyCacheMutex;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    static constexpr long long kClassifyCacheTtlMs = 2000;
    static constexpr size_t kClassifyCacheMax = 1024;
    {
        std::lock_guard<std::mutex> lock(s_classifyCacheMutex);
        auto it = s_classifyCache.find(cacheKey);
        if (it != s_classifyCache.end() && (nowMs - std::get<4>(it->second)) < kClassifyCacheTtlMs) {
            outPriority = std::get<1>(it->second);
            outSubtype = std::get<2>(it->second);
            if (outConfidence) *outConfidence = std::get<3>(it->second);
            return std::get<0>(it->second);
        }
    }
    int confidence = 0;
    ThreadRole role = classifyThreadImpl(pid, tid, tname, pname, outPriority, outSubtype, &confidence);
    {
        std::lock_guard<std::mutex> lock(s_classifyCacheMutex);
        s_classifyCache[cacheKey] = std::make_tuple(role, outPriority, outSubtype, confidence, nowMs);
        if (s_classifyCache.size() > kClassifyCacheMax) {
            for (auto it = s_classifyCache.begin(); it != s_classifyCache.end(); ) {
                if (nowMs - std::get<4>(it->second) >= kClassifyCacheTtlMs)
                    it = s_classifyCache.erase(it);
                else
                    ++it;
            }
            if (s_classifyCache.size() > kClassifyCacheMax) {
                auto oldest = s_classifyCache.begin();
                for (auto it = s_classifyCache.begin(); it != s_classifyCache.end(); ++it) {
                    if (std::get<4>(it->second) < std::get<4>(oldest->second)) oldest = it;
                }
                s_classifyCache.erase(oldest);
            }
        }
    }
    if (outConfidence) *outConfidence = confidence;
    return role;
}

static ThreadRole classifyThreadImpl(int pid, int tid, const std::string& tname, const std::string& pname, int& outPriority, std::string& outSubtype, int* outConfidence) {
    int conf = 0;
    if (isBinderThreadName(tname, pname)) {
        outPriority = thread_classify::kDefaultPriority;
        outSubtype = "VENDOR_BINDER_SYSTEM";
        if (outConfidence) *outConfidence = 85;
        return ROLE_S0_SYSTEM;
    }
    if (tname == "CrRendererMain") {
        outSubtype = "WEBVIEW_RENDERER"; if (outConfidence) *outConfidence=85; return ROLE_S1_UI;
    }
    if (aatpe::isRenderWhitelisted(tname.c_str())) {
        outSubtype = "RENDER_WHITELISTED"; if (outConfidence) *outConfidence=85; return ROLE_S2_RENDER;
    }
    if (aatpe::isFlutterEngineThread(tname.c_str())) {
        std::string s = tname;
        if (s.size() >= 7 && s.compare(s.size()-7, 7, ".raster")==0) {
            outSubtype = "FLUTTER_RASTER"; if (outConfidence) *outConfidence=85; return ROLE_S2_RENDER;
        }
        if (s.size() >= 4 && s.compare(s.size()-4, 4, ".gpu")==0) {
            outSubtype = "FLUTTER_GPU"; if (outConfidence) *outConfidence=85; return ROLE_S2_RENDER;
        }
        outSubtype = "FLUTTER_UI"; if (outConfidence) *outConfidence=85; return ROLE_S1_UI;
    }
    {
        std::shared_lock<std::shared_mutex> rl(g_rulesMutex);
        for (const auto& r : g_rules) {
            bool procMatch = matchPattern(pname, r.procPattern);
            bool threadMatch = matchPattern(tname, r.threadPattern);
            if (procMatch && threadMatch) {
                outPriority = r.priority;
                outSubtype = r.subtype;
                conf = (r.procPattern == "*") ? 80 : 90;
                if (outConfidence) *outConfidence = conf;
                return r.role;
            }
        }
    }
    outPriority = thread_classify::kDefaultPriority;
    outSubtype = "unknown";
    if (tname == "main") { outSubtype = "UI_MAIN_FALLBACK"; if (outConfidence) *outConfidence = 75; return ROLE_S1_UI; }
    if (tname == "RenderThread") { outSubtype = "RENDER_FALLBACK"; if (outConfidence) *outConfidence = 75; return ROLE_S2_RENDER; }
    if (tname != "main" && tname != "RenderThread") {
        std::string schedstatPath = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/schedstat";
        std::ifstream f(schedstatPath);
        if (f) {
            unsigned long long runTime = 0, waitTime = 0, nrSwitches = 1;
            f >> runTime >> waitTime >> nrSwitches;
            if (f.fail()) { if (outConfidence) *outConfidence = 20; return ROLE_S5_BACKGROUND; }
            if (nrSwitches == 0) nrSwitches = 1;
            unsigned long long avgRun = runTime / nrSwitches / 1000;
            if (isGameThreadName(tname) && avgRun > thread_classify::kGameAvgRunUs) {
                outSubtype = "GAME_HEURISTIC";
                if (outConfidence) *outConfidence = 70;
                return ROLE_S3_GAME;
            }
            if (avgRun > thread_classify::kThroughputAvgRunUs) {
                if (outConfidence) *outConfidence = 55;
                return ROLE_S4_THROUGHPUT;
            }
            if (avgRun > thread_classify::kGameAvgRunUs) {
                if (outConfidence) *outConfidence = 45;
                return ROLE_S3_GAME;
            }
            if (avgRun > thread_classify::kUiAvgRunUs && nrSwitches > thread_classify::kUiMinSwitches) {
                if (outConfidence) *outConfidence = 55;
                return ROLE_S1_UI;
            }
        }
    }
    if (outConfidence) *outConfidence = 30;
    return ROLE_S5_BACKGROUND;
}

static unsigned long long readThreadStarttime(int pid, int tid);

enum class RefreshResult { KEEP, REUSE, GONE };

static void invalidateAuxThreadState(int tid) {
    if (tid <= 0) return;
    {
        std::lock_guard<std::mutex> lk(g_lastMigMutex);
        g_lastMigrationTime.erase(tid);
    }
    {
        std::lock_guard<std::mutex> lk(g_migFailMutex);
        g_migFailCooldown.erase(tid);
        g_migFailCount.erase(tid);
        g_migFailCpu.erase(tid);
        g_migFailBackoffUntilMs.erase(tid);
    }
    {
        std::lock_guard<std::mutex> lk(g_unmigratableMutex);
        g_unmigratableThreads.erase(tid);
    }
    placementInvalidateSettle(tid);
    aatpe_mech::aatpeUclampFloorReap(tid);
}

static bool populateNewThread(int pid, int tid, const std::string& pname,
                              const std::string& tname, bool ignoreSys,
                              ThreadInfo& out) {
    out.pid = pid;
    out.tid = tid;
    out.pname = pname;
    out.tname = tname;
    out.starttime = readThreadStarttime(pid, tid);
    int prio = thread_classify::kDefaultPriority;
    std::string subtype;
    int conf = 0;
    out.role = classifyThread(pid, tid, tname, pname, prio, subtype, &conf, out.starttime);
    out.priority = prio;
    out.subtype = subtype;
    out.roleConfidence = conf;
    out.protect = (out.role == ROLE_S0_SYSTEM);
    if (ignoreSys && out.role == ROLE_S0_SYSTEM) return false;
    std::string schedstatPath = "/proc/" + std::to_string(pid) + "/task/" +
                                 std::to_string(tid) + "/schedstat";
    std::ifstream sf(schedstatPath);
    if (!sf) return false;
    unsigned long long schedWaitField = 0;
    sf >> out.runTime >> schedWaitField >> out.nrSwitches;
    (void)schedWaitField;
    if (sf.fail()) return false;
    if (out.nrSwitches == 0) return false;
    out.currentCpu = readThreadCurrentCpu(pid, tid);
    if (out.currentCpu < 0) return false;
    out.nice = readThreadNiceValue(pid, tid);
    return true;
}

static constexpr int kIdleSkipThreshold   = 6;
static constexpr int kLongIdleRescanPeriod = 10;

static RefreshResult refreshTrackedThread(ThreadInfo& ti, bool checkStarttime) {
    if (ti.needsHydration) {
        return RefreshResult::REUSE;
    }

    {
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(g_scoreCacheMutex);
        auto sit = g_scoreCache.find(ti.tid);
        if (sit != g_scoreCache.end()) {
            long long lastSw = sit->second.lastSwitchMs;
            if (lastSw != 0 && (nowMs - lastSw) < kInactiveMs) {
                ti.idleRefreshes = 0;
                ti.longIdleRescan = 0;
            }
        }
    }

    bool skipSchedstat = (ti.idleRefreshes >= kIdleSkipThreshold) &&
                         (ti.longIdleRescan < kLongIdleRescanPeriod);
    if (skipSchedstat) {
        ti.longIdleRescan++;
        return RefreshResult::KEEP;
    }
    ti.longIdleRescan = 0;

    std::string schedstatPath = "/proc/" + std::to_string(ti.pid) + "/task/" +
                                 std::to_string(ti.tid) + "/schedstat";
    std::ifstream sf(schedstatPath);
    if (!sf) return RefreshResult::KEEP;
    unsigned long long schedWaitField = 0, runTime = 0, nrSwitches = 0;
    sf >> runTime >> schedWaitField >> nrSwitches;
    (void)schedWaitField;
    if (sf.fail()) return RefreshResult::KEEP;
    if (nrSwitches == 0) return RefreshResult::KEEP;

    if (nrSwitches == ti.nrSwitches) {
        ti.idleRefreshes++;
    } else {
        ti.idleRefreshes = 0;
    }

    if (checkStarttime) {
        unsigned long long st = readThreadStarttime(ti.pid, ti.tid);
        if (st != 0 && ti.starttime != 0 && st != ti.starttime) return RefreshResult::REUSE;
    }
    uint64_t nowNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (ti.lastSwitchStampNs > 0 && nowNs > ti.lastSwitchStampNs && nrSwitches >= ti.nrSwitches) {
        double dt = (double)(nowNs - ti.lastSwitchStampNs) / 1e9;
        if (dt > 0.001) {
            double rate = (double)(nrSwitches - ti.nrSwitches) / dt;
            ti.switchesPerSec = (rate > 0.0) ? rate : 0.0;
        }
    } else {
        ti.switchesPerSec = 0.0;
    }
    ti.lastSwitchStampNs = nowNs;
    ti.runTime = runTime;
    ti.nrSwitches = nrSwitches;
    return RefreshResult::KEEP;
}
static std::string sanitizeCommName(std::string s) {
    while (!s.empty()) {
        unsigned char b = (unsigned char)s.back();
        if (b < 0x80) break;
        if ((b & 0xC0) == 0xC0) { s.pop_back(); continue; }
        int need = 0, leadIdx = -1;
        for (int i = (int)s.size() - 1; i >= 0; --i) {
            unsigned char c = (unsigned char)s[i];
            if ((c & 0xC0) == 0x80) continue;
            if      ((c & 0xE0) == 0xC0) need = 1;
            else if ((c & 0xF0) == 0xE0) need = 2;
            else if ((c & 0xF8) == 0xF0) need = 3;
            else need = 0;
            leadIdx = i; break;
        }
        if (leadIdx < 0) { s.pop_back(); continue; }
        int have = (int)s.size() - 1 - leadIdx;
        if (have == need) break;
        s.resize(leadIdx);
    }
    return s;
}

#ifdef AATPE_HOST_TEST
std::string threadScanSanitizeCommForTest(const std::string& in) { return sanitizeCommName(in); }
int threadScanRefreshForTest(ThreadInfo& ti, bool checkStarttime) {
    return (int)refreshTrackedThread(ti, checkStarttime);
}
#endif

static void jsonEscapeStr(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; snprintf(b,sizeof(b),"\\u%04x",(unsigned char)c); out += b; }
                else out += c;
        }
    }
}

void persistThreadDb(const std::unordered_map<int, ThreadInfo>& threads) {
    std::string dbDir = std::string(RUN_DIR) + "/thread_db";
    mkdirIfNotExist(dbDir.c_str(), 0700);

    static std::atomic<bool> s_legacyCleaned{false};
    if (!s_legacyCleaned.load(std::memory_order_acquire)) {
        DIR* dir = opendir(dbDir.c_str());
        if (dir) {
            struct dirent* e;
            while ((e = readdir(dir)) != nullptr) {
                if (e->d_name[0] == '.') continue;
                std::string name = e->d_name;
                if (name == "thread_db.json" || name == "thread_db.json.tmp") continue;
                std::string fpath = dbDir + "/" + name;
                unlink(fpath.c_str());
            }
            closedir(dir);
        }
        s_legacyCleaned.store(true, std::memory_order_release);
    }

    std::string json = "{\"version\":1,\"count\":" + std::to_string(threads.size()) + ",\"threads\":[";
    bool first = true;
    for (const auto& kv : threads) {
        const ThreadInfo& ti = kv.second;
        if (!first) json += ",";
        first = false;
        json += "{\"pid\":" + std::to_string(ti.pid);
        json += ",\"tid\":" + std::to_string(ti.tid);
        json += ",\"pname\":\""; jsonEscapeStr(json, ti.pname); json += "\"";
        json += ",\"tname\":\"";
        if (ti.tname.empty()) jsonEscapeStr(json, "[tid " + std::to_string(ti.tid) + "]");
        else                 jsonEscapeStr(json, ti.tname);
        json += "\"";
        json += ",\"role\":" + std::to_string((int)ti.role);
        json += ",\"subtype\":\""; jsonEscapeStr(json, ti.subtype); json += "\"";
        json += ",\"priority\":" + std::to_string(ti.priority);
        json += ",\"protect\":" + std::string(ti.protect ? "1" : "0");
        json += ",\"nr_switches\":" + std::to_string(ti.nrSwitches);
        json += ",\"run_time\":" + std::to_string(ti.runTime) + "}";
    }
    json += "]}";

    std::string finalPath = dbDir + "/thread_db.json";
    std::string tmpPath    = finalPath + ".tmp";
    int fd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { writeFile(finalPath.c_str(), json); return; }
    size_t written = 0;
    const char* data = json.data();
    size_t len = json.size();
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        written += (size_t)n;
    }
    if (fsync(fd) != 0 || written != len) {
        close(fd);
        unlink(tmpPath.c_str());
        writeFile(finalPath.c_str(), json);
        return;
    }
    close(fd);
    if (rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
        LOG_WARN("persistThreadDb: rename %s -> %s failed errno=%d",
                 tmpPath.c_str(), finalPath.c_str(), errno);
        unlink(tmpPath.c_str());
    }
}

static unsigned long long readThreadStarttime(int pid, int tid) {
    std::string statPath = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat";
    std::ifstream f(statPath);
    if (!f) return 0;
    std::string content;
    std::getline(f, content);
    if (content.empty()) return 0;
    size_t rp = content.rfind(')');
    if (rp == std::string::npos) return 0;
    std::string rest = content.substr(rp + 1);
    std::istringstream iss(rest);
    std::string field;
    int idx = 0;
    unsigned long long starttime = 0;
    while (iss >> field) {
        if (idx == 19) {
            try { starttime = std::stoull(field); } catch (...) { starttime = 0; }
            break;
        }
        idx++;
    }
    return starttime;
}

void scanThreads() {
    Config scanCfg;
    snapshotConfig(scanCfg);
    int configuredMax = scanCfg.maxTrackedThreads;
    const int kDefaultMaxTracked = 512;
    auto adaptiveBudget = [](int observed) -> int {
        long long b = (long long)(observed * 1.15);
        if (b < 512) b = 512;
        if (b > 2048) b = 2048;
        return (int)b;
    };
    int maxTracked = configuredMax;
    if (configuredMax == kDefaultMaxTracked) {
        size_t prevObserved = 0;
        { std::lock_guard<std::mutex> lk(g_threadsMutex); prevObserved = g_threads.size(); }
        maxTracked = adaptiveBudget((int)prevObserved);
    }
    bool ignoreSys = scanCfg.ignoreSystemThreads;

    static std::atomic<int> s_scanCounter{0};
    s_scanCounter.fetch_add(1, std::memory_order_relaxed);
    static constexpr int kStarttimeStaggerPeriod = 10;

    std::unordered_map<int, ThreadInfo> work;
    std::set<int> snapshotTids;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        work = g_threads;
        for (const auto& kv : g_threads) snapshotTids.insert(kv.first);
    }

    int fgPid = -1;
    std::string fgPkg;
    {
        fgPkg = aatpeDetectForegroundApp();
        if (!fgPkg.empty() && fgPkg != "unknown") {
            fgPid = findPidByPackage(fgPkg);
        }
    }

    std::set<int> liveTids;

    auto scanPid = [&](int pid, bool isFg) {
        if (pid <= 0) return;
        std::string cmdlinePath = "/proc/" + std::to_string(pid) + "/cmdline";
        std::ifstream cmdf(cmdlinePath);
        if (!cmdf) return;
        std::string pname;
        std::getline(cmdf, pname, '\0');
        if (pname.empty()) {
            pname = sanitizeCommName(readFile(("/proc/" + std::to_string(pid) + "/comm").c_str()));
        }
        if (pname.empty()) return;
        if (isFg) {
            if (!isValidPackageName(pname)) return;
            if (isSystemProcessToSkip(pid)) return;
        } else {
            if (!isPackageWhitelisted(pname)) return;
        }
        std::string taskDir = "/proc/" + std::to_string(pid) + "/task";
        DIR* taskDirp = opendir(taskDir.c_str());
        if (!taskDirp) {
            for (const auto& kv : work) {
                if (kv.second.pid == pid) liveTids.insert(kv.first);
            }
            return;
        }
        struct dirent* taskEntry;
        while ((taskEntry = readdir(taskDirp)) != nullptr) {
            char* endptr = nullptr;
            long tidLong = strtol(taskEntry->d_name, &endptr, 10);
            if (endptr == taskEntry->d_name || *endptr != '\0' || tidLong <= 0 || tidLong > INT_MAX) continue;
            int tid = (int)tidLong;
            liveTids.insert(tid);

            auto it = work.find(tid);
            if (it != work.end()) {
                bool checkSt = ((s_scanCounter.load(std::memory_order_relaxed) + tid) % kStarttimeStaggerPeriod) == 0;
                RefreshResult r = refreshTrackedThread(it->second, checkSt);
                if (r == RefreshResult::REUSE) {
                    std::string tname = sanitizeCommName(readFile((taskDir + "/" + std::to_string(tid) + "/comm").c_str()));
                    if (tname.empty() || isKernelThread(tname, tid)) continue;
                    ThreadInfo ti;
                    if (populateNewThread(pid, tid, pname, tname, ignoreSys, ti)) {
                        invalidateAuxThreadState(tid);
                        it->second = ti;
                    } else {
                        it = work.erase(it);
                        continue;
                    }
                }
                continue;
            }

            if ((int)work.size() >= maxTracked && pname != fgPkg) continue;
            std::string tname = sanitizeCommName(readFile((taskDir + "/" + std::to_string(tid) + "/comm").c_str()));
            if (tname.empty()) continue;
            if (isKernelThread(tname, tid)) continue;
            ThreadInfo ti;
            if (populateNewThread(pid, tid, pname, tname, ignoreSys, ti)) {
                invalidateAuxThreadState(tid);
                work[tid] = ti;
            }
        }
        closedir(taskDirp);
    };

    if (fgPid > 0) {
        scanPid(fgPid, true);
    }

    DIR* procDir = opendir("/proc");
    if (procDir) {
        struct dirent* procEntry;
        while ((procEntry = readdir(procDir)) != nullptr) {
            char* endptr = nullptr;
            long pidLong = strtol(procEntry->d_name, &endptr, 10);
            if (endptr == procEntry->d_name || *endptr != '\0' || pidLong <= 0 || pidLong > INT_MAX) continue;
            int pid = (int)pidLong;
            if (pid == fgPid) continue;
            scanPid(pid, false);
        }
        closedir(procDir);
    } else {
        LOG_WARN("[SCAN] opendir(/proc) failed errno=%d; skipping this scan to preserve tracked threads", errno);
        return;
    }

    for (auto it = work.begin(); it != work.end(); ) {
        if (liveTids.count(it->first) == 0) it = work.erase(it);
        else ++it;
    }

    std::unordered_map<int, ThreadInfo> threadsCopy;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        for (auto& kv : work) {
            g_threads[kv.first] = std::move(kv.second);
        }
        for (int tid : snapshotTids) {
            if (work.find(tid) == work.end()) {
                g_threads.erase(tid);
                cleanupThreadScoringState(tid);
                aatpe_recovery::recoveryErase(tid);
            }
        }
        while ((int)g_threads.size() > maxTracked) {
            auto victim = g_threads.end();
            int worstIdle = -1;
            for (auto mt = g_threads.begin(); mt != g_threads.end(); ++mt) {
                if (!mt->second.protect && mt->second.role == ROLE_S5_BACKGROUND) {
                    if (mt->second.idleRefreshes > worstIdle) {
                        worstIdle = mt->second.idleRefreshes;
                        victim = mt;
                    }
                }
            }
            if (victim == g_threads.end()) break;
            int evictTid = victim->first;
            g_threads.erase(victim);
            cleanupThreadScoringState(evictTid);
            aatpe_recovery::recoveryErase(evictTid);
        }
        threadsCopy = g_threads;
    }
    persistThreadDb(threadsCopy);
    writeFile((std::string(STATE_DIR) + "/tracked_threads").c_str(), (int)threadsCopy.size());
    {
        long long sum = 0; int n = 0;
        for (const auto& kv : threadsCopy) {
            if (kv.second.roleConfidence > 0) { sum += kv.second.roleConfidence; n++; }
        }
        int avg = (n > 0) ? (int)(sum / n) : 0;
        writeFile((std::string(STATE_DIR) + "/thread_role_confidence").c_str(), avg);
    }
    LOG_DEBUG("Scanned %zu threads (fgPid=%d)", threadsCopy.size(), fgPid);
}
