// gpu_deps.cpp - host link deps for aatped_gpu.cpp. Provides the externs
// gpu.cpp references but does not itself define.
#include "aatped.h"
#include "aatped_types.h"
#include <atomic>
#include <fstream>
#include <string>

// P1-6: atomic<float> to match production type.
std::atomic<float> g_gpuUtilization{0.0f};
std::atomic<const char*> g_gpuType{"unknown"};
GpuState g_gpuState;
const char* STATE_DIR = "/tmp/aatpe_v94_state";
const char* METRICS_DIR = "/tmp/aatpe_v94_state";

// Global config object (production default values from aatped_types.h)
Config g_cfg;

// T-01/T-03 thermal gate inputs consumed by gpu_boost.cpp's gpuBoostTick().
// In production these are written by thermal.cpp's 1s sysfs scan. On host they
// default to "cold / healthy" so existing gpu_boost tests (which do not exercise
// gpuBoostTick()) are unaffected; the new thermal-gate test overrides them.
std::atomic<int> g_thermalCpu{0};
std::atomic<int> g_thermalHeadroomPct{-1};
// Weak: host tests can override to simulate sensor outage (T-06).
namespace aatpe {
__attribute__((weak)) bool aatpeThermalDegraded() { return false; }
}

// F-07: weak no-op stub. Recipes that link aatped_gpu.cpp (the `gpu` recipe)
// get the strong real implementation; gpu_boost-only recipes link this stub.
__attribute__((weak)) void aatpeGpuRefreshUtilForBoost(void) {}

// aatped_gpu.cpp (KGSL+Mali dual platform) uses these utility externs.
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
