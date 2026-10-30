#include "aatped.h"
#include <sys/system_properties.h>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <unordered_map>
#include <algorithm>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <csignal>
#include <chrono>
#include <thread>
#include <vector>

using namespace aatpe;

static const int AATPE_EXEC_TIMEOUT_MS    = 2000;
static const int AATPE_EXEC_TOTAL_MS     = 3000;
static const int AATPE_REAP_MAX_MS       = 300;
static const int AATPE_DUMPSYS_COOLDOWN_S = 10;
static const size_t AATPE_EXEC_MAX_OUTPUT = size_t(256) * 1024;

static std::atomic<time_t> g_dumpsysCooldownUntil{0};

bool dumpsysAvailable() {
    time_t until = g_dumpsysCooldownUntil.load(std::memory_order_relaxed);
    if (until == 0) return true;
    return time(nullptr) >= until;
}

void triggerDumpsysCooldown() {
    g_dumpsysCooldownUntil.store(time(nullptr) + AATPE_DUMPSYS_COOLDOWN_S,
                                 std::memory_order_relaxed);
}

std::string aatpeExec(const std::string& cmd, const char* debugTag) {
    if (!dumpsysAvailable() && cmd.find("dumpsys") != std::string::npos) {
        return std::string();
    }
    std::string out;
    int pipefd[2];
    if (pipe(pipefd) != 0) return out;
    int syncPipe[2];
    if (pipe(syncPipe) != 0) {
        close(pipefd[0]); close(pipefd[1]);
        return out;
    }
    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]); close(pipefd[1]);
        close(syncPipe[0]); close(syncPipe[1]);
        return out;
    }
    if (child == 0) {
        int pgidRc = setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        signal(SIGPIPE, SIG_IGN);
        close(syncPipe[0]);
        char syncByte = (pgidRc == 0) ? 'S' : 'F';
        if (write(syncPipe[1], &syncByte, 1) != 1) {  }
        close(syncPipe[1]);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/system/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }
    close(syncPipe[1]);
    bool childInOwnPg = false;
    {
        char syncByte = 0;
        struct pollfd spfd{syncPipe[0], POLLIN, 0};
        int syncRet = poll(&spfd, 1, 500);
        if (syncRet > 0) {
            ssize_t nr = read(syncPipe[0], &syncByte, 1);
            if (nr == 1 && syncByte == 'S') childInOwnPg = true;
        }
    }
    close(syncPipe[0]);
    close(pipefd[1]);
    auto execStart = std::chrono::steady_clock::now();
    char buf[512];
    bool timedOut = false;
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - execStart).count();
        int remainingMs = AATPE_EXEC_TOTAL_MS - (int)elapsed;
        if (remainingMs <= 0) { timedOut = true; break; }
        int pollTimeout = (remainingMs < AATPE_EXEC_TIMEOUT_MS) ? remainingMs : AATPE_EXEC_TIMEOUT_MS;
        struct pollfd pfd{pipefd[0], POLLIN, 0};
        int pollRet = poll(&pfd, 1, pollTimeout);
        if (pollRet < 0) { if (errno == EINTR) continue; break; }
        if (pollRet == 0) { timedOut = true; break; }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                if (out.size() + (size_t)n <= AATPE_EXEC_MAX_OUTPUT) {
                    out += buf;
                } else {
                    size_t space = AATPE_EXEC_MAX_OUTPUT - out.size();
                    if (space > 0) out.append(buf, space);
                    break;
                }
            } else if (n == 0) { break; }
        }
        if (pfd.revents & POLLHUP) break;
    }
    close(pipefd[0]);
    if (timedOut) {
        if (childInOwnPg) kill(-child, SIGKILL);
        kill(child, SIGKILL);
        triggerDumpsysCooldown();
        if (debugTag) LOG_WARN("EXEC[%s] TIMEOUT after %lldms, killed %s %d",
                               debugTag,
                               (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - execStart).count(),
                               childInOwnPg ? "process group" : "child (pgid not detached)",
                               child);
    }
    int status = -1; bool reaped = false; int waitedMs = 0;
    while (waitedMs < AATPE_REAP_MAX_MS) {
        pid_t ret = waitpid(child, &status, WNOHANG);
        if (ret > 0) { reaped = true; break; }
        if (ret < 0 && errno == ECHILD) { reaped = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitedMs += 10;
    }
    if (!reaped && debugTag)
        LOG_WARN("EXEC[%s] child PID %d not reaped after %dms (D-state?), detaching",
                 debugTag, child, AATPE_REAP_MAX_MS);
    (void)status;
    return out;
}

