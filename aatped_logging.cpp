#include "aatped.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <mutex>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace aatpe;

std::atomic<int> g_logLevel{2};
static FILE* g_logFp = nullptr;
static std::mutex g_logMutex;
std::atomic<unsigned int> g_debugSampleCounter{0};

static std::string g_logBuffer;
static int g_logBufferCount = 0;
static std::atomic<bool> g_logFlushThreadRunning{false};
static std::thread g_logFlushThread;
static constexpr size_t kLogBufferMaxBytes = 4096;
static constexpr int kLogBufferMaxCount = 100;
static constexpr int kLogFlushIntervalMs = 100;
static constexpr size_t kLogBufferOrphanCapBytes = 1u << 20;
static std::atomic<unsigned long> g_logDroppedOrphan{0};
static const mode_t kLogFileMode = 0600;

static void flushLogBufferLocked() {
    if (g_logBuffer.empty() || !g_logFp) return;
    size_t nw = fwrite(g_logBuffer.data(), 1, g_logBuffer.size(), g_logFp);
    if (nw != g_logBuffer.size()) {
        fprintf(stderr, "AATPE: log short write %zu/%zu\n", nw, g_logBuffer.size());
    }
    g_logBuffer.clear();
    g_logBufferCount = 0;
    fflush(g_logFp);
}

void logFlush() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    flushLogBufferLocked();
}

static void logFlushThreadFunc() {
    while (g_logFlushThreadRunning.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kLogFlushIntervalMs));
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (!g_logBuffer.empty()) {
            flushLogBufferLocked();
        }
    }
}

static void startLogFlushThread() {
    if (g_logFlushThreadRunning.load()) return;
    g_logFlushThreadRunning.store(true, std::memory_order_release);
    try {
        g_logFlushThread = std::thread(logFlushThreadFunc);
    } catch (const std::exception& e) {
        g_logFlushThreadRunning.store(false);
        fprintf(stderr, "AATPE: logFlushThread start failed: %s\n", e.what());
    }
}

void logShutdown() {
    g_logFlushThreadRunning.store(false, std::memory_order_release);
    if (g_logFlushThread.joinable()) {
        g_logFlushThread.join();
    }
    std::lock_guard<std::mutex> lock(g_logMutex);
    flushLogBufferLocked();
}

void logInit() {
    mode_t oldMask = umask(0077);
    g_logFp = fopen(LOG_FILE, "a");
    umask(oldMask);
    if (g_logFp) {
        int fd = fileno(g_logFp);
        if (fd >= 0 && fchmod(fd, kLogFileMode) != 0) {
            char ebuf[128];
            LOG_WARN("logInit fchmod failed: %s", aatpeStrError(errno, ebuf, sizeof(ebuf)));
        }
    } else {
        char ebuf[128];
        fprintf(stderr, "AATPE: Cannot open log file %s: %s\n", LOG_FILE,
                aatpeStrError(errno, ebuf, sizeof(ebuf)));
    }
}

void logReinit() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    flushLogBufferLocked();
    if (g_logFp) {
        if (fclose(g_logFp) != 0) {
            char ebuf[128];
            fprintf(stderr, "AATPE: logReinit fclose failed: %s\n",
                    aatpeStrError(errno, ebuf, sizeof(ebuf)));
        }
        g_logFp = nullptr;
    }
    mkdirIfNotExist(RUN_DIR, 0700);
    mode_t oldMask = umask(0077);
    g_logFp = fopen(LOG_FILE, "a");
    if (!g_logFp) {
        int fbFd = ::open("/data/local/tmp/aatpe_daemon.log",
                          O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fbFd >= 0) {
            g_logFp = ::fdopen(fbFd, "a");
            if (!g_logFp) ::close(fbFd);
        }
        if (!g_logFp) {
            umask(oldMask);
            char ebuf[128];
            fprintf(stderr, "AATPE: logReinit: FATAL Cannot open log file %s: %s\n",
                    LOG_FILE, aatpeStrError(errno, ebuf, sizeof(ebuf)));
            return;
        }
        { int fd = fileno(g_logFp); if (fd >= 0 && fchmod(fd, kLogFileMode) != 0) {
            char ebufF[128];
            fprintf(g_logFp, "[LOG] WARNING: logReinit fallback fchmod failed: %s\n",
                    aatpeStrError(errno, ebufF, sizeof(ebufF))); } }
        char ebuf[128];
        fprintf(g_logFp, "[LOG] WARNING: primary log %s unavailable (%s), using fallback\n",
                LOG_FILE, aatpeStrError(errno, ebuf, sizeof(ebuf)));
    }
    umask(oldMask);
    { int fd = fileno(g_logFp); if (fd >= 0 && fchmod(fd, kLogFileMode) != 0) {
        char ebufL[128];
        fprintf(g_logFp, "[LOG] WARNING: logReinit fchmod failed: %s\n",
                aatpeStrError(errno, ebufL, sizeof(ebufL))); } }
    fprintf(g_logFp, "[LOG] reinitialized after daemonize, fd restored\n");
    fflush(g_logFp);
    startLogFlushThread();
}

static const long long kMaxLogSize = aatpe::logging::kMaxLogSizeBytes;

static std::mutex g_logRotationMutex;

static bool logRotationNeededLocked() {
    if (!g_logFp) return false;
    long long pos = ftell(g_logFp);
    return pos > kMaxLogSize;
}

