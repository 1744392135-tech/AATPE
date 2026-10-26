// aatpe_pid_identity.h - unified PID identity verification (P1-2 hardening)
//
// Problem being fixed:
//   The previous identity checks were substring-based and incomplete:
//     * aatpe-watchdog.cpp isAatpedIdentity(): readlink /proc/<pid>/exe, then
//       exe.find("aatped") != npos. This matches "aatped_fake", "my_aatped_test",
//       "/system/bin/aatped-evil" etc.
//     * aatpe-cli.cpp isAatpedPid(): exe+cmdline substring "aatped".
//     * aatped_main.cpp --stop: strstr(linkBuf, "aatped") — the loosest.
//   None of them checked /proc/<pid>/stat starttime, so a PID that was recycled
//   by an unrelated process (after the real daemon exited) would pass as long as
//   its exe path happened to contain the substring "aatped". The watchdog could
//   SIGKILL/SIGTERM the wrong process.
//
// This header provides ONE unified verification entry point used by all three
// signal-senders (watchdog, CLI, aatped --stop):
//
//   verifyAatpedPid(pid_t pid)
//
// Triple identity:
//   1. kill(pid, 0) == 0                — process exists and we have permission.
//   2. readlink /proc/<pid>/exe         — realpath EXACTLY equals
//                                        "/system/bin/aatped" (string compare,
//                                        NOT find()/substring).
//   3. /proc/<pid>/stat starttime       — must equal the starttime the real
//                                        daemon published to
//                                        STATE_DIR/daemon_starttime at its own
//                                        startup. This defeats PID reuse: if the
//                                        PID was recycled by another process
//                                        (even one whose exe happens to be
//                                        /system/bin/aatped), the starttime
//                                        differs and verification fails.
//
// The daemon (aatped_main.cpp) writes daemon_starttime ONCE at startup, BEFORE
// writing daemon.pid, so that any observer that sees daemon.pid can immediately
// cross-check. If the published starttime file is absent (very-early-boot race
// before the daemon writes it, or a downgraded binary), the starttime check is
// skipped but the exact-exe check still rejects any non-aatped binary. This
// keeps first-boot liveness working without weakening the steady-state defense.
//
// Self-contained: includes only standard C/C++ headers so it can be dropped into
// any of the three standalone binaries (aatpe-watchdog.cpp, aatpe-cli.cpp,
// aatped_main.cpp) without pulling in the rest of the project.
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

// Exact expected exe realpath. NOT a substring match: this must be byte-for-byte
// equal to readlink("/proc/<pid>/exe"). A process whose exe is
// "/system/bin/aatped_fake" or "/data/my/my_aatped_test" does NOT match.
static const char* kExpectedDaemonExe = "/system/bin/aatped";

// Resolve the expected daemon exe path through any overlay/magic-mount layer.
// On Magisk-installed systems, /system/bin/aatped is a magic mount that
// readlink() resolves to the real module path
// (e.g. /data/adb/modules/aatpe/system/bin/aatped). A strict comparison
// against "/system/bin/aatped" would therefore reject the real daemon.
// We resolve at verification time and accept BOTH the canonical path and
// the raw expected path, so Magisk, bind-mount, and plain installs all pass
// while substring-based spoofing (aatped_fake, my_aatped_test) is still
// rejected. Returns the resolved path, or kExpectedDaemonExe if realpath
// fails (file not present yet — early boot race).
inline std::string resolveExpectedDaemonExe() {
    char resolved[PATH_MAX];
    if (realpath(kExpectedDaemonExe, resolved) != nullptr) {
        return std::string(resolved);
    }
    return std::string(kExpectedDaemonExe);
}

// Where the daemon publishes its own starttime (clock ticks from
// /proc/self/stat field 21). Lives in the state dir next to daemon.pid.
//
// SEC-05 (P3, single source of truth): these two paths used to be hardcoded
// to "/dev/aatpe/state/...". The writer side in aatped_main.cpp builds the
// same paths from the daemon's global `STATE_DIR` (currently "/dev/aatpe/state"
// — see aatped_main.cpp:76). The header is deliberately self-contained (it is
// dropped into the standalone aatpe-cli / aatpe-watchdog TUs, which do NOT
// link the daemon's STATE_DIR definition), so we cannot take an
// `extern const char* STATE_DIR` here. Instead we expose an overridable
// accessor that defaults to the production path; the daemon calls
// pid_identity::setStateDir(STATE_DIR) once at startup so the writer and every
// reader share ONE source of truth. If STATE_DIR ever moves (e.g. to a test
// tmp dir, or a future install layout), the daemon-side setter moves the
// reader with it instead of silently drifting to the hardcoded bootstrap-only
// path (which would have meant starttime/exe cross-checks were skipped while
// the daemon wrote somewhere else).
inline const char*& stateDirRef() {
    static const char* s = "/dev/aatpe/state";  // production default
    return s;
}
inline const char* stateDir() { return stateDirRef(); }
// Daemon calls this once, right after it has resolved STATE_DIR, BEFORE it
// publishes daemon_starttime / daemon_exe. Standalone binaries never call it
// and keep the production default (which matches the on-device layout).
inline void setStateDir(const char* d) { if (d && *d) stateDirRef() = d; }