std::string aatpeExecArgv(const std::vector<std::string>& argv, const char* debugTag,
                          int* outExitStatus, bool bypassCooldown) {
    std::string out;
    if (outExitStatus) *outExitStatus = -1;
    if (argv.empty()) return out;
    if (!bypassCooldown && !dumpsysAvailable()) return out;
    int pipefd[2];
    if (pipe(pipefd) != 0) return out;
    int syncPipe[2];
    if (pipe(syncPipe) != 0) { close(pipefd[0]); close(pipefd[1]); return out; }
    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]); close(pipefd[1]);
        close(syncPipe[0]); close(syncPipe[1]);
        return out;
    }
    if (child == 0) {
        int pgidRc = setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        signal(SIGPIPE, SIG_IGN);
        close(syncPipe[0]);
        char syncByte = (pgidRc == 0) ? 'S' : 'F';
        if (write(syncPipe[1], &syncByte, 1) != 1) {  }
        close(syncPipe[1]);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        std::vector<char*> vec;
        vec.reserve(argv.size() + 1);
        for (auto& a : argv) vec.push_back(const_cast<char*>(a.c_str()));
        vec.push_back(nullptr);
        execvp(vec[0], vec.data());
        _exit(127);
    }
    close(syncPipe[1]);
    bool childInOwnPg = false;
    {
        char syncByte = 0;
        struct pollfd spfd{syncPipe[0], POLLIN, 0};
        int sr = poll(&spfd, 1, 500);
        if (sr > 0) {
            ssize_t nr = read(syncPipe[0], &syncByte, 1);
            if (nr == 1 && syncByte == 'S') childInOwnPg = true;
        }
    }
    close(syncPipe[0]);
    close(pipefd[1]);
    auto execStart = std::chrono::steady_clock::now();
    char buf[512];
    bool timedOut = false;
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - execStart).count();
        int remaining = AATPE_EXEC_TOTAL_MS - (int)elapsed;
        if (remaining <= 0) { timedOut = true; break; }
        struct pollfd pfd{pipefd[0], POLLIN, 0};
        int pr = poll(&pfd, 1, remaining < AATPE_EXEC_TIMEOUT_MS ? remaining : AATPE_EXEC_TIMEOUT_MS);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) { timedOut = true; break; }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                if (out.size() + (size_t)n <= AATPE_EXEC_MAX_OUTPUT) out += buf;
                else break;
            } else if (n == 0) break;
        }
        if (pfd.revents & POLLHUP) break;
    }
    close(pipefd[0]);
    if (timedOut) {
        if (childInOwnPg) kill(-child, SIGKILL);
        kill(child, SIGKILL);
        triggerDumpsysCooldown();
        if (debugTag) LOG_WARN("EXEC_ARGV[%s] TIMEOUT, killed", debugTag);
    }
    int status = -1; bool reaped = false; int waited = 0;
    while (waited < AATPE_REAP_MAX_MS) {
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r > 0) { reaped = true; break; }
        if (r < 0 && errno == ECHILD) { reaped = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waited += 10;
    }
    if (!reaped && debugTag) LOG_WARN("EXEC_ARGV[%s] child %d not reaped, detaching", debugTag, child);
    if (outExitStatus) *outExitStatus = reaped ? status : -1;
    return out;
}

struct StateCache {
    std::mutex mu;
    std::unordered_map<std::string,std::string> map;
    bool seeded = false;
};
static StateCache& stateCache() {
    static StateCache* c = new StateCache();
    return *c;
}

std::atomic<uint64_t> g_stateGeneration{0};

uint64_t stateCacheGeneration() {
    return g_stateGeneration.load(std::memory_order_acquire);
}

std::string stateCacheTrimTrailing(const std::string& s) {
    size_t e = s.size();
    while (e > 0) {
        char c = s[e-1];
        if (c=='\n'||c=='\r'||c==' '||c=='\t') e--; else break;
    }
    return s.substr(0, e);
}

