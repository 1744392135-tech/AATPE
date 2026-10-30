#include "c4_freezer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace aatpe {
namespace c4freezer {

C4Decision decideC4Freeze(const C4DecisionInputs& in) {
    C4Decision d;

    if (!in.featureEnabled) {
        d.action = C4Action::NO_CHANGE;
        d.reason = "gate-off";
        return d;
    }

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

    if (!in.freezerAvailable) {
        d.action = C4Action::NO_CHANGE;
        d.degraded = true;
        d.reason = "no-freezer";
        return d;
    }

    if (in.alreadyFrozen) {
        d.action = C4Action::NO_CHANGE;
        d.reason = "already-frozen";
        return d;
    }

    d.action = C4Action::FREEZE;
    d.reason = "background-freeze";
    return d;
}

std::string parseV2CgroupPath(const std::string& content) {
    std::string path;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(pos, eol - pos);
        while (!line.empty() && (line.back()=='\r' || line.back()==' ' || line.back()=='\t'))
            line.pop_back();
        if (line.rfind("0::", 0) == 0) {
            std::string p = line.substr(3);
            while (!p.empty() && p.front() == '/') p.erase(p.begin());
            if (!p.empty()) { path = p; break; }
        }
        pos = eol + 1;
    }
    return path;
}

std::string buildFreezeDir(const std::string& v2Path, const std::string& mountRoot) {
    if (v2Path.empty()) return "";
    std::string root = mountRoot;
    while (!root.empty() && root.back() == '/') root.pop_back();
    return root + "/" + v2Path;
}

bool mediaSessionMentionsPlaying(const std::string& output, const std::string& pkg) {
    if (output.empty() || pkg.empty()) return false;
    std::string curPkg;
    size_t pos = 0;
    auto isHeader = [](const std::string& ln) {
        size_t i = 0;
        while (i < ln.size() && (ln[i] == ' ' || ln[i] == '\t')) i++;
        if (ln.compare(i, 7, "Session") != 0) return false;
        size_t j = i + 7;
        if (j < ln.size() && (ln[j] == ' ' || ln[j] == ':' || ln[j] == '#' ||
                              (ln[j] >= '0' && ln[j] <= '9')))
            return true;
        return false;
    };
    auto hasExactToken = [](const std::string& ln, const char* tok) {
        size_t i = 0;
        while (i < ln.size()) {
            while (i < ln.size() && (ln[i] == ' ' || ln[i] == '\t' ||
                                     ln[i] == ',' || ln[i] == ';')) i++;
            if (i >= ln.size()) break;
            size_t j = i;
            while (j < ln.size() && ln[j] != ' ' && ln[j] != '\t' &&
                   ln[j] != ',' && ln[j] != ';') j++;
            if (ln.compare(i, j - i, tok) == 0) return true;
            i = j;
        }
        return false;
    };
    while (pos < output.size()) {
        size_t eol = output.find('\n', pos);
        if (eol == std::string::npos) eol = output.size();
        std::string line = output.substr(pos, eol - pos);
        pos = eol + 1;
        if (isHeader(line)) { curPkg.clear(); continue; }
        size_t eq = line.find("package=");
        if (eq != std::string::npos) {
            std::string tok = line.substr(eq + 8);
            size_t sp = tok.find_first_of(" \t,;");
            if (sp != std::string::npos) tok = tok.substr(0, sp);
            size_t colon = tok.find(':');
            if (colon != std::string::npos) tok = tok.substr(0, colon);
            curPkg = tok;
            continue;
        }
        if (hasExactToken(line, "state=3") && curPkg == pkg) return true;
    }
    return false;
}

