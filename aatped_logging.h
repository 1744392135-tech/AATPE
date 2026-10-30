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

inline const char* aatpeErrStr(int err) {
    static thread_local char s_buf[128];
    return aatpeStrError(err, s_buf, sizeof(s_buf));
}

extern std::atomic<int> g_logLevel;
#define LOG_ERROR(...) logMsg(0, __VA_ARGS__)
#define LOG_WARN(...)  logMsg(1, __VA_ARGS__)
#define LOG_INFO(...)  logMsg(2, __VA_ARGS__)
#define LOG_FORCE(...) do { \
    int _saved = g_logLevel.load(); \
    g_logLevel.store(4); \
    logMsg(2, __VA_ARGS__); \
    logFlush(); \
    g_logLevel.store(_saved); \
} while(0)
extern std::atomic<unsigned int> g_debugSampleCounter;
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

#endif