static std::string stateCacheRelativeName(const char* path) {
    if (!path || !STATE_DIR) return "";
    std::string p(path), sd(STATE_DIR);
    if (sd.empty()) return "";
    if (p.size() < sd.size() + 1 ||
        p.compare(0, sd.size(), sd) != 0 || p[sd.size()] != '/') return "";
    std::string name = p.substr(sd.size() + 1);
    if (name.empty() || name.find('/') != std::string::npos) return "";
    return name;
}

static const size_t kStateCacheMaxEntries = 4096;
static void stateCacheNoteWrite(const char* path, const std::string& content) {
    std::string name = stateCacheRelativeName(path);
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(stateCache().mu);
    if (stateCache().map.size() >= kStateCacheMaxEntries) {
        size_t evict = kStateCacheMaxEntries / 10;
        auto it = stateCache().map.begin();
        for (size_t i = 0; i < evict && it != stateCache().map.end(); ++i) {
            it = stateCache().map.erase(it);
        }
    }
    stateCache().map[name] = stateCacheTrimTrailing(content);
}

void stateCacheNoteRemove(const char* path) {
    std::string name = stateCacheRelativeName(path);
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(stateCache().mu);
    stateCache().map.erase(name);
}

bool stateCacheIsSeeded() {
    std::lock_guard<std::mutex> lk(stateCache().mu);
    return stateCache().seeded;
}

void stateCacheSeedFromDir(const char* dir) {
    std::lock_guard<std::mutex> lk(stateCache().mu);
    if (stateCache().seeded) return;
    stateCache().map.clear();
    if (dir) {
        DIR* d = opendir(dir);
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                if (strstr(ent->d_name, ".tmp.") != nullptr) continue;
                std::string full = std::string(dir) + "/" + ent->d_name;
                struct stat st;
                if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                    int fd = open(full.c_str(), O_RDONLY);
                    if (fd >= 0) {
                        char buf[4096]; std::string val; ssize_t n;
                        while ((n = read(fd, buf, sizeof(buf))) > 0) val.append(buf, n);
                        close(fd);
                        stateCache().map[ent->d_name] = stateCacheTrimTrailing(val);
                    }
                }
            }
            closedir(d);
        }
    }
    stateCache().seeded = true;
}

std::vector<std::pair<std::string,std::string>> stateCacheSnapshot() {
    std::vector<std::pair<std::string,std::string>> out;
    std::string sd(STATE_DIR ? STATE_DIR : "");
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lk(stateCache().mu);
        keys.reserve(stateCache().map.size());
        for (const auto& kv : stateCache().map) keys.push_back(kv.first);
    }
    std::vector<std::string> dead;
    if (!sd.empty()) {
        for (const auto& key : keys) {
            struct stat st;
            std::string full = sd + "/" + key;
            if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) dead.push_back(key);
        }
    }
    {
        std::lock_guard<std::mutex> lk(stateCache().mu);
        for (const auto& key : dead) stateCache().map.erase(key);
        out.reserve(stateCache().map.size());
        for (const auto& kv : stateCache().map) {
            const std::string& k = kv.first;
            if (k.size() > 5 && k.compare(k.size()-5, 5, ".json") == 0) continue;
            out.push_back(kv);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const std::pair<std::string,std::string>&a,
                 const std::pair<std::string,std::string>&b){ return a.first < b.first; });
    return out;
}