// Build "<stateDir>/<name>". Kept as a helper so the two published records
// always resolve through the same accessor.
inline std::string statePath(const char* name) {
    return std::string(stateDir()) + "/" + name;
}

// h6 D2: where the live daemon publishes its own /proc/self/exe (readlink).
// On a KernelSU/Magisk bind/magic-mount device the real exe is
// /data/adb/modules/aatpe/system/bin/aatped, and realpath(/system/bin/aatped)
// does NOT resolve through the magic mount — so the old set-based exe check
// falsely classified the live daemon as dead. The daemon publishes its own exe
// once (after lockf success) and the watchdog compares against it.
// SEC-05: resolved via statePath("daemon_exe") so it tracks setStateDir().

// Read the exe path the daemon published at its own startup.
// Returns "" if the record is missing/unreadable (bootstrap race / downgraded).
inline std::string readPublishedExe() {
    std::ifstream f(statePath("daemon_exe"));
    if (!f) return "";
    std::string s;
    std::getline(f, s);
    // trim trailing whitespace/newline
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// Read /proc/<pid>/exe realpath. Returns "" on any failure.
inline std::string readExeRealpath(pid_t pid) {
    if (pid <= 0) return "";
    std::string exePath = "/proc/" + std::to_string(pid) + "/exe";
    char link[PATH_MAX];
    ssize_t n = readlink(exePath.c_str(), link, sizeof(link) - 1);
    if (n <= 0) return "";
    link[n] = '\0';
    return std::string(link);
}

// Read the starttime field (clock ticks) from /proc/<pid>/stat.
// Field layout (proc(5)):
//   pid comm state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt
//   cmajflt utime stime cutime cstime priority num_threads itrealvalue starttime ...
// comm is wrapped in parentheses and may itself contain spaces and parens, so we
// anchor tokenization at the LAST ')'. After ')', the token stream starts at
// `state` (field 3). R8-003 / LED-110: host-verified on this build, starttime
// (stat field 21) lands on the 20th token after ')'. The earlier comment block
// contradicted the code by saying "starttime is the 19th token"; the code walks to
// i == 20 and returns there, which on-device equals uptime*HZ (the clock-tick
// starttime), not vsize. Returns -1 on any parse failure.
inline long long readPidStarttime(pid_t pid) {
    if (pid <= 0) return -1;
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream f(path);
    if (!f) return -1;
    std::string line;
    if (!std::getline(f, line)) return -1;
    size_t rp = line.rfind(')');
    if (rp == std::string::npos) return -1;
    // P1-fix (issue 1): a malformed /proc/<pid>/stat line (short read, comm
    // ending in ')', or pid exited mid-read) can have rp == line.size() - 1.
    // Calling line.substr(rp + 2) with pos > size() throws std::out_of_range.
    // Guard explicitly and bail out instead.
    if (rp + 2 > line.size()) return -1;
    std::istringstream ss(line.substr(rp + 2));
    std::string tok;
    for (int i = 1; i <= 20; i++) {
        if (!(ss >> tok)) return -1;
        if (i == 20) {
            try {
                long long st = std::stoll(tok);
                // Issue-26: sanity check — starttime is clock ticks since boot and
                // must be non-negative and within a reasonable range (< 100 years
                // of ticks at 1000 HZ ≈ 3.15e12). An out-of-range value suggests
                // the field offset shifted (kernel change) — reject rather than
                // silently comparing against a wrong field.
                if (st < 0 || st > 3153600000000LL) return -1;
                return st;
            } catch (...) { return -1; }
        }
    }
    return -1;
}

// Read the starttime the daemon published at its own startup.
// Returns -1 if the record is missing/unreadable (bootstrap race).
inline long long readPublishedStarttime() {
    std::ifstream f(statePath("daemon_starttime"));
    if (!f) return -1;
    long long v = -1;
    f >> v;
    return v;
}

// Triple identity verification. Returns true ONLY if:
//   1. kill(pid, 0) succeeds (process exists, we may signal it);
//   2. /proc/<pid>/exe realpath == kExpectedDaemonExe (exact, not substring);
//   3. /proc/<pid>/stat starttime == the starttime the daemon published.
// On false, callers MUST NOT send any signal to pid.
//
// Note on #3: if the published starttime record is absent (very early boot,
// before the daemon has written it), the starttime cross-check is skipped and
// the result is gated by checks #1 and #2 alone. The exact-exe match already
// rejects any non-aatped binary; the residual window (a /system/bin/aatped
// process that is not *our* instance) only exists in the first milliseconds
// after daemon launch and is closed the moment daemon_starttime is written.
//
// P1-fix (issue 2 / TOCTOU): instead of three independent path-based lookups
// (readlink "/proc/<pid>/exe", then read "/proc/<pid>/stat"), open
// "/proc/<pid>" as a directory fd ONCE and perform both the exe realpath
// (via readlinkat) and the starttime read (via openat+read) relative to that
// fd. Holding the fd pins the /proc/<pid> dentry for the duration of the
// check: even if the target exits mid-check, the dentry (and its exe/stat
// contents) remains valid until close(), so a recycled pid cannot swap in
// underneath us. The kill(pid,0) pre-check still confirms the process was
// alive when we started.
inline bool verifyAatpedPid(pid_t pid) {
    if (pid <= 0) return false;
    // 1. existence + signal permission
    if (kill(pid, 0) != 0) return false;

    // Open /proc/<pid> as a directory fd to pin the dentry for checks 2+3.
    std::string procDir = "/proc/" + std::to_string(pid);
    int dirFd = ::open(procDir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirFd < 0) return false;

    bool ok = false;
    do {
        // 2. exact exe realpath comparison via readlinkat (pinned to dirFd).
        //    Accept both the canonical /system/bin/aatped AND its realpath
        //    resolution (e.g. /data/adb/modules/aatpe/system/bin/aatped on
        //    Magisk). Any other path (aatped_fake, my_aatped_test, etc.)
        //    is rejected. This is still an EXACT match set, not a substring.
        char link[PATH_MAX];
        ssize_t n = ::readlinkat(dirFd, "exe", link, sizeof(link) - 1);
        if (n <= 0) break;
        link[n] = '\0';
        std::string liveExe(link);
        // h6 D2 identity: if the live daemon published its own /proc/self/exe
        // (daemon_exe), require an EXACT byte-for-byte match against it. This is
        // correct on bind/magic-mount installs where the real exe lives under
        // /data/adb/modules/... and /system/bin/aatped does NOT realpath through.
        // It is also at least as strict as the old set: a recycled PID running any
        // other binary (including /system/bin/aatped_fake) has a different exe and
        // fails. If daemon_exe is absent (bootstrap / downgraded), fall back to the
        // historical exact-match set {kExpectedDaemonExe, realpath(kExpected)}.
        std::string publishedExe = readPublishedExe();
        bool exeOk;
        if (!publishedExe.empty()) {
            exeOk = (liveExe == publishedExe);
        } else {
            std::string resolvedExe = resolveExpectedDaemonExe();
            exeOk = (liveExe == kExpectedDaemonExe || liveExe == resolvedExe);
        }
        if (!exeOk) break;

        // 3. starttime cross-check via openat(dirFd, "stat") — same pinned dentry.
        int statFd = ::openat(dirFd, "stat", O_RDONLY | O_CLOEXEC);
        if (statFd < 0) break;
        char buf[512];
        ssize_t rn = ::read(statFd, buf, sizeof(buf) - 1);
        ::close(statFd);
        if (rn <= 0) break;
        buf[rn] = '\0';

        // Parse the starttime field from the stat content we just read.
        // Find last ')' and walk 20 tokens after it.
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
        // v4.4.9 R3-F07: mirror the readPidStarttime() sanity bounds (non-negative
        // and < 100 years of ticks at 1000Hz ≈ 3.15e12). The inline openat+read
        // parse below used to only reject liveSt<0; a wildly out-of-range field
        // (e.g. a shifted /proc/<pid>/stat layout) would otherwise compare
        // garbage against the published starttime. Same clock/direction as
        // readPublishedStarttime(), so the bound is identical.
        if (!got || liveSt < 0 || liveSt > 3153600000000LL) break;

        long long expectedSt = readPublishedStarttime();
        if (expectedSt < 0) {
            // No trusted reference yet — bootstrap path. exe exact-match already
            // ensures this is the aatped binary; allow.
            ok = true;
            break;
        }
        ok = (liveSt == expectedSt);
    } while (false);

    ::close(dirFd);
    return ok;
}

} // namespace pid_identity
} // namespace aatpe

#endif // AATPE_PID_IDENTITY_H
