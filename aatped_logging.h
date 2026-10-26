// AATPE - Logging sub-header
#ifndef AATPED_LOGGING_H
#define AATPED_LOGGING_H

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string.h>

void logInit();
void logReinit();
void logMsg(int level, const char* fmt, ...);
void logFlush();
void logShutdown();
void logClear();

// H7 (Track H): reentrant strerror wrapper. Defined inline in the header so any
// production TU can call it without linking aatped_logging.o (recipes for many
// unit tests do not pull logging.o). strerror_r branch:
//   - bionic (__ANDROID__) / glibc _GNU_SOURCE: GNU-style, returns char*.
//   - plain glibc host (XSI): returns int, fills buf.
inline const char* aatpeStrError(int err, char* buf, size_t n) {
    if (!buf || n == 0) return "";
    buf[0] = '\0';
    const char* s = buf;
#if defined(__ANDROID__) || defined(_GNU_SOURCE)
    s = ::strerror_r(err, buf, n);
#else
    (void)::strerror_r(err, buf, n);
    s = buf;
#endif
    if (!s || s[0] == '\0') {
        ::snprintf(buf, n, "Unknown error %d", err);
        s = buf;
    }
    return s;
}

// Convenience: per-thread buffer wrapper around aatpeStrError. thread_local makes
// this thread-safe; returns a C string valid until the next aatpeErrStr() call on
// THIS thread — safe to use directly in the immediately-following printf call.
inline const char* aatpeErrStr(int err) {
    static thread_local char s_buf[128];
    return aatpeStrError(err, s_buf, sizeof(s_buf));
}

// H7 (Track H): reentrant strerror wrapper. strerror() returns a pointer to a
// shared static buffer and is not thread-safe (clang-tidy concurrency-mt-unsafe).
// This renders the message into the caller-provided buffer `buf`/`n` and always
// returns a usable C string (never NULL). Handles both the bionic (GNU-style,
// char* return) and glibc (XSI-style int return / _GNU_SOURCE GNU-style)
// strerror_r signatures. Used on the logging/error path.

extern std::atomic<int> g_logLevel;
#define LOG_ERROR(...) logMsg(0, __VA_ARGS__)
#define LOG_WARN(...)  logMsg(1, __VA_ARGS__)
#define LOG_INFO(...)  logMsg(2, __VA_ARGS__)
// P3-28: LOG_FORCE temporarily raises g_logLevel to 4, logs at INFO, then
// restores the saved level. This is NOT thread-safe: another thread's logMsg()
// may interleave between the store(4) and the restore, and the restore itself
// is a lost-update. LOG_FORCE is used only for one-shot diagnostic dumps (CLI
// commands), not on the hot path, so the race is benign in practice.
#define LOG_FORCE(...) do { \
    int _saved = g_logLevel.load(); \
    g_logLevel.store(4); \
    logMsg(2, __VA_ARGS__); \
    logFlush(); \
    g_logLevel.store(_saved); \
} while(0)
extern std::atomic<unsigned int> g_debugSampleCounter;
// #24: compile-time trimming of debug/trace logging.
// Release builds do NOT define AATPE_DEBUG, so LOG_DEBUG / LOG_TRACE expand to
// nothing at compile time (zero runtime cost, no sampled-debug chatter on the
// real device). Dev builds pass -DAATPE_DEBUG to re-enable them.
#ifdef AATPE_DEBUG
  #define LOG_DEBUG(...) do { \
      if ((g_debugSampleCounter.fetch_add(1, std::memory_order_relaxed) % 100) == 0) \
          logMsg(3, __VA_ARGS__); \
  } while(0)
  #define LOG_TRACE(...) logMsg(4, __VA_ARGS__)
#else
  #define LOG_DEBUG(...) ((void)0)
  #define LOG_TRACE(...) ((void)0)
#endif

#endif // AATPED_LOGGING_H