std::string readFile(const char* path) {
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

bool writeFile(const char* path, const std::string& content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const char* data = content.c_str();
    size_t remaining = content.size();
    ssize_t totalWritten = 0;
    while (remaining > 0) {
        ssize_t w = write(fd, data + totalWritten, remaining);
        if (w <= 0) {
            if (w == 0) { usleep(1000); continue; }
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        totalWritten += w;
        remaining -= (size_t)w;
    }
    int closeRc = close(fd);
    if (closeRc == 0) stateCacheNoteWrite(path, content);
    return closeRc == 0;
}

bool writeFile(const char* path, int val) {
    return writeFile(path, std::to_string(val));
}

bool writeFile(const char* path, float value) {
    return writeFile(path, std::to_string(value));
}

bool writeFile(const char* path, long long value) {
    return writeFile(path, std::to_string(value));
}

#ifdef AATPE_HOST_TEST
int g_fi_write_short_after = 0;
int g_fi_write_fail_after  = 0;
int g_fi_fsync_fail        = 0;
void aatpeTestFiReset() { g_fi_write_short_after = 0; g_fi_write_fail_after = 0; g_fi_fsync_fail = 0; }
#endif

bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmpPath = std::string(path) + ".tmp.XXXXXX";
    int fd = mkstemp(&tmpPath[0]);
    if (fd < 0) return false;
    const char* data = content.c_str();
    size_t remaining = content.size();
    ssize_t totalWritten = 0;
#ifdef AATPE_HOST_TEST
    ssize_t writeCalls = 0;
#endif
    while (remaining > 0) {
        ssize_t w;
#ifdef AATPE_HOST_TEST
        writeCalls++;
        if (g_fi_write_fail_after > 0 && writeCalls >= g_fi_write_fail_after) {
            errno = EIO; w = -1;
        } else if (g_fi_write_short_after > 0 && writeCalls == g_fi_write_short_after) {
            w = write(fd, data + totalWritten, 1);
        } else
#endif
        w = write(fd, data + totalWritten, remaining);
        if (w <= 0) {
            if (w == 0) { usleep(1000); continue; }
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmpPath.c_str());
            return false;
        }
        totalWritten += w;
        remaining -= (size_t)w;
    }
    int fsyncRc;
#ifdef AATPE_HOST_TEST
    if (g_fi_fsync_fail) { errno = EIO; fsyncRc = -1; } else
#endif
    fsyncRc = fsync(fd);
    if (fsyncRc != 0) {
        int e = errno;
        LOG_WARN("writeFileAtomic fsync failed: %s (aborting atomic publish, tmp removed)",
                 aatpeErrStr(e));
        close(fd);
        unlink(tmpPath.c_str());
        return false;
    }
    struct stat tmpSt;
    if (fstat(fd, &tmpSt) != 0 || !S_ISREG(tmpSt.st_mode)) {
        close(fd);
        unlink(tmpPath.c_str());
        return false;
    }
    close(fd);
    if (rename(tmpPath.c_str(), path) != 0) {
        unlink(tmpPath.c_str());
        return false;
    }
    {
        std::string dir = ".";
        std::string pstr(path);
        size_t slash = pstr.rfind('/');
        if (slash != std::string::npos) dir = pstr.substr(0, slash == 0 ? 1 : slash);
        int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            if (fsync(dfd) != 0) LOG_WARN("persistState: dir fsync failed (%s)", aatpeErrStr(errno));
            close(dfd);
        }
    }
    stateCacheNoteWrite(path, content);
    return true;
}

bool atomicWriteFile(const char* path, const std::string& content) {
    return writeFileAtomic(path, content);
}
bool atomicWriteFile(const char* path, int val) {
    return writeFileAtomic(path, std::to_string(val));
}
bool atomicWriteFile(const char* path, long long value) {
    return writeFileAtomic(path, std::to_string(value));
}

