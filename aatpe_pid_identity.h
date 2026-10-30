#ifndef AATPE_PID_IDENTITY_H
#define AATPE_PID_IDENTITY_H

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <limits.h>

namespace aatpe {
namespace pid_identity {

static const char* kExpectedDaemonExe = "/system/bin/aatped";

inline std::string resolveExpectedDaemonExe() {
    char resolved[PATH_MAX];
    if (realpath(kExpectedDaemonExe, resolved) != nullptr) {
        return std::string(resolved);
    }
    return std::string(kExpectedDaemonExe);
}

inline const char*& stateDirRef() {
    static const char* s = "/dev/aatpe/state";
    return s;
}
inline const char* stateDir() { return stateDirRef(); }
inline void setStateDir(const char* d) { if (d && *d) stateDirRef() = d; }

inline std::string statePath(const char* name) {
    return std::string(stateDir()) + "/" + name;
}

inline std::string readPublishedExe() {
    std::ifstream f(statePath("daemon_exe"));
    if (!f) return "";
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

inline std::string readExeRealpath(pid_t pid) {
    if (pid <= 0) return "";
    std::string exePath = "/proc/" + std::to_string(pid) + "/exe";
    char link[PATH_MAX];
    ssize_t n = readlink(exePath.c_str(), link, sizeof(link) - 1);
    if (n <= 0) return "";
    link[n] = '\0';
    return std::string(link);
}

inline long long readPidStarttime(pid_t pid) {
    if (pid <= 0) return -1;
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream f(path);
    if (!f) return -1;
    std::string line;
    if (!std::getline(f, line)) return -1;
    size_t rp = line.rfind(')');
    if (rp == std::string::npos) return -1;
    if (rp + 2 > line.size()) return -1;
    std::istringstream ss(line.substr(rp + 2));
    std::string tok;
    for (int i = 1; i <= 20; i++) {
        if (!(ss >> tok)) return -1;
        if (i == 20) {
            try {
                long long st = std::stoll(tok);
                if (st < 0 || st > 3153600000000LL) return -1;
                return st;
            } catch (...) { return -1; }
        }
    }
    return -1;
}

inline long long readPublishedStarttime() {
    std::ifstream f(statePath("daemon_starttime"));
    if (!f) return -1;
    long long v = -1;
    f >> v;
    return v;
}

inline bool verifyAatpedPid(pid_t pid) {
    if (pid <= 0) return false;
    if (kill(pid, 0) != 0) return false;

    std::string procDir = "/proc/" + std::to_string(pid);
    int dirFd = ::open(procDir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirFd < 0) return false;

    bool ok = false;
    do {
        char link[PATH_MAX];
        ssize_t n = ::readlinkat(dirFd, "exe", link, sizeof(link) - 1);
        if (n <= 0) break;
        link[n] = '\0';
        std::string liveExe(link);
        std::string publishedExe = readPublishedExe();
        bool exeOk;
        if (!publishedExe.empty()) {
            exeOk = (liveExe == publishedExe);
        } else {
            std::string resolvedExe = resolveExpectedDaemonExe();
            exeOk = (liveExe == kExpectedDaemonExe || liveExe == resolvedExe);
        }
        if (!exeOk) break;

        int statFd = ::openat(dirFd, "stat", O_RDONLY | O_CLOEXEC);
        if (statFd < 0) break;
        char buf[512];
        ssize_t rn = ::read(statFd, buf, sizeof(buf) - 1);
        ::close(statFd);
        if (rn <= 0) break;
        buf[rn] = '\0';

        std::string line(buf);
        size_t rp = line.rfind(')');
        if (rp == std::string::npos || rp + 2 > line.size()) break;
        std::istringstream ss(line.substr(rp + 2));
        std::string tok;
        long long liveSt = -1;
        bool got = false;
        for (int i = 1; i <= 20; i++) {
            if (!(ss >> tok)) { got = false; break; }
            if (i == 20) {
                try { liveSt = std::stoll(tok); got = true; } catch (...) { got = false; }
            }
        }
        if (!got || liveSt < 0 || liveSt > 3153600000000LL) break;

        long long expectedSt = readPublishedStarttime();
        if (expectedSt < 0) {
            ok = true;
            break;
        }
        ok = (liveSt == expectedSt);
    } while (false);

    ::close(dirFd);
    return ok;
}

}
}

#endif