bool cgroupPathBelongsToUid(const std::string& v2Path, int appUid) {
    if (appUid <= 0 || v2Path.empty()) return false;
    std::string want = "uid_" + std::to_string(appUid);
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

std::string parseV1FreezerPath(const std::string& content) {
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(pos, eol - pos);
        size_t c1 = line.find(':');
        if (c1 == std::string::npos) { pos = eol + 1; continue; }
        size_t c2 = line.find(':', c1 + 1);
        if (c2 == std::string::npos) { pos = eol + 1; continue; }
        std::string ctrls = line.substr(c1 + 1, c2 - c1 - 1);
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

std::string parseV1FreezerMountPoint(const std::string& procMountsContent) {
    size_t pos = 0;
    while (pos < procMountsContent.size()) {
        size_t eol = procMountsContent.find('\n', pos);
        if (eol == std::string::npos) eol = procMountsContent.size();
        std::string line = procMountsContent.substr(pos, eol - pos);
        pos = eol + 1;
        std::vector<std::string> fields;
        size_t p = 0;
        while (p < line.size()) {
            while (p < line.size() && (line[p]==' '||line[p]=='\t'||
                                       line[p]=='\r')) p++;
            size_t s = p;
            while (p < line.size() && line[p]!=' ' && line[p]!='\t' &&
                   line[p]!='\r') p++;
            if (s < p) fields.push_back(line.substr(s, p - s));
        }
        if (fields.size() < 4) continue;
        const std::string& vfsType = fields[2];
        if (vfsType != "cgroup") continue;
        const std::string& opts = fields[3];
        bool hasFreezer = false;
        size_t q = 0;
        while (q <= opts.size()) {
            size_t comma = opts.find(',', q);
            if (comma == std::string::npos) comma = opts.size();
            std::string ctrl = opts.substr(q, comma - q);
            if (ctrl == "freezer") { hasFreezer = true; break; }
            q = comma + 1;
        }
        if (!hasFreezer) continue;
        std::string mp = fields[1];
        while (!mp.empty() && mp.back() == '/') mp.pop_back();
        if (!mp.empty()) return mp;
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
    return dir.find("/freezer/") != std::string::npos;
}

C4BucketPaths buildV1BucketPaths(const std::string& v1Root) {
    C4BucketPaths out;
    if (v1Root.empty()) return out;
    std::string root = v1Root;
    while (!root.empty() && root.back() == '/') root.pop_back();
    out.frozenDir = root + "/perf/frozen";
    out.thawedDir = root + "/perf/thawed";
    out.valid = true;
    return out;
}

bool isV1FrozenBucketDir(const std::string& dir) {
    if (dir.size() < sizeof("/perf/frozen") - 1) return false;
    return dir.compare(dir.size() - (sizeof("/perf/frozen") - 1),
                       sizeof("/perf/frozen") - 1, "/perf/frozen") == 0;
}

std::string thawedBucketFromFrozen(const std::string& frozenDir) {
    if (!isV1FrozenBucketDir(frozenDir)) return "";
    return frozenDir.substr(0, frozenDir.size() - 7) + "/thawed";
}

std::vector<int> parsePidsList(const std::string& s) {
    std::vector<int> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        std::string tok = s.substr(pos, comma - pos);
        size_t b = tok.find_first_not_of(" \t\r\n");
        size_t e = tok.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) {
            std::string num = tok.substr(b, e - b + 1);
            bool allDigits = !num.empty();
            for (char c : num) if (c < '0' || c > '9') { allDigits = false; break; }
            if (allDigits) {
                int pid = std::atoi(num.c_str());
                if (pid > 0) out.push_back(pid);
            }
        }
        pos = comma + 1;
    }
    return out;
}

std::string formatPidsList(const std::vector<int>& pids) {
    std::string s;
    for (size_t i = 0; i < pids.size(); i++) {
        if (i) s += ",";
        s += std::to_string(pids[i]);
    }
    return s;
}

bool procCgroupOnFrozenBucket(const std::string& procCgroupContent) {
    return parseV1FreezerPath(procCgroupContent) == "perf/frozen";
}

bool audioReevalDue(int bgTicks, int period) {
    if (period <= 0) period = kAudioReevalPeriod;
    if (bgTicks < 0) return false;
    if (bgTicks == 0) return true;
    return (bgTicks % period) == 0;
}

std::string serializeC4Frozen(const std::string& cgDir, const std::string& pkg,
                              int uid, const std::string& lastGame,
                              const std::vector<int>& pids) {
    std::string s = "cg=" + cgDir + "\npkg=" + pkg + "\nuid=" + std::to_string(uid) + "\n";
    if (!lastGame.empty()) s += "lg=" + lastGame + "\n";
    if (!pids.empty()) s += "pids=" + formatPidsList(pids) + "\n";
    return s;
}

C4FrozenState parseC4Frozen(const std::string& content) {
    C4FrozenState s;
    std::string cg, pkg, lg, pidsRaw; int uid = 0;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        std::string line = content.substr(pos, eol - pos);
        while (!line.empty() && (line.back()=='\r'||line.back()==' '||line.back()=='\t'))
            line.pop_back();
        if (line.rfind("cg=", 0) == 0)       cg  = line.substr(3);
        else if (line.rfind("pkg=", 0)== 0)  pkg = line.substr(4);
        else if (line.rfind("lg=", 0) == 0)  lg  = line.substr(3);
        else if (line.rfind("pids=", 0)== 0) pidsRaw = line.substr(5);
        else if (line.rfind("uid=", 0)== 0) {
            std::string v = line.substr(4);
            if (!v.empty() && std::all_of(v.begin(), v.end(),
                                          [](char c){return c>='0'&&c<='9';})) {
                uid = std::atoi(v.c_str());
            }
        }
        pos = eol + 1;
    }
    if (cg.empty() || pkg.empty() || uid <= 0) return s;
    s.cgDir = cg; s.pkg = pkg; s.uid = uid; s.lastGame = lg;
    s.pids = parsePidsList(pidsRaw);
    s.valid = true;
    return s;
}