std::string getModuleDir() {
    static std::once_flag once;
    static std::string cachedDir;
    std::call_once(once, [](){
        const char* candidates[] = {
            "/data/adb/modules/aatpe",
            "/data/adb/ksu/modules/aatpe",
            "/data/adb/ap/modules/aatpe",
            "/data/adb/modules_update/aatpe",
            nullptr
        };
        for (int i = 0; candidates[i]; i++) {
            std::string binPath = std::string(candidates[i]) + "/system/bin";
            struct stat st;
            if (stat(binPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                cachedDir = candidates[i];
                return;
            }
        }
        cachedDir = "";
    });
    return cachedDir;
}
int readInt(const char* path, int def) {
    std::string s = readFile(path);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

long long readInt64(const char* path, long long def) {
    std::string s = readFile(path);
    if (s.empty()) return def;
    try { return std::stoll(s); } catch (...) { return def; }
}

static const int CPUSET_CACHE_TTL_SEC = 1;

struct CpusetCacheEntry {
    cpu_set_t mask;
    time_t timestamp;
    bool valid;
};

static std::mutex g_cpusetCacheMutex;
static std::unordered_map<std::string, CpusetCacheEntry> g_cpusetCache;

cpu_set_t parseCpusetCpuList(const std::string& listStr) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    std::string s = listStr;
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return mask;
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);
    if (s.empty()) return mask;

    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string token = (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
        size_t ts = token.find_first_not_of(" \t");
        size_t te = token.find_last_not_of(" \t");
        if (ts != std::string::npos) token = token.substr(ts, te - ts + 1);
        if (!token.empty()) {
            size_t dash = token.find('-');
            if (dash != std::string::npos) {
                int lo = parseIntSafe(token.substr(0, dash));
                int hi = parseIntSafe(token.substr(dash + 1));
                if (lo >= 0 && hi >= lo && lo < CPU_SETSIZE && hi < CPU_SETSIZE) {
                    for (int c = lo; c <= hi; c++) CPU_SET(c, &mask);
                }
            } else {
                int c = parseIntSafe(token);
                if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &mask);
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return mask;
}

std::string readFirstLine(const char* path) {
    std::ifstream f(path);
    if (!f) return "";
    std::string line;
    if (!std::getline(f, line)) return "";
    size_t start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = line.find_last_not_of(" \t\r\n");
    return line.substr(start, end - start + 1);
}

#include <sys/system_properties.h>
int aatpeAndroidSdkInt() {
    static std::once_flag once;
    static int s_sdkInt = 0;
    std::call_once(once, [](){
        int sdk = 0;
        char buf[PROP_VALUE_MAX] = {0};
        if (__system_property_get("ro.build.version.sdk", buf) > 0) {
            char* end = nullptr;
            long v = strtol(buf, &end, 10);
            if (end && *end == '\0' && v > 0 && v < 10000) sdk = (int)v;
        }
        if (sdk == 0) {
            std::ifstream f("/system/build.prop");
            std::string line;
            while (sdk == 0 && std::getline(f, line)) {
                if (line.rfind("ro.build.version.sdk=", 0) == 0) {
                    char* end = nullptr;
                    long v = strtol(line.c_str() + 21, &end, 10);
                    if (end && *end == '\0' && v > 0 && v < 10000) sdk = (int)v;
                }
            }
        }
        if (sdk == 0) {
#ifdef __ANDROID_API__
            sdk = __ANDROID_API__;
#endif
        }
        s_sdkInt = sdk;
    });
    return s_sdkInt;
}
const char* aatpeAndroidVersionName() {
    static std::once_flag once;
    static char s_verName[32] = {0};
    std::call_once(once, [](){
        char buf[PROP_VALUE_MAX] = {0};
        if (__system_property_get("ro.build.version.release", buf) > 0 && buf[0] != '\0') {
            snprintf(s_verName, sizeof(s_verName), "%s", buf);
        } else {
            snprintf(s_verName, sizeof(s_verName), "unknown");
        }
    });
    return s_verName;
}
bool aatpeIsCgroupV2Cpuset() {
    static std::once_flag once;
    static int s_cached = -1;
    std::call_once(once, [](){
        bool v2 = false;
        struct stat st;
        if (stat("/sys/fs/cgroup/cpuset", &st) == 0 && S_ISDIR(st.st_mode)) {
            struct stat st2;
            if (stat("/sys/fs/cgroup/cpuset/cpuset.cpus", &st2) == 0 ||
                stat("/sys/fs/cgroup/cpuset/cpuset.effective_cpus", &st2) == 0) {
                v2 = true;
            }
        }
        s_cached = v2 ? 1 : 0;
    });
    return s_cached == 1;
}
std::string aatpeCpusetMountRoot() {
    struct stat st;
    if (stat("/dev/cpuset", &st) == 0 && S_ISDIR(st.st_mode)) {
        struct stat st2;
        if (stat("/dev/cpuset/cpus", &st2) == 0 || stat("/dev/cpuset/cpuset.cpus", &st2) == 0) {
            return "/dev/cpuset";
        }
    }
    if (stat("/sys/fs/cgroup/cpuset", &st) == 0 && S_ISDIR(st.st_mode)) {
        return "/sys/fs/cgroup/cpuset";
    }
    return "/dev/cpuset";
}
static std::string readCpusetAttr(const std::string& relPath, const std::string& v1Name, const std::string& v2Name) {
    const char* roots[] = {"/dev/cpuset", "/sys/fs/cgroup/cpuset"};
    for (const char* root : roots) {
        struct stat st;
        if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        std::string p1 = std::string(root) + relPath + "/" + v1Name;
        std::string v = readFirstLine(p1.c_str());
        if (!v.empty()) return v;
        std::string p2 = std::string(root) + relPath + "/" + v2Name;
        v = readFirstLine(p2.c_str());
        if (!v.empty()) return v;
    }
    std::string root = aatpeCpusetMountRoot();
    std::string p1 = root + relPath + "/" + v1Name;
    std::string v = readFirstLine(p1.c_str());
    if (!v.empty()) return v;
    std::string p2 = root + relPath + "/" + v2Name;
    return readFirstLine(p2.c_str());
}

std::string readThreadCpuset(int tid) {
    std::string path = "/proc/" + std::to_string(tid) + "/cpuset";
    std::string cpuset = readFirstLine(path.c_str());
    if (cpuset.empty()) return "";
    if (cpuset[0] != '/') cpuset = "/" + cpuset;
    return cpuset;
}

static cpu_set_t buildDynamicFallbackMask() {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    std::vector<int> dynCpus = getOnlineCpus();
    if (!dynCpus.empty()) {
        for (int c : dynCpus) {
            if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &mask);
        }
        return mask;
    }
    if (!g_allCpus.empty()) {
        for (int c : g_allCpus) {
            if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &mask);
        }
        return mask;
    }
    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n <= 0) n = 8;
    if (n > CPU_SETSIZE) n = CPU_SETSIZE;
    for (int c = 0; c < n; c++) CPU_SET(c, &mask);
    return mask;
}

