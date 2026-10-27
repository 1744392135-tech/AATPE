// AATPE - Thread Scanning & Classification (fix45 Problem 5: split from aatped.cpp)
// Contains: loadThreadRules, matchPattern, classifyThread, isKernelThread,
// findPidByPackage, scanThreadsOfPid, scanThreads, persistThreadDb.
#include "aatped.h"
#include "aatpe_recovery.h"   // recoveryErase for gone threads
#include "aatped_threadutil.h" // G4 unified thread-name truth (ROUND-014 P3)
#include <mutex>
#include <shared_mutex>
#include <chrono>

// RC6 (B2): per-tid uclamp floor ledger reap (real impl in aatpe_placement_mech.cpp;
// neutral no-op stub in tests/stubs/thread_deps.cpp for the host thread_scan test).
namespace aatpe_mech { void aatpeUclampFloorReap(int tid); }
// .cpp files may use using namespace (not included elsewhere)
using namespace aatpe;
using namespace aatpe_ml;

// R3-003: g_rules is a global vector (defined in aatped_main.cpp) that loadThreadRules()
// SWAPS on a config reload while classifyThreadImpl() iterates it on the scanning
// thread. Without a lock a reload mid-iteration reads freed / swapped vectors
// (use-after-free / torn reads). A shared_mutex lets many classifiers read
// concurrently while a reload takes the unique write side.
static std::shared_mutex g_rulesMutex;

// merge: phase1 item5 - 包名白名单验证
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

// Package whitelist check. Returns true if pname is in the whitelist
// or whitelist is empty (allow all). Whitelist is comma-separated package names.
static bool isPackageWhitelisted(const std::string& pname) {
    // P1-1: snapshot g_cfg to avoid torn read racing config hot-reload.
    Config wlCfg;
    snapshotConfig(wlCfg);
    const std::string& wl = wlCfg.packageWhitelist;
    if (wl.empty()) return true;  // empty whitelist = allow all
    if (pname.empty()) return false;
    size_t start = 0;
    while (start <= wl.size()) {
        size_t comma = wl.find(',', start);
        std::string entry = (comma == std::string::npos) ? wl.substr(start) : wl.substr(start, comma - start);
        // Trim whitespace
        while (!entry.empty() && (entry.front()==' '||entry.front()=='\t')) entry.erase(entry.begin());
        while (!entry.empty() && (entry.back()==' '||entry.back()=='\t')) entry.pop_back();
        if (!entry.empty() && entry == pname) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}
// B-004 (zero package-name hardcoding): judge system processes by UID instead of an
// enumerated package-name list. Every third-party AND foreground app runs at
// AID_APP_START (10000) or above; system services — system_server, telecom/nfc/
// providers (com.android.*), AND any vendor system component such as
// com.mediatek.*/com.qualcomm.* — run in the <10000 system/shared-uid range. We
// therefore never promote a foreground process whose real UID is below the app
// range. This is package-name-free and vendor-agnostic: a new SoC vendor or a new
// AOSP system component needs no code change.
//
// Falls back to "do not skip" when /proc/<pid>/status cannot be read (pid gone) —
// promotion of an unidentifiable process is the conservative choice, and scanPid
// bails shortly after anyway.
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
    if (uid < 0) return false;      // unknown -> do not skip (conservative)
    return uid < 10000;            // system/shared daemon range -> skip
}