std::string c4ChooseTarget(const std::string& lastGame,
                           const std::string& fgPkg,
                           bool fgIsGame) {
    if (fgIsGame) return fgPkg;
    if (!lastGame.empty()) return lastGame;
    return "";
}

bool c4FgIsGameSignals(bool manualGame, bool configGame, bool engineThreadGame) {
    return manualGame || configGame || engineThreadGame;
}

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

}
}

#ifndef AATPE_HOST_TEST

#include <unistd.h>
#include <dirent.h>
#include <cstdlib>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <atomic>
#include <mutex>

#include "aatpe_managers.h"

std::string aatpeExec(const std::string& cmd, const char* debugTag);
std::string readFile(const char* path);
bool writeFileAtomic(const char* path, const std::string& content);
extern const char* STATE_DIR;
namespace aatpe { namespace gamelist { bool isManualGame(const std::string& pkg); } }
bool isPackageInGameList(const std::string& pkg);
namespace aatpe { bool isGameEngineThread(const char* comm, const char* procComm); }

namespace aatpe {
namespace c4freezer {

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

#ifdef C4_FREEZER_TEST_SEAM
std::atomic<int> g_c4freezerLockDepth{0};
struct C4LockProbe {
    C4LockProbe()  { g_c4freezerLockDepth.fetch_add(1, std::memory_order_relaxed); }
    ~C4LockProbe() { g_c4freezerLockDepth.fetch_sub(1, std::memory_order_relaxed); }
};
#else
struct C4LockProbe { C4LockProbe() {} };
#endif

static std::mutex  s_stateMutex;
static bool        s_alreadyFrozen = false;
static std::string s_frozenCgDir;
static std::string s_targetPkg;
static int         s_targetUid = 0;
static std::string s_lastGame;
static bool        s_audioActive = false;
static int         s_bgTicks = -1;
static std::string s_lastAction;
static std::vector<int> s_frozenPids;

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

static std::string s_v1FreezerRoot;
static bool        s_v1FreezerProbed = false;

static C4BucketPaths s_bucketPaths;
static bool         s_bucketAvailable = false;
static bool         s_bucketProbed   = false;

static std::mutex  s_probeMutex;
static int          s_probeTickAge   = 0;
static constexpr int kProbeRescanTicks = 200;

static void probeResetLocked() {
    s_v1FreezerProbed = false;
    s_v1FreezerRoot.clear();
    s_bucketProbed = false;
    s_bucketAvailable = false;
    s_bucketPaths = C4BucketPaths();
}

#ifdef C4_FREEZER_TEST_SEAM
static std::string s_testMountsContent;
void c4FreezerTestSetMountsContent(const std::string& content) {
    std::lock_guard<std::mutex> lk(s_probeMutex);
    s_testMountsContent = content;
    probeResetLocked();
    s_probeTickAge = 0;
}
#endif

static std::string readProcMountsContent() {
#ifdef C4_FREEZER_TEST_SEAM
    if (!s_testMountsContent.empty()) return s_testMountsContent;
#endif
    FILE* f = std::fopen("/proc/mounts", "r");
    if (!f) return "";
    std::string out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

static std::string resolveV1FreezerRootUnlocked() {
    if (s_v1FreezerProbed) return s_v1FreezerRoot;
    s_v1FreezerProbed = true;

    std::string mp = parseV1FreezerMountPoint(readProcMountsContent());
    if (!mp.empty()) {
        struct stat st;
        if (::stat(mp.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            s_v1FreezerRoot = mp;
            return s_v1FreezerRoot;
        }
    }
    std::string conv = cgroupMountRoot() + "/freezer";
    struct stat st;
    if (::stat(conv.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        s_v1FreezerRoot = conv;
    }
    return s_v1FreezerRoot;
}

static std::string resolveV1FreezerRoot() {
    std::lock_guard<std::mutex> lk(s_probeMutex);
    return resolveV1FreezerRootUnlocked();
}

static bool freezerControllerPresent() {
    FILE* f = std::fopen((cgroupMountRoot() + "/cgroup.controllers").c_str(), "r");
    if (!f) return false;
    char buf[256] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf)-1, f);
    std::fclose(f);
    return n > 0 && std::strstr(buf, "freezer") != nullptr;
}

static bool freezeNodeWritable(const std::string& dir) {
    if (dir.empty()) return false;
    std::string p = dir + "/cgroup.freeze";
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    return ::access(p.c_str(), W_OK) == 0;
}

static bool v1FreezerPresent() {
    return !resolveV1FreezerRoot().empty();
}

static void probeHeartbeat() {
    std::lock_guard<std::mutex> lk(s_probeMutex);
    if (++s_probeTickAge >= kProbeRescanTicks) {
        s_probeTickAge = 0;
        probeResetLocked();
    }
}

static bool v1FreezeNodeWritable(const std::string& dir) {
    if (dir.empty()) return false;
    std::string p = dir + "/freezer.state";
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    return ::access(p.c_str(), W_OK) == 0;
}

static std::string findTargetV1CgroupDir(int appUid) {
    if (appUid <= 0) return "";
    std::string root = resolveV1FreezerRoot();
    if (root.empty()) return "";
    std::string dir = root + "/uid_" + std::to_string(appUid);
    struct stat st;
    if (::stat(dir.c_str(), &st) != 0) return "";
    if (!S_ISDIR(st.st_mode)) return "";
    return dir;
}

static std::string readSmallFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return "";
    char buf[256] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf)-1, f);
    std::fclose(f);
    return std::string(buf, n);
}