static void performLogRotation() {
    FILE* fpToClose = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (!g_logFp) return;
        double pos = ftell(g_logFp);
        if (pos <= kMaxLogSize) return;
        fpToClose = g_logFp;
        g_logFp = nullptr;
    }
    fclose(fpToClose);
    std::string oldPath = std::string(LOG_FILE) + ".1";
    char ebuf[128];
    if (rename(LOG_FILE, oldPath.c_str()) != 0) {
        int e = errno;
        fprintf(stderr, "AATPE: log rotation rename failed: %s\n",
                aatpeStrError(e, ebuf, sizeof(ebuf)));
    }
    if (chmod(oldPath.c_str(), kLogFileMode) != 0 && errno != ENOENT) {
        int e = errno;
        fprintf(stderr, "AATPE: log rotation chmod old failed: %s\n",
                aatpeStrError(e, ebuf, sizeof(ebuf)));
    }
    mode_t oldMask = umask(0077);
    FILE* newFp = fopen(LOG_FILE, "a");
    umask(oldMask);
    if (newFp) {
        int fd = fileno(newFp);
        if (fd >= 0 && fchmod(fd, kLogFileMode) != 0) {
            int e = errno;
            fprintf(stderr, "AATPE: log rotation fchmod failed: %s\n",
                    aatpeStrError(e, ebuf, sizeof(ebuf)));
        }
    } else {
        int e = errno;
        fprintf(stderr, "AATPE: log rotation failed: %s\n",
                aatpeStrError(e, ebuf, sizeof(ebuf)));
    }
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFp) fclose(g_logFp);
    g_logFp = newFp;
}

void logMsg(int level, const char* fmt, ...) {
    if (level > g_logLevel.load()) return;
    const char* tags[] = {"ERROR", "WARN ", "INFO ", "DEBUG", "TRACE"};
    const char* tag = (level >= 0 && level <= 4) ? tags[level] : "INFO ";
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    char msgBuf[4096];
    va_list args;
    va_start(args, fmt);
    int msgLen = vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);
    if (msgLen < 0) msgLen = 0;
    if (msgLen >= (int)sizeof(msgBuf)) msgLen = (int)sizeof(msgBuf) - 1;

    for (int i = 0; i < msgLen; i++) {
        unsigned char c = (unsigned char)msgBuf[i];
        if (c == '\n' || c == '\r' || c == '\t') msgBuf[i] = ' ';
        else if (c < 0x20 || c == 0x7f) msgBuf[i] = '?';
    }

    char lineBuf[4200];
    int lineLen = snprintf(lineBuf, sizeof(lineBuf), "[%s] [%s] %.*s\n", ts, tag, msgLen, msgBuf);
    if (lineLen < 0) lineLen = 0;
    if (lineLen >= (int)sizeof(lineBuf)) lineLen = (int)sizeof(lineBuf) - 1;

    std::unique_lock<std::mutex> lock(g_logMutex);
    if (logRotationNeededLocked()) {
        lock.unlock();
        {
            std::lock_guard<std::mutex> rlock(g_logRotationMutex);
            performLogRotation();
        }
        lock.lock();
    }
    if (!g_logFp) {
        if (g_logBuffer.size() < kLogBufferOrphanCapBytes) {
            g_logBuffer.append(lineBuf, (size_t)lineLen);
            g_logBufferCount++;
        } else {
            unsigned long dropped = g_logDroppedOrphan.fetch_add(1, std::memory_order_relaxed);
            if (dropped == 0) {
                fprintf(stderr, "AATPE: log orphan buffer hit %zu bytes; dropping further lines until sink recovers\n",
                        kLogBufferOrphanCapBytes);
            }
        }
        return;
    }

    if (level <= 1) {
        flushLogBufferLocked();
        size_t nw = fwrite(lineBuf, 1, (size_t)lineLen, g_logFp);
        if (nw != (size_t)lineLen) {
            fprintf(stderr, "AATPE: log direct-write short write %zu/%zu\n",
                    nw, (size_t)lineLen);
        }
        fflush(g_logFp);
        return;
    }

    g_logBuffer.append(lineBuf, (size_t)lineLen);
    g_logBufferCount++;
    if (g_logBuffer.size() >= kLogBufferMaxBytes || g_logBufferCount >= kLogBufferMaxCount) {
        flushLogBufferLocked();
    }
}

void logClear() {
    std::lock_guard<std::mutex> rotLock(g_logRotationMutex);
    std::lock_guard<std::mutex> logLock(g_logMutex);
    if (g_logFp) {
        fclose(g_logFp);
        g_logFp = nullptr;
    }
    int fd = ::open(LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                    kLogFileMode);
    if (fd >= 0) {
        g_logFp = fdopen(fd, "w");
        if (!g_logFp) {
            ::close(fd);
        }
    } else {
        mode_t oldMask = umask(0077);
        g_logFp = fopen(LOG_FILE, "a");
        umask(oldMask);
        if (g_logFp) {
            int cfd = fileno(g_logFp);
            if (cfd >= 0) fchmod(cfd, kLogFileMode);
        }
    }
}

#ifdef AATPE_HOST_TEST
size_t aatpeTestLogOrphanBytes() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logBuffer.size();
}
unsigned long aatpeTestLogOrphanDropped() {
    return g_logDroppedOrphan.load(std::memory_order_relaxed);
}
#endif
