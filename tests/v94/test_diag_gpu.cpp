// v4.1.2 hotfix host test: aatpe_diag GPU node-access + empty-freq behavior.
// Reproduces the on-device scenario (Redmi chopin, KSU uid=0, SELinux enforcing):
//   - gpu_utilization reads a non-empty value  -> must count as R
//   - current_freqency reads EMPTY (SELIMNUX returns empty under root shell)
//     -> must NOT be treated as an available frequency (no fake 0/0MHz)
//   - under forced-root, access(W_OK) must NOT be trusted -> no node judged W,
//     gpu ctrl must stay 0. We never write to a node.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "aatpe_diag.h"

// ---- stubs for deps not under test ----
std::atomic<int> g_logLevel{0};
std::atomic<unsigned int> g_debugSampleCounter{0};
void logMsg(int, const char*, ...) {}
void logInit() {} void logReinit() {} void logFlush() {} void logShutdown() {} void logClear() {}
// atomicWriteFile stub: just remember it was called, do not actually write.
bool atomicWriteFile(const char* path, const std::string& content) {
    (void)path; (void)content; return true;
}

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); ++g_failures; } \
} while (0)

static void writeFile(const std::string& path, const std::string& content) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { perror(path.c_str()); _exit(2); }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
}
static void mkDir(const std::string& p) { mkdir(p.c_str(), 0755); }

// Count R/W/N occurrences in a probe line "name=R;name=W:...".
static void countProbe(const std::string& probe, int& r, int& w, int& n) {
    r = w = n = 0;
    size_t pos = 0;
    while (pos < probe.size()) {
        size_t semi = probe.find(';', pos);
        std::string e = probe.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        size_t eq = e.find('=');
        if (eq != std::string::npos) {
            char st = e[eq + 1];
            if (st == 'R') r++; else if (st == 'W') w++; else n++;
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
}

int main() {
    const char* root = "v412_testroot";
    // Build fake sys tree.
    mkDir(root);
    mkDir(std::string(root) + "/sys");
    mkDir(std::string(root) + "/sys/fs");
    mkDir(std::string(root) + "/sys/fs/selinux");
    mkDir(std::string(root) + "/sys/kernel");
    mkDir(std::string(root) + "/sys/kernel/ged");
    mkDir(std::string(root) + "/sys/kernel/ged/hal");
    mkDir(std::string(root) + "/proc");

    // ---- Scenario A: forced root + SELinux enforcing (the real device) ----
    writeFile(std::string(root) + "/.aatpe_env_root", "1\n");      // force euid==0
    writeFile(std::string(root) + "/sys/fs/selinux/enforce", "1\n");
    // util node: readable, non-empty.
    writeFile(std::string(root) + "/sys/kernel/ged/hal/gpu_utilization", "22\n");
    // All other GED nodes exist but read EMPTY (SELinux denies cat under root shell).
    const char* empties[] = {
        "current_freqency", "previous_freqency", "gpu_boost_level",
        "custom_boost_gpu_freq", "custom_upbound_gpu_freq", "total_gpu_freq_level_count",
        "dvfs_loading_mode", "dvfs_margin_value", "ged_kpi", "loading_base_dvfs_step",
        "opp_logs", "timer_base_dvfs_margin"
    };
    for (const char* e : empties)
        writeFile(std::string(root) + "/sys/kernel/ged/hal/" + e, "");

    aatpe::diag::setTestRoot(root);
    aatpe::diag::collect();
    aatpe::diag::SystemPressure s = aatpe::diag::getSystemPressureSnapshot();

    printf("Scenario A: root + SELinux enforcing, util=22, all other nodes empty\n");
    CHECK(s.gpuUtilPct == 22, "gpu_utilization read non-empty -> gpuUtilPct==22");
    CHECK(s.gpuFreqAvail == false, "current_freqency empty -> freq NOT available (no fake MHz)");
    CHECK(s.gpuFreqKhz == 0, "gpuFreqKhz==0 when no usable frequency");
    CHECK(s.gpuCtrl == false, "under root we never claim a writable control node (ctrl==0)");
    CHECK(s.gpuDevName.find("gpu_utilization") != std::string::npos,
          "source falls back to gpu_utilization (NOT current_freqency)");
    int r, w, n; countProbe(s.gpuProbe, r, w, n);
    printf("    probe counts: R=%d W=%d N=%d  (line: %s)\n", r, w, n, s.gpuProbe.c_str());
    CHECK(w == 0, "enforcing/root: zero nodes judged writable (W==0)");
    CHECK(r == 1, "only the non-empty util node counts as R (R==1)");
    // 12 empty GED nodes + 4 absent platform-device entries = 16 N. Total 17 nodes,
    // matching the on-device enumeration (was R0/W17/N0 before the hotfix).
    CHECK(n == 16, "12 empty GED nodes + 4 absent plat dirs counted as N (N==16)");
    CHECK(s.gpuProbe.find("=W") == std::string::npos, "probe line contains no '=W'");

    aatpe::diag::setTestRoot("");  // reset
    printf("\n%s\n", g_failures ? "RESULT: FAILURES PRESENT" : "RESULT: ALL PASS");
    return g_failures ? 1 : 0;
}