cpu_set_t readCpusetCpus(const std::string& cpusetPath) {
    if (cpusetPath.empty() || cpusetPath == "/") {
        std::string eff = readCpusetAttr("", "effective_cpus", "cpuset.effective_cpus");
        if (!eff.empty()) return parseCpusetCpuList(eff);
        std::string cpus = readCpusetAttr("", "cpus", "cpuset.cpus");
        if (!cpus.empty()) return parseCpusetCpuList(cpus);
        return buildDynamicFallbackMask();
    }

    std::string eff = readCpusetAttr(cpusetPath, "effective_cpus", "cpuset.effective_cpus");
    if (!eff.empty()) return parseCpusetCpuList(eff);
    std::string cpus = readCpusetAttr(cpusetPath, "cpus", "cpuset.cpus");
    if (!cpus.empty()) return parseCpusetCpuList(cpus);
    cpu_set_t empty;
    CPU_ZERO(&empty);
    return empty;
}

cpu_set_t getThreadAllowedCpus(int tid) {
    std::string cpusetPath = readThreadCpuset(tid);
    if (cpusetPath.empty()) {
        return buildDynamicFallbackMask();
    }

    time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> lock(g_cpusetCacheMutex);
        auto it = g_cpusetCache.find(cpusetPath);
        if (it != g_cpusetCache.end() && it->second.valid &&
            (now - it->second.timestamp) < CPUSET_CACHE_TTL_SEC) {
            return it->second.mask;
        }
    }

    cpu_set_t mask = readCpusetCpus(cpusetPath);

    if (CPU_COUNT(&mask) == 0) {
        mask = readCpusetCpus("/");
        if (CPU_COUNT(&mask) == 0) {
            mask = buildDynamicFallbackMask();
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_cpusetCacheMutex);
        if (g_cpusetCache.size() >= 256) {
            std::vector<std::pair<time_t, std::string>> age;
            age.reserve(g_cpusetCache.size());
            for (const auto& kv : g_cpusetCache)
                age.emplace_back(kv.second.timestamp, kv.first);
            std::sort(age.begin(), age.end());
            size_t toDrop = std::min<size_t>(64, age.size());
            for (size_t i = 0; i < toDrop; ++i)
                g_cpusetCache.erase(age[i].second);
        }
        CpusetCacheEntry entry;
        entry.mask = mask;
        entry.timestamp = now;
        entry.valid = true;
        g_cpusetCache[cpusetPath] = entry;
    }

    return mask;
}

bool isCpuAllowed(int cpu, const cpu_set_t& allowed) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
    return CPU_ISSET(cpu, &allowed);
}

void trim(std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) { s.clear(); return; }
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);
}

void mkdirIfNotExist(const char* path, mode_t mode) {
    if (mkdir(path, mode) != 0) {
        if (errno == EEXIST) {
            if (chmod(path, mode) != 0 && errno != ENOENT) {
                LOG_WARN("mkdir %s exists but chmod %o failed: %s", path, (unsigned)mode, aatpeErrStr(errno));
            }
        } else {
            LOG_WARN("mkdir %s failed: %s", path, aatpeErrStr(errno));
        }
    }
}

