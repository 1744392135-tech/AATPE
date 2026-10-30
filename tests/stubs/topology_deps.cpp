#include "aatped.h"
void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}
std::atomic<uint64_t> g_topologyGeneration{0};

bool atomicWriteFile(const char*, const std::string&) { return true; }
bool atomicWriteFile(const char*, int) { return true; }
void initScoringArrays(int) {}
void seedOnlineCacheFromSysfs() {}