// merge: phase2 item3 - nice值预读取
static int readThreadNiceValue(int pid, int tid) {
    std::string statPath = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat";
    std::ifstream f(statPath);
    if (!f) return 0;
    std::string line; std::getline(f, line);
    if (line.empty()) return 0;
    size_t parenPos = line.rfind(')');
    if (parenPos == std::string::npos) return 0;
    // P3: do NOT hardcode +2 (that assumes exactly one space after ')'). Start
    // at +1 and let istringstream skip any leading whitespace (space/tab) itself.
    std::string rest = line.substr(parenPos + 1);
    std::istringstream iss(rest);
    std::string field; int fn = 0;
    // nice is at 0-indexed position 16 after comm. Read up to idx 25 as a safe
    // margin (previously fn>17 could skip nice on malformed lines); let the while
    // loop end naturally if the stream runs out.
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
    // idempotent reload. Previously this pushed straight into the
    // shared g_rules; because loadThreadRules() can be re-invoked (init + any
    // rules reload), every reload APPENDED another full copy of the file, so
    // matchPattern() iterated N^2 redundant rules. Build into a local vector and
    // swap into g_rules only once the parse completes. A missing file (early
    // return above) intentionally leaves the previously loaded rules intact.
    std::vector<ThreadRule> built;
    std::string line;
    while (std::getline(f, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        // CFG-binder: anchor role/subtype/priority from the RIGHT so a malformed
        // extra colon (e.g. "*:binder:*:S0:BINDER:95", 6 fields instead of 5) no
        // longer shifts S0 into the subtype slot and makes stoi("BINDER:95") throw.
        // Canonical format is proc:thread:role:subtype:priority (5 fields). Thread
        // names never contain ':' in practice, so any extra middle tokens are
        // concatenated into threadPattern (recovering "*:binder*:S0:BINDER:95").
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
        if (toks.size() < 5) continue;   // malformed: skip
        std::string roleStr = toks[toks.size() - 3];
        std::string subtype = toks[toks.size() - 2];
        std::string prioStr = toks[toks.size() - 1];
        std::string procPattern = toks[0];
        // Concatenate any middle tokens between proc and role as the thread
        // pattern. Thread names never contain ':' in practice, so joining them
        // (no separator) recovers "binder*" from a malformed "binder:*" line.
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
    // R3-003: take the write side of g_rulesMutex so a concurrent
    // classifyThreadImpl() can't iterate while we swap the vector out.
    {
        std::unique_lock<std::shared_mutex> wl(g_rulesMutex);
        g_rules.swap(built);
    }
    LOG_INFO("Thread rules loaded: %zu", g_rules.size());
}

// fix: support glob-style '*' wildcard (e.g. "Binder_*", "kworker*")
// P2-21: Thread role pattern matching with explicit match types.
//   "=pattern"  -> exact match (text == pattern)
//   "^pattern"  -> prefix match (text starts with pattern)
//   "pattern$"  -> suffix match (text ends with pattern)
//   "*pattern*" -> wildcard match (contains *)
//   plain pattern -> substring match (backward compatible with old configs)
bool matchPattern(const std::string& text, const std::string& pattern) {
    if (pattern == "*") return true;
    // Exact match: "=pattern"
    if (!pattern.empty() && pattern[0] == '=') {
        return text == pattern.substr(1);
    }
    // Prefix match: "^pattern"
    if (!pattern.empty() && pattern[0] == '^') {
        std::string prefix = pattern.substr(1);
        if (text.size() < prefix.size()) return false;
        return text.compare(0, prefix.size(), prefix) == 0;
    }
    // Suffix match: "pattern$"
    if (!pattern.empty() && pattern.back() == '$') {
        std::string suffix = pattern.substr(0, pattern.size() - 1);
        if (text.size() < suffix.size()) return false;
        return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    // Wildcard match: supports multiple '*' (e.g. "*foo*bar*", "a*b*c", "*foo", "foo*")
    // P2-11: Replaced single-star-only logic with proper multi-star glob matching.
    size_t star = pattern.find('*');
    if (star != std::string::npos) {
        // Split pattern into segments separated by '*'
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
        // Match segments in order
        size_t textPos = 0;
        for (size_t i = 0; i < segs.size(); i++) {
            const std::string& seg = segs[i];
            if (seg.empty()) continue;
            size_t found = text.find(seg, textPos);
            if (found == std::string::npos) return false;
            // First segment must match at start if pattern doesn't start with '*'
            if (i == 0 && pattern[0] != '*' && found != 0) return false;
            // Last segment must match at end if pattern doesn't end with '*'
            if (i == segs.size() - 1 && pattern.back() != '*' &&
                found + seg.size() != text.size()) return false;
            textPos = found + seg.size();
        }
        return true;
    }
    // Backward compatible: substring match for plain patterns
    return text.find(pattern) != std::string::npos;
}

// Detect game-related thread names by substring matching.
// Used in heuristic classification to prevent busy game workers (avgRun>5000us)
// from being misclassified as S4_THROUGHPUT, which has weak big-core pool preference.
// S3_GAME has strong BIG=50/MID=20/LITTLE=-40 pool preference, ensuring game
// threads are pushed to performance cores instead of drifting to idle little cores.
//
// the old list contained the bare token "AI", which is a substring
// of "mtkPowerAIDL" / "PowerAIDL" / "HAL-AIDL" etc. — on the real Dimensity 1100
// device the MTK power HAL binder worker was misclassified as S3_GAME and then
// aggressively migrated/RT-boosted, destabilizing system power management. We now:
//   * match short tokens ("AI", "GPU", "Gfx") only as whole word-boundary tokens,
//   * explicitly treat Binder/HwBinder/AIDL/vendor binder threads as SYSTEM
//     (see isBinderThreadName), and those are checked FIRST so they win.
static bool isGameThreadName(const std::string& tname) {
    // Whole-word tokens: match when the name contains the token bounded by a
    // non-alphanumeric (or string) edge, e.g. "UnityMain" still matches "Unity"
    // via the prefix rule below, but "mtkPowerAIDL" must NOT match "AI".
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
    // Short ambiguous tokens require whole-word match only.
    if (wordMatch(tname, "AI"))   return true;
    if (wordMatch(tname, "GPU"))   return true;
    if (wordMatch(tname, "Gfx"))   return true;
    return false;
}

// vendor / framework binder threads must be SYSTEM (S0), never GAME.
// On the real device ~512 tracked threads are mostly vendor HwBinder/Binder
// services; the old name heuristics let "mtkPowerAIDL" slip through as GAME_AI.
// Detects: Binder:<pid>_*, HwBinder:<pid>_*, mtkPowerAIDL, *-AIDL, Binder:*,
// and the common MTK/vendor power/telephony/audio binder worker names.
static bool isBinderThreadName(const std::string& tname, const std::string& pname) {
    if (tname.rfind("Binder:", 0) == 0) return true;
    if (tname.rfind("HwBinder:", 0) == 0) return true;
    if (tname.rfind("binder:", 0) == 0) return true;
    if (tname.find("AIDL") != std::string::npos) return true;
    // MTK power/telephony/audio HAL binder worker threads observed on the device.
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

// P2-4: forward declaration — readThreadStarttime() is defined later in this file
// (after scanThreadsOfPid), but classifyThread() needs it for the cache key
// (tid + starttime + tname + pname) to prevent TID-reuse cross-contamination.
static unsigned long long readThreadStarttime(int pid, int tid);

ThreadRole classifyThread(int pid, int tid, const std::string& tname, const std::string& pname, int& outPriority, std::string& outSubtype, int* outConfidence, unsigned long long cachedStarttime) {
    // P2-4: cache key now includes tid and starttime to prevent two different
    // threads (same name+process but different tid, or tid reuse after exit)
    // from sharing classification results. classifyThreadImpl() reads dynamic
    // schedstat/runtime/switches data — different threads with the same name
    // can have very different behaviors. starttime (from /proc/<tid>/stat field 22)
    // prevents TID reuse: a new thread on the same tid has a different starttime.
    // Falls back to tid|tname|pname if starttime cannot be read.
    //
    // P2-8: when the caller already has starttime cached in ThreadInfo (steady-state
    // populateNewThread / TID-reuse path), pass it in via cachedStarttime and SKIP
    // the /proc/<pid>/task/<tid>/stat open+read entirely. On a cache HIT within the
    // 5s TTL this saves one proc I/O per call. The eBPF onExec path passes 0
    // (no cached value) and still reads /proc once for the cache key.
    unsigned long long st = (cachedStarttime != 0) ? cachedStarttime : readThreadStarttime(pid, tid);
    std::string cacheKey;
    if (st != 0) {
        cacheKey = std::to_string(tid) + "|" + std::to_string(st) + "|" + tname + "|" + pname;
    } else {
        cacheKey = std::to_string(tid) + "|" + tname + "|" + pname;
    }
    // Cache TTL remains 5s (unchanged). classifyThreadImpl() reads /proc schedstat
    // which is expensive; role changes slowly so 5s cache is safe.
    static std::unordered_map<std::string, std::tuple<ThreadRole, int, std::string, int, long long>> s_classifyCache;
    static std::mutex s_classifyCacheMutex;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // MON-07: TTL dropped from 5s to 2s so stale classifications expire faster;
    // the hard cap is bumped from 256 to 1024 to cover the ~400-600 threads that
    // modern Android devices track (256 was capping below the working set, causing
    // constant cache misses and re-reads of /proc/<tid>/stat + schedstat).
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
    // #29/#30: confidence scoring. Start at 0; each supporting feature adds weight.
    int conf = 0;
    // #29: binder/vendor system threads detected FIRST so they win over game-name
    // heuristics (the old "AI" substring false-positive on mtkPowerAIDL).
    if (isBinderThreadName(tname, pname)) {
        outPriority = thread_classify::kDefaultPriority;
        outSubtype = "VENDOR_BINDER_SYSTEM";
        if (outConfidence) *outConfidence = 85;
        return ROLE_S0_SYSTEM;
    }
    // ROUND-014 P3: delegate thread-name role recognition to G4's unified truth
    // (aatped_threadutil) — single source, shared with frame layer-picker. The old
    // bare endsWith(".ui"/".gpu") matched arbitrary self-named threads (e.g.
    // AppDebug.ui). The tight regex ^[0-9]+\.(ui|raster|gpu|platform)$ and the
    // render whitelist now live there.
    // ROUND-014 P3: blink/v8 renderer is a UI-side thread (S1), NOT the GPU main (S2).
    // The render whitelist lumps both; split CrRendererMain out before the generic S2.
    if (tname == "CrRendererMain") {
        outSubtype = "WEBVIEW_RENDERER"; if (outConfidence) *outConfidence=85; return ROLE_S1_UI;
    }
    if (aatpe::isRenderWhitelisted(tname.c_str())) {
        outSubtype = "RENDER_WHITELISTED"; if (outConfidence) *outConfidence=85; return ROLE_S2_RENDER;
    }
    if (aatpe::isFlutterEngineThread(tname.c_str())) {
        std::string s = tname;
        // .raster AND .gpu are the rendering side; .ui/.platform are the UI side.
        if (s.size() >= 7 && s.compare(s.size()-7, 7, ".raster")==0) {
            outSubtype = "FLUTTER_RASTER"; if (outConfidence) *outConfidence=85; return ROLE_S2_RENDER;
        }
        if (s.size() >= 4 && s.compare(s.size()-4, 4, ".gpu")==0) {
            outSubtype = "FLUTTER_GPU"; if (outConfidence) *outConfidence=85; return ROLE_S2_RENDER;
        }
        outSubtype = "FLUTTER_UI"; if (outConfidence) *outConfidence=85; return ROLE_S1_UI;
    }
    // R3-003: read g_rules under the shared side of g_rulesMutex so a concurrent
    // loadThreadRules() swap can't free the vector we're iterating.
    {
        std::shared_lock<std::shared_mutex> rl(g_rulesMutex);
        for (const auto& r : g_rules) {
            bool procMatch = matchPattern(pname, r.procPattern);
            bool threadMatch = matchPattern(tname, r.threadPattern);
            if (procMatch && threadMatch) {
                outPriority = r.priority;
                outSubtype = r.subtype;
                // Explicit admin rule = high confidence; tighten if only the thread
                // name matched but the process pattern was a loose "*" wildcard.
                conf = (r.procPattern == "*") ? 80 : 90;
                if (outConfidence) *outConfidence = conf;
                return r.role;
            }
        }
    }
    outPriority = thread_classify::kDefaultPriority;
    outSubtype = "unknown";
    // Defense-in-depth for "main" and "RenderThread".
    // These names skip the schedstat heuristic below (legacy guard), but if
    // rules failed to load they would fall through to S5_BACKGROUND (inverse
    // capacity → little cores). Provide sensible fallback classification.
    if (tname == "main") { outSubtype = "UI_MAIN_FALLBACK"; if (outConfidence) *outConfidence = 75; return ROLE_S1_UI; }
    if (tname == "RenderThread") { outSubtype = "RENDER_FALLBACK"; if (outConfidence) *outConfidence = 75; return ROLE_S2_RENDER; }
    if (tname != "main" && tname != "RenderThread") {
        std::string schedstatPath = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/schedstat";
        std::ifstream f(schedstatPath);
        if (f) {
            unsigned long long runTime = 0, waitTime = 0, nrSwitches = 1;
            f >> runTime >> waitTime >> nrSwitches;
            // P2-16: Check stream state after extraction. If schedstat content
            // was malformed (partial read), classify as BACKGROUND to avoid
            // using garbage avgRun values.
            if (f.fail()) { if (outConfidence) *outConfidence = 20; return ROLE_S5_BACKGROUND; }
            if (nrSwitches == 0) nrSwitches = 1;
            unsigned long long avgRun = runTime / nrSwitches / 1000;
            // Game-related thread names with high avgRun
            // must be S3_GAME, NOT S4_THROUGHPUT.
            if (isGameThreadName(tname) && avgRun > thread_classify::kGameAvgRunUs) {
                outSubtype = "GAME_HEURISTIC";
                if (outConfidence) *outConfidence = 70;
                return ROLE_S3_GAME;
            }
            if (avgRun > thread_classify::kThroughputAvgRunUs) {
                if (outConfidence) *outConfidence = 55;
                return ROLE_S4_THROUGHPUT;
            }
            // G5-003: ORDER-SENSITIVE. The high-util-worker -> GAME decision must be
            // evaluated BEFORE the generic UI branch. Thresholds are
            //   kUiAvgRunUs(500) < kGameAvgRunUs(2000) < kThroughputAvgRunUs(5000).
            // The old order tested UI first (avgRun>500 && switches>100), so an
            // unnamed game worker running 2-5ms/slice with >100 switches was
            // classified S1_UI and never reached the GAME fallback below. Now any
            // avgRun > 2000us (and <=5000us) is treated as a high-util worker.
            if (avgRun > thread_classify::kGameAvgRunUs) {
                if (outConfidence) *outConfidence = 45;  // lower confidence: name unknown
                return ROLE_S3_GAME;
            }
            // Genuine UI threads run short slices (<=2000us) but switch often.
            if (avgRun > thread_classify::kUiAvgRunUs && nrSwitches > thread_classify::kUiMinSwitches) {
                if (outConfidence) *outConfidence = 55;
                return ROLE_S1_UI;
            }
        }
    }
    // Fallback background: low confidence — placement layer will not aggressively
    // migrate these (it keeps conservative heuristic placement).
    if (outConfidence) *outConfidence = 30;
    return ROLE_S5_BACKGROUND;
}


// P0: forward declaration — readThreadStarttime() is defined after
// scanThreadsOfPid(), but scanThreadsOfPid() now calls it (foreground thread
// TID-reuse protection). Without this forward declaration, NDK compile fails.
static unsigned long long readThreadStarttime(int pid, int tid);

// ============================================================================
// incremental thread scan.
//
// The previous scanThreads() rebuilt g_threads from scratch every 5s, opening
// comm + schedstat + (classify's own schedstat) + currentCpu + nice + starttime
// for EVERY tracked thread (~6 open()/readdir per thread x ~200 threads). Most
// threads are unchanged between scans. We now diff the live /proc enumeration
// against the previous g_threads:
//   - NEW tid  -> full populate (comm, classify, schedstat, cpu, nice, starttime)
//   - EXISTING -> refresh ONLY the dynamic metrics the scheduler consumes
//                 (runTime/nrSwitches from schedstat) and guard TID-reuse via
//                 starttime. Static fields (tname/role/priority/nice) are kept.
//   - GONE tid -> removed.
// Dynamic scheduling inputs stay fresh (no regression); per-thread /proc reads
// drop from ~6 to ~1-2 in the steady state.
// ============================================================================
enum class RefreshResult { KEEP, REUSE, GONE };

// TID-reuse protection for the auxiliary per-thread state tables.
// g_threads already guards identity with starttime, but g_lastMigrationTime,
// g_migFailCooldown/Count/Cpu and g_unmigratableThreads are keyed by the bare
// int tid. When a thread exits, scanThreads drops it from g_threads but leaves
// these aux entries behind; once the kernel recycles the tid for a NEW thread,
// that new thread would inherit the old migration history / cooldown /
// "unmigratable" mark. On every (re)insertion of a thread we clear the aux
// records for that tid. For a genuinely new tid the erases are no-ops; for a
// recycled tid they drop the stale state. Mutexes mirror placement.cpp.
static void invalidateAuxThreadState(int tid) {
    if (tid <= 0) return;
    // P0-6: g_lastMigrationTime is mutated under g_lastMigMutex by placement.cpp;
    // the erase must take the same lock (was a bare map erase racing the writers).
    {
        std::lock_guard<std::mutex> lk(g_lastMigMutex);
        g_lastMigrationTime.erase(tid);
    }
    {
        std::lock_guard<std::mutex> lk(g_migFailMutex);
        g_migFailCooldown.erase(tid);
        g_migFailCount.erase(tid);
        g_migFailCpu.erase(tid);
        g_migFailBackoffUntilMs.erase(tid);  // P3-52: drop backoff on thread exit
    }
    {
        std::lock_guard<std::mutex> lk(g_unmigratableMutex);
        g_unmigratableThreads.erase(tid);
    }
    // #34: also drop the hysteresis settled point so a recycled tid does not
    // inherit the previous thread's anchored CPU/score.
    placementInvalidateSettle(tid);
    // RC6 (B2): reap the per-tid uclamp floor ledger. invalidateAuxThreadState is the
    // chokepoint for both thread death (vanished/evicted) and tid recycle (starttime
    // changed); clearing the ledger here stops a stale floor from being inherited via
    // max() by a new thread that reuses the tid. (The starttime guard itself is the
    // existing refreshTrackedThread REUSE detection above.)
    aatpe_mech::aatpeUclampFloorReap(tid);
}

// Fully populate a ThreadInfo for a newly-seen thread. Returns false if the
// thread must be skipped (system thread, bad schedstat, no current cpu).
static bool populateNewThread(int pid, int tid, const std::string& pname,
                              const std::string& tname, bool ignoreSys,
                              ThreadInfo& out) {
    out.pid = pid;
    out.tid = tid;
    out.pname = pname;
    out.tname = tname;
    // P2-8: read starttime ONCE here and reuse it both for the classifyThread
    // cache key (passed in as cachedStarttime) and for ThreadInfo.starttime.
    // Previously this was read twice: once inside classifyThread() and once
    // below — two open()+read() of /proc/<pid>/task/<tid>/stat per new thread.
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
    // R3-016: schedstat's middle field (time spent waiting on a runqueue) must be
    // consumed to reach nrSwitches, but it is not used for placement. Name it as a
    // throwaway so it is not mistaken for a live variable.
    unsigned long long schedWaitField = 0;
    sf >> out.runTime >> schedWaitField >> out.nrSwitches;
    (void)schedWaitField;
    if (sf.fail()) return false;  // P2: truncated/garbage schedstat
    if (out.nrSwitches == 0) return false;
    out.currentCpu = readThreadCurrentCpu(pid, tid);
    if (out.currentCpu < 0) return false;
    out.nice = readThreadNiceValue(pid, tid);
    // out.starttime already set above (P2-8).
    return true;
}

// P2-17+19: Tiered scan tuning.
//   kIdleSkipThreshold — after this many consecutive refreshes with NO change
//     in nrSwitches, the thread is considered idle and we skip the schedstat
//     open() on subsequent cycles (use cached runTime/nrSwitches). At 5s
//     scan interval this is ~30s of no switches.
//   kLongIdleRescanPeriod — even for long-idle threads, force one schedstat
//     read every N cycles so we detect when the thread wakes up. At 5s this
//     is ~50s worst-case detection latency for a previously-idle thread.
static constexpr int kIdleSkipThreshold   = 6;
static constexpr int kLongIdleRescanPeriod = 10;

// Refresh dynamic metrics for an already-tracked thread. Returns:
//   KEEP   -> values refreshed (or transient read error: keep last good values)
//   REUSE  -> starttime changed (TID recycled): caller must fully repopulate
// checkStarttime: when false, skip the /proc/<tid>/stat starttime read entirely
//   (caller staggers these across cycles to reduce proc I/O). The TID-reuse
//   backstop is the eBPF onExit/onFork event path; this periodic check is
//   only a safety net and does not need to run every cycle for every thread.
static RefreshResult refreshTrackedThread(ThreadInfo& ti, bool checkStarttime) {
    // a placeholder inserted by onFork() (starttime==0, empty comm,
    // role=S5_BACKGROUND) is flagged needsHydration. The old TID-reuse guard below
    // only fired when BOTH starttimes were nonzero and differed, so a fork
    // placeholder (starttime==0) silently never got fully populated. Force a full
    // repopulate on the first scan that sees such a placeholder.
    if (ti.needsHydration) {
        return RefreshResult::REUSE;
    }

    // rel15 大小核颠倒 fix: wake-up evidence from eBPF. markThreadScoreDirty()
    // keeps lastSwitchMs fresh on every real sched_switch. If the thread had been
    // frozen by skipSchedstat (idleRefreshes >= threshold) but its lastSwitchMs
    // is still recent (< kInactiveMs), it just woke up -> reset the idle-tier
    // bookkeeping so we fall through and read schedstat this cycle, instead of
    // waiting up to kLongIdleRescanPeriod calibration cycles for the forced
    // rescan. We key off lastSwitchMs freshness (not a separate wakeHint flag)
    // because shouldRescoreThread consumes wakeHint on the very next main-loop
    // turn (~200ms) -- long before the 60s calibration scan -- so relying on it
    // would lose the freeze-break. A fresh lastSwitchMs IS the wake evidence.
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

    // P2-17+19: long-idle tier. If the thread has been idle for >= threshold
    // refreshes and we are NOT due for a rescan this cycle, skip the schedstat
    // open() entirely and keep cached metrics. The eBPF sched_switch path keeps
    // currentCpu fresh in real time; runTime/nrSwitches are only used for
    // placement scoring and an idle thread is not being migrated anyway.
    bool skipSchedstat = (ti.idleRefreshes >= kIdleSkipThreshold) &&
                         (ti.longIdleRescan < kLongIdleRescanPeriod);
    if (skipSchedstat) {
        ti.longIdleRescan++;
        return RefreshResult::KEEP;
    }
    // Due for a rescan (or not yet idle): read schedstat.
    ti.longIdleRescan = 0;

    std::string schedstatPath = "/proc/" + std::to_string(ti.pid) + "/task/" +
                                 std::to_string(ti.tid) + "/schedstat";
    std::ifstream sf(schedstatPath);
    if (!sf) return RefreshResult::KEEP;  // transient: keep last good values
    // R3-017: middle schedstat field consumed but unused — explicitly discarded.
    unsigned long long schedWaitField = 0, runTime = 0, nrSwitches = 0;
    sf >> runTime >> schedWaitField >> nrSwitches;
    (void)schedWaitField;
    if (sf.fail()) return RefreshResult::KEEP;
    if (nrSwitches == 0) return RefreshResult::KEEP;

    // Track idle tier: if nrSwitches did not change since last refresh, the
    // thread made no context switches -> idle. Otherwise it's active.
    if (nrSwitches == ti.nrSwitches) {
        ti.idleRefreshes++;
    } else {
        ti.idleRefreshes = 0;
    }

    // fix65 TID-reuse guard: a changed starttime means a recycled tid.
    // P2-17+19: only read /proc/<tid>/stat when checkStarttime is true (caller
    // staggers these across cycles). When false, trust the cached starttime.
    if (checkStarttime) {
        unsigned long long st = readThreadStarttime(ti.pid, ti.tid);
        if (st != 0 && ti.starttime != 0 && st != ti.starttime) return RefreshResult::REUSE;
    }
    // R005c: derive switches/sec from (nrSwitches_delta / elapsed_sec) so the
    // hybrid feature f[2] is a RATE (a chattery thread scores high; a long-lived
    // but currently-quiet thread scores ~0). Reset on first observation or tid
    // reuse (nrSwitches went backwards).
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
// C-01: Persist thread info to thread_db for CLI access.
// P2-9+18: Atomic single-file snapshot (thread_db.json) instead of a directory
// of ~512 per-tid key=value files. The old approach did:
//   opendir + readdir + unlink(every file)  -> ~500 unlink()
//   open + write + close per thread          -> ~500 open/write/close
// every 5s. The new approach does ONE open(tmp) + write(all) + fsync + close +
// rename: ~5 syscalls per 5s cycle, and readers always see a consistent
// snapshot (rename is atomic on ext4/overlayfs).
//
// We also unlink any stale per-tid files from the old format on the first call
// after upgrade, so leftover <tid>-named files don't accumulate.
// ROUND-005: repair a kernel-truncated /proc/<tid>/comm.
// comm is capped at 15 bytes. A multibyte UTF-8 name (e.g. Chinese) can be cut
// mid-sequence, leaving an invalid trailing byte that the WebUI/browser renders
// as "?". Strip any incomplete trailing UTF-8 sequence so the emitted name is
// valid UTF-8. Also drops ASCII control bytes that survived readFile.
static std::string sanitizeCommName(std::string s) {
    while (!s.empty()) {
        unsigned char b = (unsigned char)s.back();
        if (b < 0x80) break;                              // ASCII, complete
        if ((b & 0xC0) == 0xC0) { s.pop_back(); continue; } // orphan leading byte
        // back is a continuation byte: walk back to its leading byte.
        int need = 0, leadIdx = -1;
        for (int i = (int)s.size() - 1; i >= 0; --i) {
            unsigned char c = (unsigned char)s[i];
            if ((c & 0xC0) == 0x80) continue;             // continuation
            if      ((c & 0xE0) == 0xC0) need = 1;
            else if ((c & 0xF0) == 0xE0) need = 2;
            else if ((c & 0xF8) == 0xF0) need = 3;
            else need = 0;
            leadIdx = i; break;
        }
        if (leadIdx < 0) { s.pop_back(); continue; }  // continuation w/o leading
        int have = (int)s.size() - 1 - leadIdx;
        if (have == need) break;                        // complete sequence -> keep
        s.resize(leadIdx);                             // incomplete -> strip tail
    }
    return s;
}

// Test seam: expose sanitizeCommName to the unit test TU.
#ifdef AATPE_HOST_TEST
std::string threadScanSanitizeCommForTest(const std::string& in) { return sanitizeCommName(in); }
// v4.4.9 G4: expose the placeholder-hydration / TID-reuse decision for a host regression.
// Returns 0=KEEP,1=REUSE,2=GONE (enum class RefreshResult order).
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

    // One-time cleanup of legacy per-tid files from the old format. After the
    // first successful atomic write we drop a marker file; subsequent scans skip
    // the opendir() sweep entirely.
    static std::atomic<bool> s_legacyCleaned{false};
    if (!s_legacyCleaned.load(std::memory_order_acquire)) {
        DIR* dir = opendir(dbDir.c_str());
        if (dir) {
            struct dirent* e;
            while ((e = readdir(dir)) != nullptr) {
                if (e->d_name[0] == '.') continue;
                // keep thread_db.json / .tmp; remove everything else (old per-tid files)
                std::string name = e->d_name;
                if (name == "thread_db.json" || name == "thread_db.json.tmp") continue;
                std::string fpath = dbDir + "/" + name;
                unlink(fpath.c_str());
            }
            closedir(dir);
        }
        s_legacyCleaned.store(true, std::memory_order_release);
    }

    // Build the JSON payload.
    std::string json = "{\"version\":1,\"count\":" + std::to_string(threads.size()) + ",\"threads\":[";
    bool first = true;
    for (const auto& kv : threads) {
        const ThreadInfo& ti = kv.second;
        if (!first) json += ",";
        first = false;
        json += "{\"pid\":" + std::to_string(ti.pid);
        json += ",\"tid\":" + std::to_string(ti.tid);
        json += ",\"pname\":\""; jsonEscapeStr(json, ti.pname); json += "\"";
        // ROUND-006: onFork placeholders (tname empty until the next /proc scan
        // hydrates them) previously emitted an empty string, which the WebUI
        // renders as "?". Emit a deterministic placeholder instead.
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

    // Atomic write: tmp -> fsync -> rename.
    std::string finalPath = dbDir + "/thread_db.json";
    std::string tmpPath    = finalPath + ".tmp";
    // R3-012: O_CLOEXEC so the tmp fd is not leaked across fork+exec into a
    // child process (db is written on a timer thread).
    int fd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { writeFile(finalPath.c_str(), json); return; }
    size_t written = 0;
    const char* data = json.data();
    size_t len = json.size();
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;  // P1 round7: avoid infinite loop on zero write
        written += (size_t)n;
    }
    if (fsync(fd) != 0 || written != len) {
        close(fd);
        unlink(tmpPath.c_str());
        writeFile(finalPath.c_str(), json);
        return;
    }
    close(fd);
    // P3: check rename() — on failure (cross-device/perms/etc.) log and unlink
    // the tmp file so we don't leak stale .tmp files.
    if (rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
        LOG_WARN("persistThreadDb: rename %s -> %s failed errno=%d",
                 tmpPath.c_str(), finalPath.c_str(), errno);
        unlink(tmpPath.c_str());
    }
}


// (问题40): Read thread starttime from /proc/[pid]/task/[tid]/stat field 22
// This is used to detect TID reuse: if the same TID has a different starttime,
// it is a new thread and old state (cooldown/history/pending) must not be inherited.
static unsigned long long readThreadStarttime(int pid, int tid) {
    std::string statPath = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat";
    std::ifstream f(statPath);
    if (!f) return 0;
    std::string content;
    std::getline(f, content);
    if (content.empty()) return 0;
    // stat format: pid (comm) state ppid ... field 22 = starttime
    // Find the closing ')' of comm, then split remaining by spaces
    size_t rp = content.rfind(')');
    if (rp == std::string::npos) return 0;
    std::string rest = content.substr(rp + 1);
    std::istringstream iss(rest);
    std::string field;
    int idx = 0;  // field after ')' is state (index 0), starttime is field 22-3 = 19
    unsigned long long starttime = 0;
    while (iss >> field) {
        if (idx == 19) {  // field 22 in 1-based = index 19 after ')'
            try { starttime = std::stoull(field); } catch (...) { starttime = 0; }
            break;
        }
        idx++;
    }
    return starttime;
}

void scanThreads() {
    // incremental scan. We start from the PREVIOUS g_threads map
    // (copied under lock), then diff the live /proc enumeration against it:
    // existing threads only get their dynamic schedstat metrics refreshed, new
    // threads are fully classified, gone threads are removed. This preserves all
    // scheduling inputs while avoiding redundant per-thread /proc reads.
    Config scanCfg;
    snapshotConfig(scanCfg);
    int configuredMax = scanCfg.maxTrackedThreads;
    // ROUND-007c: adaptive tracking budget. Fixed 512 slots could be saturated by
    // vendor binder threads, starving the foreground game's own late threads.
    // Adapt to observed thread count when the user did NOT explicitly override.
    // clamp(observed*1.15, 512, 2048). Explicit config value always wins.
    const int kDefaultMaxTracked = 512;
    auto adaptiveBudget = [](int observed) -> int {
        long long b = (long long)(observed * 1.15);
        if (b < 512) b = 512;
        if (b > 2048) b = 2048;
        return (int)b;
    };
    int maxTracked = configuredMax;  // may be widened after we count observed below
    // Use the previous cycle's tracked count as the observed thread population.
    if (configuredMax == kDefaultMaxTracked) {
        size_t prevObserved = 0;
        { std::lock_guard<std::mutex> lk(g_threadsMutex); prevObserved = g_threads.size(); }
        maxTracked = adaptiveBudget((int)prevObserved);
    }
    bool ignoreSys = scanCfg.ignoreSystemThreads;

    // P2-17+19: global scan counter. Used to stagger starttime (/proc/stat)
    // checks across threads so we don't open+parse 500 stat files every cycle.
    // With kStarttimeStaggerPeriod=10 and ~500 threads, each thread's starttime
    // is re-checked every ~10 scans (~50s at 5s interval) instead of every scan.
    // P3: atomic — scanThreads can be reached from more than one loop; relaxed
    // order is fine (only used for a stagger cadence).
    static std::atomic<int> s_scanCounter{0};
    s_scanCounter.fetch_add(1, std::memory_order_relaxed);
    static constexpr int kStarttimeStaggerPeriod = 10;

    std::unordered_map<int, ThreadInfo> work;
    // P0-4 fix: record the TID set present at snapshot time so we can
    // distinguish "exited since snapshot" (erase from g_threads) from
    // "eBPF-inserted after snapshot" (preserve) when merging back.
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

    // Scan one pid's task dir, diffing against `work`. `isFg` selects the exact
    // filtering the old code used for foreground (valid pkg + skip system, no
    // whitelist) vs background (whitelist only).
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
            if (isSystemProcessToSkip(pid)) return;   // B-004: UID-based generic skip
        } else {
            if (!isPackageWhitelisted(pname)) return;
        }
        std::string taskDir = "/proc/" + std::to_string(pid) + "/task";
        DIR* taskDirp = opendir(taskDir.c_str());
        if (!taskDirp) {
            // MON-06 fix: opendir(taskDir) can fail transiently (SELinux denial
            // spike, pid just died, binder pressure). The old code returned early,
            // which left this pid's tids out of liveTids, and the commit path at
            // the end of the scan then ERASED every tracked thread of this pid as
            // "exited". The next scan re-populated them from scratch (full
            // reclassification storm + aux-state invalidation). Instead, preserve
            // any previously-tracked tids belonging to this pid by inserting them
            // into liveTids; they won't be refreshed this cycle but won't be
            // dropped either.
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
                // EXISTING thread: refresh dynamic metrics only.
                // P2-17+19: stagger starttime checks so not every thread opens
                // /proc/<tid>/stat every cycle. TID-reuse is also caught by the
                // eBPF onExit/onFork event path in real time; this periodic check
                // is a backstop.
                bool checkSt = ((s_scanCounter.load(std::memory_order_relaxed) + tid) % kStarttimeStaggerPeriod) == 0;
                RefreshResult r = refreshTrackedThread(it->second, checkSt);
                if (r == RefreshResult::REUSE) {
                    std::string tname = sanitizeCommName(readFile((taskDir + "/" + std::to_string(tid) + "/comm").c_str()));
                    if (tname.empty() || isKernelThread(tname, tid)) continue;
                    ThreadInfo ti;
                    if (populateNewThread(pid, tid, pname, tname, ignoreSys, ti)) {
                        // starttime changed -> recycled tid. Drop stale
                        // migration history / cooldown / unmigratable mark before
                        // the new incarnation inherits it.
                        invalidateAuxThreadState(tid);
                        it->second = ti;
                    } else {
                        // 问题7 fix: REUSE means the TID no longer refers to the
                        // original thread (starttime changed). If we cannot fully
                        // repopulate the new incarnation, keeping the old
                        // ThreadInfo would leave a stale record with the wrong
                        // starttime/role/comm in g_threads — and decidePlacement
                        // would treat it as a live, migratable thread. Erase the
                        // stale entry so the next scan re-adds it under the normal
                        // new-thread path (with cap accounting) when it is
                        // actually readable.
                        it = work.erase(it);
                        continue;
                    }
                }
                continue;
            }

            // NEW thread: respect the maxTracked cap. ROUND-007c: foreground
            // package threads are NEVER skipped when the table is full — the
            // commit-side cap loop evicts the oldest BACKGROUND victim to make
            // room, so the game's own threads are never starved by vendor binder.
            if ((int)work.size() >= maxTracked && pname != fgPkg) continue;
            std::string tname = sanitizeCommName(readFile((taskDir + "/" + std::to_string(tid) + "/comm").c_str()));
            if (tname.empty()) continue;
            if (isKernelThread(tname, tid)) continue;
            ThreadInfo ti;
            if (populateNewThread(pid, tid, pname, tname, ignoreSys, ti)) {
                // tid may be a recycled id (exited thread's aux state
                // lingers). Clear it; no-op when this is a genuinely new tid.
                invalidateAuxThreadState(tid);
                work[tid] = ti;
            }
        }
        closedir(taskDirp);
    };

    // Foreground pid first (its threads win the remaining cap slots).
    if (fgPid > 0) {
        scanPid(fgPid, /*isFg=*/true);
    }

    DIR* procDir = opendir("/proc");
    if (procDir) {
        struct dirent* procEntry;
        while ((procEntry = readdir(procDir)) != nullptr) {
            char* endptr = nullptr;
            long pidLong = strtol(procEntry->d_name, &endptr, 10);
            if (endptr == procEntry->d_name || *endptr != '\0' || pidLong <= 0 || pidLong > INT_MAX) continue;
            int pid = (int)pidLong;
            if (pid == fgPid) continue; // already scanned as foreground
            scanPid(pid, /*isFg=*/false);
        }
        closedir(procDir);
    } else {
        // R3-002: a transient /proc open failure (EIO / EMFILE / memory pressure)
        // must NOT be interpreted as "every background thread exited". liveTids at
        // this point only holds the foreground pid's threads, so the diff-drop below
        // would erase the ENTIRE background tracking table — losing hysteresis /
        // cooldown / recovery state and triggering a full reclassification storm.
        // Preserve the previous g_threads snapshot and bail this cycle.
        LOG_WARN("[SCAN] opendir(/proc) failed errno=%d; skipping this scan to preserve tracked threads", errno);
        return;
    }

    // drop threads that are no longer live.
    for (auto it = work.begin(); it != work.end(); ) {
        if (liveTids.count(it->first) == 0) it = work.erase(it);
        else ++it;
    }

    // R3-010: snapshot declared here, filled under the single critical section below.
    std::unordered_map<int, ThreadInfo> threadsCopy;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        // P0-4 fix: incremental merge instead of wholesale g_threads = std::move(work).
        // Between the snapshot at the top of this function and this commit, the eBPF
        // poll thread (onFork/onExec) may have inserted new fork/exec placeholders into
        // g_threads under g_threadsMutex. The old std::move(work) discarded those entries
        // entirely because `work` was copied before they existed.
        //
        // Merge semantics:
        //   1. Every entry in `work` (refreshed existing + newly scanned) overwrites
        //      the corresponding g_threads entry.
        //   2. TIDs that were in the original snapshot but are no longer in `work`
        //      (exited between snapshot and commit, or filtered out by liveTids) are
        //      erased from g_threads.
        //   3. TIDs NOT in the original snapshot (eBPF-inserted after snapshot) are
        //      preserved — they are live fork/exec placeholders that the next scan
        //      will hydrate via populateNewThread().
        for (auto& kv : work) {
            g_threads[kv.first] = std::move(kv.second);
        }
        for (int tid : snapshotTids) {
            if (work.find(tid) == work.end()) {
                g_threads.erase(tid);
                // P1-7: a thread that vanished since the snapshot is gone. Drop its
                // per-tid scoring aux state (s_lastWrite / g_threadMigrationCount)
                // here too — previously only the eBPF onExit path cleaned these, so
                // threads that disappeared between scans leaked entries.
                // Lock order: g_threadsMutex -> s_lastWriteMutex -> g_migCountMutex
                // (g_migCountMutex is the last lock in the documented chain).
                cleanupThreadScoringState(tid);
                // also drop the recovery record so dead tids don't
                // accumulate .rec files forever.
                aatpe_recovery::recoveryErase(tid);
            }
        }
        // Cap enforcement based on the MERGED size: eBPF may have inserted extra
        // placeholders between snapshot and commit. Evict non-protected S5_BACKGROUND
        // entries (same strategy as onFork/onExec in aatped_ebpf_agg.cpp).
        while ((int)g_threads.size() > maxTracked) {
            auto victim = g_threads.end();
            // R3-011: evict the MOST idle background thread (highest consecutive
            // idleRefreshes = least recently scheduled) instead of the arbitrary
            // first bucket — unordered_map iteration order previously picked a
            // victim essentially at random, which could evict a just-scanned active
            // background thread.
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
            // V-001: a capacity eviction is a thread leaving our tracking set, so
            // it must also drop the per-tid scoring aux state and the recovery
            // record exactly like the thread-vanished path above — otherwise an
            // evicted (and later re-forked) tid leaks stale scoring / .rec entries.
            cleanupThreadScoringState(evictTid);
            aatpe_recovery::recoveryErase(evictTid);
        }
        // R3-010: publish the persist snapshot while we already hold
        // g_threadsMutex (the previous code took a SECOND lock + deep-copied
        // g_threads a moment later). One critical section, one snapshot.
        threadsCopy = g_threads;
    }
    persistThreadDb(threadsCopy);
    writeFile((std::string(STATE_DIR) + "/tracked_threads").c_str(), (int)threadsCopy.size());
    // #30: average classifier confidence across tracked threads, for WebUI.
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
