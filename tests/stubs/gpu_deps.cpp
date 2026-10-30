#include "aatped.h"
#include "aatped_types.h"
#include <atomic>
#include <fstream>
#include <string>

std::atomic<float> g_gpuUtilization{0.0f};
std::atomic<const char*> g_gpuType{"unknown"};
GpuState g_gpuState;
const char* STATE_DIR = "/tmp/aatpe_v94_state";
const char* METRICS_DIR = "/tmp/aatpe_v94_state";

Config g_cfg;

std::atomic<int> g_thermalCpu{0};
std::atomic<int> g_thermalHeadroomPct{-1};
namespace aatpe {
__attribute__((weak)) bool aatpeThermalDegraded() { return false; }
}

__attribute__((weak)) void aatpeGpuRefreshUtilForBoost(void) {}

std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return "";
    std::getline(f, s); return s;
}
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int) { return true; }
bool writeFile(const char*, float) { return true; }
bool writeFile(const char*, long long) { return true; }
void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}
int readInt(const char* path, int def) {
    std::ifstream f(path); if (!f) return def;
    int v; f >> v; return f.good() ? v : def;
}