static bool v1WriteFreeze(const std::string& dir, bool freeze) {
    std::string p = dir + "/freezer.state";
    FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return false;
    const char* val = freeze ? "FROZEN" : "THAWED";
    int rc = std::fprintf(f, "%s\n", val);
    int ce = std::fclose(f);
    if (rc < 1 || ce != 0) return false;

    std::string want = freeze ? "FROZEN" : "THAWED";
    for (int i = 0; i < 30; i++) {
        usleep(i == 0 ? 2000 : 10000);
        std::string state = parseV1FreezerState(readSmallFile(p));
        if (state == want) return true;
        if (state == "FREEZING" || state == "THAWING") continue;
    }
    return false;
}

static bool writeFreezeV2(const std::string& dir, int val) {
    std::string p = dir + "/cgroup.freeze";
    FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return false;
    int rc = std::fprintf(f, "%d", val);
    int ce = std::fclose(f);
    return (rc >= 1 && ce == 0);
}

static bool applyFreezeState(const std::string& dir, bool freeze) {
    if (isV1FreezerDir(dir)) return v1WriteFreeze(dir, freeze);
    return writeFreezeV2(dir, freeze ? 1 : 0);
}

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

static int procUid(int pid) {
    std::string p = "/proc/" + std::to_string(pid) + "/status";
    FILE* f = std::fopen(p.c_str(), "r");
    if (!f) return -1;
    char line[256]; int uid = -1;
    while (fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "Uid:", 4) == 0) {
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
        if (procUid(pid) != appUid) continue;
        std::string cf = std::string("/proc/") + de->d_name + "/cgroup";
        FILE* f = std::fopen(cf.c_str(), "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            std::string l = line;
            if (l.rfind("0::", 0) == 0) {
                std::string p = l.substr(3);
                while (!p.empty() && p.front()=='/') p.erase(p.begin());
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

static bool audioActiveNow(const std::string& pkg) {
    std::string out = aatpeExec("dumpsys media_session", "c4freezer-media");
    if (out.empty()) return true;
    return mediaSessionMentionsPlaying(out, pkg);
}

#ifdef C4_FREEZER_TEST_SEAM
static std::string s_testAutoGamePkg;
static void flushAutoGameCache();
void c4FreezerTestSetAutoGamePkg(const std::string& pkg) {
    s_testAutoGamePkg = pkg;
    flushAutoGameCache();
}
#endif

static bool fgHasGameEngineThreads(const std::string& pkg) {
    if (pkg.empty()) return false;
#ifdef C4_FREEZER_TEST_SEAM
    if (!s_testAutoGamePkg.empty()) return pkg == s_testAutoGamePkg;
#else
    std::string out = aatpeExec(("pidof " + pkg).c_str(), "c4freezer-autogame");
    if (out.empty()) return false;
    size_t pos = 0;
    while (pos < out.size()) {
        while (pos < out.size() &&
               (out[pos]==' ' || out[pos]=='\n' || out[pos]=='\t' || out[pos]=='\r')) pos++;
        size_t sp = pos;
        while (pos < out.size() && out[pos] >= '0' && out[pos] <= '9') pos++;
        if (sp == pos) break;
        int pid = std::atoi(out.substr(sp, pos - sp).c_str());
        if (pid <= 0) continue;
        std::string taskDir = "/proc/" + std::to_string(pid) + "/task";
        DIR* d = ::opendir(taskDir.c_str());
        if (!d) continue;
        struct dirent* de;
        bool hit = false;
        while ((de = ::readdir(d)) != nullptr) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
            std::string commPath = taskDir + "/" + de->d_name + "/comm";
            FILE* f = std::fopen(commPath.c_str(), "re");
            if (!f) continue;
            char comm[256] = {0};
            size_t n = std::fread(comm, 1, sizeof(comm) - 1, f);
            std::fclose(f);
            if (n == 0) continue;
            while (n > 0 && (comm[n-1]=='\n' || comm[n-1]=='\r' ||
                             comm[n-1]==' '  || comm[n-1]=='\t')) {
                comm[n-1] = '\0'; n--;
            }
            if (aatpe::isGameEngineThread(comm, nullptr)) { hit = true; break; }
        }
        ::closedir(d);
        if (hit) return true;
    }
#endif
    return false;
}

static std::string s_autoCachePkg;
static bool        s_autoCacheResult = false;
#ifdef C4_FREEZER_TEST_SEAM
static void flushAutoGameCache() { s_autoCachePkg.clear(); s_autoCacheResult = false; }
#endif
static bool fgHasGameEngineThreadsCached(const std::string& pkg) {
    if (pkg == s_autoCachePkg) return s_autoCacheResult;
    s_autoCachePkg    = pkg;
    s_autoCacheResult = fgHasGameEngineThreads(pkg);
    return s_autoCacheResult;
}

#ifdef C4_FREEZER_TEST_SEAM
static std::vector<int> s_testPidsForUid;
static bool             s_testPidsForUidInjected = false;
static std::string      s_testProcCgroupContent;
static int              s_testReadbackFailAfter = 0;
static bool             s_testFailThawedWrites = false;
void c4FreezerTestSetPidsForUid(const std::vector<int>& pids) {
    s_testPidsForUid = pids;
    s_testPidsForUidInjected = true;
}
void c4FreezerTestSetProcCgroupContent(const std::string& cg) {
    s_testProcCgroupContent = cg;
}
void c4FreezerTestSetReadbackFailAfter(int n) {
    s_testReadbackFailAfter = n;
}
void c4FreezerTestFailThawedWrites(bool f) {
    s_testFailThawedWrites = f;
}
#endif
static std::vector<int> enumPidsForUid(int appUid) {
    std::vector<int> out;
    if (appUid <= 0) return out;
#ifdef C4_FREEZER_TEST_SEAM
    if (s_testPidsForUidInjected) return s_testPidsForUid;
#endif
    DIR* proc = ::opendir("/proc");
    if (!proc) return out;
    struct dirent* de;
    while ((de = ::readdir(proc)) != nullptr) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int pid = std::atoi(de->d_name);
        if (pid <= 0) continue;
        if (procUid(pid) != appUid) continue;
        out.push_back(pid);
    }
    ::closedir(proc);
    return out;
}

static bool bucketLooksReady(const C4BucketPaths& bp) {
    struct stat st;
    if (::stat(bp.frozenDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (::stat(bp.thawedDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (parseV1FreezerState(readSmallFile(bp.frozenDir + "/freezer.state")) != "FROZEN")
        return false;
    if (parseV1FreezerState(readSmallFile(bp.thawedDir + "/freezer.state")) != "THAWED")
        return false;
    if (::access((bp.frozenDir + "/cgroup.procs").c_str(), W_OK) != 0) return false;
    if (::access((bp.thawedDir + "/cgroup.procs").c_str(), W_OK) != 0) return false;
    return true;
}

static bool pathHasNoSymlink(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    return !S_ISLNK(st.st_mode);
}

static bool pathComponentsNoSymlink(const std::string& path) {
    std::string cur;
    size_t i = 0;
    if (!path.empty() && path.front() == '/') { cur = "/"; i = 1; }
    while (i <= path.size()) {
        size_t sl = path.find('/', i);
        if (sl == std::string::npos) sl = path.size();
        if (sl > i) {
            cur += path.substr(i, sl - i);
            if (!pathHasNoSymlink(cur)) return false;
        }
        if (sl < path.size()) cur += '/';
        i = sl + 1;
    }
    return true;
}

static bool provisionBucket(const std::string& dir, const char* state) {
    if (!pathComponentsNoSymlink(dir)) return false;
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    if (!pathHasNoSymlink(dir)) return false;
    std::string p = dir + "/freezer.state";
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) return false;
    std::string s = std::string(state) + "\n";
    ssize_t w = ::write(fd, s.c_str(), s.size());
    ::close(fd);
    if (w != (ssize_t)s.size()) return false;
    std::string procs = dir + "/cgroup.procs";
    int pf = ::open(procs.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (pf >= 0) ::close(pf);
    return true;
}

static bool probeV1Bucket() {
    std::lock_guard<std::mutex> lk(s_probeMutex);
    if (s_bucketProbed) return s_bucketAvailable;
    s_bucketProbed = true;
    std::string root = resolveV1FreezerRootUnlocked();
    C4BucketPaths bp = buildV1BucketPaths(root);
    if (!bp.valid) return false;
    if (!bucketLooksReady(bp)) {
        if (!root.empty() && ::access(root.c_str(), W_OK) == 0) {
            provisionBucket(bp.frozenDir, "FROZEN");
            provisionBucket(bp.thawedDir, "THAWED");
        }
    }
    if (!bucketLooksReady(bp)) return false;
    s_bucketPaths = bp;
    s_bucketAvailable = true;
    return true;
}

#ifdef C4_FREEZER_TEST_SEAM
void c4FreezerTestFlushCaches() {
    std::lock_guard<std::mutex> lk(s_probeMutex);
    probeResetLocked();
    s_probeTickAge = 0;
}
#endif

static bool pidOwnerMatches(int pid, int targetUid) {
#ifdef C4_FREEZER_TEST_SEAM
    (void)pid; (void)targetUid;
    return true;
#else
    return procUid(pid) == targetUid;
#endif
}

static bool writePidToBucket(const std::string& procsPath, int pid) {
#ifdef C4_FREEZER_TEST_SEAM
    if (s_testFailThawedWrites && procsPath.find("/thawed/") != std::string::npos)
        return false;
#endif
    FILE* f = std::fopen(procsPath.c_str(), "w");
    if (!f) return false;
    int rc = std::fprintf(f, "%d\n", pid);
    int ce = std::fclose(f);
    return rc >= 1 && ce == 0;
}

static bool bucketMovePids(const std::string& frozenDir, bool freeze,
                           int targetUid, std::vector<int>* pids) {
    if (!pids) return false;
    std::string destDir = freeze ? frozenDir : thawedBucketFromFrozen(frozenDir);
    if (destDir.empty()) return false;

    std::vector<int> candidates = enumPidsForUid(targetUid);
    std::vector<int> live;
    for (int pid : candidates) {
        if (pidOwnerMatches(pid, targetUid)) live.push_back(pid);
    }
    if (live.empty()) return false;

    if (!freeze) {
        std::string procsPath = destDir + "/cgroup.procs";
        bool any = false;
        for (int pid : live) any |= writePidToBucket(procsPath, pid);
        return any;
    }

    std::string procsPath = frozenDir + "/cgroup.procs";
    std::string thawedProcs = thawedBucketFromFrozen(frozenDir) + "/cgroup.procs";
    std::vector<int> verified;
    std::vector<int> moved;
    bool ok = true;
    for (int pid : live) {
        if (!writePidToBucket(procsPath, pid)) { ok = false; break; }
        moved.push_back(pid);
#ifdef C4_FREEZER_TEST_SEAM
        if (s_testReadbackFailAfter > 0 &&
            (int)verified.size() >= s_testReadbackFailAfter) {
            ok = false; break;
        }
#endif
        bool landed = false;
        for (int i = 0; i < 5 && !landed; i++) {
            if (i) usleep(2000);
            std::string cg;
#ifdef C4_FREEZER_TEST_SEAM
            if (!s_testProcCgroupContent.empty()) cg = s_testProcCgroupContent;
            else
#endif
                cg = readSmallFile("/proc/" + std::to_string(pid) + "/cgroup");
            landed = procCgroupOnFrozenBucket(cg);
        }
        if (!landed) { ok = false; break; }
        verified.push_back(pid);
    }
    if (!ok) {
        std::vector<int> stuck;
        for (int pid : moved) {
            if (!writePidToBucket(thawedProcs, pid)) stuck.push_back(pid);
        }
        if (!stuck.empty()) {
            *pids = stuck;
            return true;
        }
        return false;
    }
    *pids = verified;
    return true;
}

static bool applyFreezeSmart(const std::string& dir, bool freeze,
                             int targetUid, std::vector<int>* pids) {
    if (isV1FrozenBucketDir(dir)) {
        return bucketMovePids(dir, freeze, targetUid, pids);
    }
    return applyFreezeState(dir, freeze);
}

void c4FreezerTick() {
    if (!c4FreezerEnabled()) return;
    probeHeartbeat();

    static bool s_loaded = false;
    C4FrozenState restored;
    bool haveRestore = false;
    if (!s_loaded) {
        s_loaded = true;
        restored = parseC4Frozen(readFile(c4StateFilePath().c_str()));
        haveRestore = restored.valid;
    }

    std::string fg = ForegroundManager::instance().getPackage();
    bool manualGame = !fg.empty() && gamelist::isManualGame(fg);
    bool configGame = !fg.empty() && isPackageInGameList(fg);
    bool engineGame = !fg.empty() && fgHasGameEngineThreadsCached(fg);
    bool fgIsGame   = c4FgIsGameSignals(manualGame, configGame, engineGame);

    bool alreadyFrozen, audioActive;
    int  targetUid, bgTicks;
    std::string frozenCgDir, targetPkg, lastGame;
    std::vector<int> frozenPids;
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
        frozenPids    = s_frozenPids;
    }

    if (haveRestore) {
        if (fg == restored.pkg) {
            std::vector<int> rpids = restored.pids;
            applyFreezeSmart(restored.cgDir, false, restored.uid, &rpids);
            writeFileAtomic(c4StateFilePath().c_str(), "");
        } else {
            alreadyFrozen = true;
            frozenCgDir   = restored.cgDir;
            frozenPids    = restored.pids;
            targetPkg     = restored.pkg;
            targetUid     = restored.uid;
            if (!restored.lastGame.empty()) lastGame = restored.lastGame;
        }
    }

    if (fgIsGame) lastGame = fg;

    std::string target = c4ChooseTarget(lastGame, fg, fgIsGame);

    if (alreadyFrozen && target != targetPkg && !frozenCgDir.empty()) {
        applyFreezeSmart(frozenCgDir, false, targetUid, &frozenPids);
        alreadyFrozen = false;
        frozenCgDir.clear();
        frozenPids.clear();
        writeFileAtomic(c4StateFilePath().c_str(), "");
    }
    if (target.empty()) {
        targetUid = 0;
        lastGame.clear();
    } else if (targetUid == 0) {
        targetUid = resolveAppUid(target);
    }

    bool isFg = !target.empty() && fg == target;

    if (isFg) {
        bgTicks = -1;
        audioActive = false;
    } else if (!target.empty()) {
        if (bgTicks < 0) bgTicks = 0; else bgTicks++;

        if (audioReevalDue(bgTicks, kAudioReevalPeriod)) {
            audioActive = audioActiveNow(target);
        }
    } else {
        bgTicks = -1;
        audioActive = false;
    }

    std::string cgDir;
    std::string backend;
    if (!alreadyFrozen) {
        cgDir = findTargetV2CgroupDir(targetUid);
        if (!cgDir.empty() && freezerControllerPresent() && freezeNodeWritable(cgDir)) {
            backend = "v2";
        } else {
            cgDir.clear();
        }
        if (cgDir.empty()) {
            std::string v1Dir = findTargetV1CgroupDir(targetUid);
            if (!v1Dir.empty() && v1FreezerPresent() && v1FreezeNodeWritable(v1Dir)) {
                cgDir = v1Dir;
                backend = "v1";
            }
        }
        if (cgDir.empty() && probeV1Bucket()) {
            cgDir   = s_bucketPaths.frozenDir;
            backend = "v1-bucket";
        }
    } else {
        cgDir = frozenCgDir;
    }
    if (cgDir.empty() && !alreadyFrozen && !target.empty()) {
        lastGame.clear();
        target.clear();
        targetUid = 0;
    }

    C4DecisionInputs in;
    in.featureEnabled   = true;
    in.isForeground     = isFg;
    in.alreadyFrozen    = alreadyFrozen;
    in.freezerAvailable = !cgDir.empty();
    in.audioActive      = audioActive;

    std::string lastAction;
    C4Decision d = decideC4Freeze(in);
    if (d.action == C4Action::FREEZE) {
        lastAction = alreadyFrozen ? "FREEZE(already)" : ("FREEZE(" + backend + ")");
        if (applyFreezeSmart(cgDir, true, targetUid, &frozenPids)) {
            alreadyFrozen = true;
            frozenCgDir   = cgDir;
            std::vector<int> persistPids =
                isV1FrozenBucketDir(cgDir) ? frozenPids : std::vector<int>{};
            writeFileAtomic(c4StateFilePath().c_str(),
                            serializeC4Frozen(cgDir, target, targetUid, lastGame,
                                              persistPids));
        } else {
            lastAction = "FREEZE(write-fail-degraded)";
        }
    } else if (d.action == C4Action::UNFREEZE) {
        lastAction = audioActive ? "UNFREEZE(audio-started-mid-freeze)"
                                 : "UNFREEZE(foreground)";
        if (!frozenCgDir.empty()) {
            applyFreezeSmart(frozenCgDir, false, targetUid, &frozenPids);
        }
        alreadyFrozen = false;
        frozenCgDir.clear();
        frozenPids.clear();
        writeFileAtomic(c4StateFilePath().c_str(), "");
    } else {
        lastAction = "NO_CHANGE";
    }

    writeFileAtomic(c4LastActionPath().c_str(), lastAction);

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
        s_frozenPids    = frozenPids;
    }
}

std::string c4FreezerStateJson() {
    bool en = c4FreezerEnabled();

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

    bool hasV2 = freezerControllerPresent();
    bool hasV1 = v1FreezerPresent();
    bool onBucket = !frozenCgDir.empty() && isV1FrozenBucketDir(frozenCgDir);
    std::string backend;
    if (onBucket)      backend = "v1-bucket";
    else if (hasV2)    backend = "v2";
    else if (hasV1)    backend = "v1";
    else               backend = "";
    bool freezerAvail = !backend.empty();

    std::string degraded;
    if (!en) degraded = "disabled";
    else if (targetPkg.empty()) degraded = "no-game-target";
    else if (!frozenCgDir.empty()) {
        bool writable;
        if (onBucket) {
            writable = ::access((frozenCgDir + "/cgroup.procs").c_str(), W_OK) == 0;
        } else if (isV1FreezerDir(frozenCgDir)) {
            writable = v1FreezeNodeWritable(frozenCgDir);
        } else {
            writable = freezeNodeWritable(frozenCgDir);
        }
        if (!writable) degraded = "freeze-node-not-writable";
    }
    if (degraded.empty() && !freezerAvail) degraded = "no-freezer-controller";
    return c4StateJson(en, targetPkg, alreadyFrozen, frozenCgDir,
                       lastAction, audioActive,
                       freezerAvail, backend, degraded);
}

}
}

#endif
