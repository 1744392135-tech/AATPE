#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
std::atomic<int> g_logLevel{3};
void logMsg(int level, const char* fmt, ...){(void)level;(void)fmt;}
void logFlush(){} void logInit(){} void logReinit(){} void logShutdown(){}
#include <cstring>
__attribute__((weak)) const char* aatpeStrError(int err, char* buf, size_t n) {
    const char* s = strerror(err);
    if (buf && n) { snprintf(buf, n, "%s", s ? s : "unknown"); return buf; }
    return s ? s : "unknown";
}
__attribute__((weak)) std::string getModuleDir() { return std::string(); }

#include <sys/stat.h>
__attribute__((weak)) bool writeFileAtomic(const char* , const std::string& ) { return false; }
__attribute__((weak)) bool isValidControlChars(const std::string& ) { return true; }

__attribute__((weak)) std::atomic<int> g_ueventEventCount{0};

__attribute__((weak)) void restoreUserControlMode() {}

namespace aatpe_ml {
__attribute__((weak)) std::string computeDeviceTopologyHash() { return std::string(); }
}
