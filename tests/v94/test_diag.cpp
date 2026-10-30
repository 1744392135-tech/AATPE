#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "aatpe_diag.h"

bool atomicWriteFile(const char* path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static void mkdirp(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == '/' || i == p.size() - 1) mkdir(cur.c_str(), 0755);
    }
}
static void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path); f << content;
}
static void writeRo(const std::string& path, const std::string& content) {
    writeFile(path, content);
    chmod(path.c_str(), 0444);
}
static std::string kv(const std::string& line, const std::string& key) {
    std::string tok = key + "=";
    size_t pos = 0;
    while (true) {
        size_t i = line.find(tok, pos);
        if (i == std::string::npos) return "";
        if (i == 0 || line[i-1] == ';') {
            size_t e = line.find(';', i + tok.size());
            return line.substr(i + tok.size(), e == std::string::npos ? e : e - (i + tok.size()));
        }
        pos = i + tok.size();
    }
}
static std::string readAll(const std::string& path) {
    std::ifstream f(path);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
    return s;
}

int main() {
    printf("=== Diag: PSI parsing ===\n");
    {
        char tmp[] = "/tmp/diag_psi_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/proc/pressure");
        writeFile(root + "/proc/pressure/cpu", "some avg10=12.50 avg60=5.00 avg300=1.00 total=123456\n");
        writeFile(root + "/proc/pressure/io", "some avg10=3.25 avg60=1.00 avg300=0.00 total=999\n");
        writeFile(root + "/proc/pressure/mem", "some avg10=7.75 avg60=2.00 avg300=0.00 total=555\n");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.psiAvailable == true, "PSI available");
        CHECK(s.psiCpuSomePct > 12.0f && s.psiCpuSomePct < 13.0f, "PSI cpu avg10=12.5");
        CHECK(s.psiIoSomePct > 3.0f && s.psiIoSomePct < 3.5f, "PSI io avg10=3.25");
        CHECK(s.psiMemSomePct > 7.5f && s.psiMemSomePct < 8.0f, "PSI mem avg10=7.75");
        CHECK(s.psiCpuTotalUsec == 123456, "PSI cpu total=123456");
    }

    printf("\n=== Diag PSI v4.2.1: cpu-only-some, io/mem=0, missing full ===\n");
    {
        char tmp[] = "/tmp/diag_psi2_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/proc/pressure");
        writeFile(root + "/proc/pressure/cpu",
                  "some avg10=7.00 avg60=3.50 avg300=1.20 total=98765\n");
        writeFile(root + "/proc/pressure/io",
                  "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
                  "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
        writeFile(root + "/proc/pressure/mem",
                  "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
                  "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.psiAvailable == true, "PSI available (cpu file present)");
        CHECK(s.psiCpuSomePct > 6.5f && s.psiCpuSomePct < 7.5f, "PSI cpu avg10=7.0 (cpu-only-some, no full line)");
        CHECK(s.psiIoSomePct == 0.0f, "PSI io=0 (no IO stall, normal)");
        CHECK(s.psiMemSomePct == 0.0f, "PSI mem=0 (no reclaim, normal)");
        CHECK(s.psiCpuTotalUsec == 98765, "PSI cpu total=98765");
    }

    printf("\n=== Diag PSI v4.2.1: cpu exists but io/mem files missing ===\n");
    {
        char tmp[] = "/tmp/diag_psi3_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/proc/pressure");
        writeFile(root + "/proc/pressure/cpu",
                  "some avg10=2.50 total=111\n");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.psiAvailable == true, "PSI available (cpu present even though io/mem missing)");
        CHECK(s.psiCpuSomePct > 2.0f && s.psiCpuSomePct < 3.0f, "PSI cpu=2.5");
        CHECK(s.psiIoSomePct == 0.0f, "PSI io defaults 0 when file missing");
        CHECK(s.psiMemSomePct == 0.0f, "PSI mem defaults 0 when file missing");
    }

    printf("\n=== Diag: GPU devfreq parsing ===\n");
    {
        char tmp[] = "/tmp/diag_gpu_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/class/devfreq/fde00000.gpu");
        writeFile(root + "/sys/class/devfreq/fde00000.gpu/cur_freq", "400000\n");
        writeFile(root + "/sys/class/devfreq/fde00000.gpu/available_frequencies", "200000 400000 800000\n");
        writeFile(root + "/sys/class/devfreq/fde00000.gpu/governor", "performance\n");
        writeFile(root + "/sys/class/devfreq/fde00000.gpu/load", "45\n");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.gpuFreqKhz == 400000, "GPU cur_freq=400000");
        CHECK(s.gpuMaxKhz == 800000, "GPU max=800000");
        CHECK(s.gpuUtilPct == 45, "GPU util=45");
        CHECK(s.gpuGovernor == "performance", "GPU governor=performance");
        CHECK(s.gpuThrottled == true, "GPU throttled (400k < 760k=95% of 800k)");
    }

    printf("\n=== Diag: GPU throttled (cur < 95%% max) ===\n");
    {
        char tmp[] = "/tmp/diag_thr_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/class/devfreq/gpu0");
        writeFile(root + "/sys/class/devfreq/gpu0/cur_freq", "400000\n");
        writeFile(root + "/sys/class/devfreq/gpu0/max_freq", "1000000\n");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.gpuThrottled == true, "GPU throttled (400k < 950k)");
    }

    printf("\n=== Diag: no nodes (all missing) ===\n");
    {
        char tmp[] = "/tmp/diag_none_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.psiAvailable == false, "PSI unavailable when /proc/pressure missing");
        CHECK(s.gpuUtilPct == -1, "GPU util=-1 when no node");
        CHECK(s.gpuFreqKhz == 0, "GPU freq=0 when no node");
        CHECK(s.gpuMaxKhz == 0, "GPU max=0 when no node");
    }

    printf("\n=== Diag: writeStateFiles ===\n");
    {
        char tmp[] = "/tmp/diag_state_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/proc/pressure");
        writeFile(root + "/proc/pressure/cpu", "some avg10=10.00 total=42\n");
        mkdirp(root + "/sys/class/devfreq/gpu0");
        writeFile(root + "/sys/class/devfreq/gpu0/cur_freq", "500000\n");
        writeFile(root + "/sys/class/devfreq/gpu0/load", "50\n");
        mkdirp(root + "/state");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        aatpe::diag::writeStateFiles((root + "/state").c_str());
        std::ifstream f(root + "/state/pressure");
        std::string line((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(!line.empty(), "pressure state file written");
        CHECK(line.find("cpu_some=") != std::string::npos, "state file has cpu_some=");
    }

    printf("\n=== Diag ROUND-014 hotfix: gpu_load ctrl=1 (devfreq freq node) ===\n");
    {
        char tmp[] = "/tmp/diag_ctrl1_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/class/devfreq/fde00000.gpu");
        writeFile(root + "/sys/class/devfreq/fde00000.gpu/cur_freq", "650000\n");
        writeFile(root + "/sys/class/devfreq/fde00000.gpu/available_frequencies", "170000 340000 650000\n");
        writeFile(root + "/sys/class/devfreq/fde00000.gpu/governor", "performance\n");
        writeFile(root + "/sys/class/devfreq/fde00000.gpu/load", "45\n");
        mkdirp(root + "/state");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        aatpe::diag::writeStateFiles((root + "/state").c_str());
        std::string line = readAll(root + "/state/gpu_load");
        printf("  gpu_load: %s\n", line.c_str());
        CHECK(kv(line, "ctrl") == "1", "ctrl=1 (devfreq cur_freq readable)");
        CHECK(kv(line, "available") == "1", "available=1 (GPU telemetry present)");
        CHECK(kv(line, "gpu_freq") == "650000", "gpu_freq=650000");
        CHECK(kv(line, "gpu_max") == "650000", "gpu_max=650000 (last of available list)");
        CHECK(kv(line, "governor") == "performance", "governor=performance");
        CHECK(kv(line, "gpu_util") == "45", "gpu_util=45");
        CHECK(kv(line, "source") == "devfreq/fde00000.gpu", "source=devfreq/fde00000.gpu");
    }

    printf("\n=== Diag ROUND-014 hotfix: gpu_load ctrl=0 (util-only, no freq node) ===\n");
    {
        char tmp[] = "/tmp/diag_ctrl0_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/kernel/ged/hal");
        writeFile(root + "/sys/kernel/ged/hal/gpu_utilization", "gpu_utilization: 30\n");
        mkdirp(root + "/state");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        aatpe::diag::writeStateFiles((root + "/state").c_str());
        std::string line = readAll(root + "/state/gpu_load");
        printf("  gpu_load: %s\n", line.c_str());
        CHECK(kv(line, "ctrl") == "0", "ctrl=0 (util-only, no freq node)");
        CHECK(kv(line, "available") == "1", "available=1 (util telemetry present)");
        CHECK(kv(line, "gpu_freq") == "0", "gpu_freq=0 (no freq node)");
        CHECK(kv(line, "gpu_max") == "0", "gpu_max=0");
        CHECK(kv(line, "gpu_util") == "30", "gpu_util=30");
        CHECK(kv(line, "source") == "ged/hal/gpu_utilization", "source=ged/hal/gpu_utilization");
    }

    printf("\n=== Diag H5: GED-only GPU enumeration (util + current_freqency, no write node) ===\n");
    {
        char tmp[] = "/tmp/hotfix_g1_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/kernel/ged/hal");
        writeRo(root + "/sys/kernel/ged/hal/gpu_utilization", "gpu_utilization: 42\n");
        writeRo(root + "/sys/kernel/ged/hal/current_freqency", "700\n");
        writeRo(root + "/sys/kernel/ged/hal/previous_freqency", "650\n");
        writeRo(root + "/sys/kernel/ged/hal/total_gpu_freq_level_count", "12\n");
        mkdirp(root + "/state");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        aatpe::diag::writeStateFiles((root + "/state").c_str());
        std::string line = readAll(root + "/state/gpu_load");
        std::string probe = readAll(root + "/state/gpu_probe");
        printf("  gpu_load: %s\n", line.c_str());
        printf("  gpu_probe: %s\n", probe.c_str());
        CHECK(kv(line, "available") == "1", "H5-1: available=1 (GED telemetry present)");
        CHECK(kv(line, "ctrl") == "0", "H5-1: ctrl=0 (no writable control node)");
        CHECK(kv(line, "freq_avail") == "1", "H5-1: freq_avail=1 (current_freqency read)");
        CHECK(kv(line, "gpu_freq") == "700000", "H5-1: gpu_freq=700000 (700MHz->kHz)");
        CHECK(kv(line, "freq_unit") == "khz", "H5-1: freq_unit=khz");
        CHECK(kv(line, "gpu_util") == "42", "H5-1: gpu_util=42");
        CHECK(kv(line, "source") == "ged/hal/current_freqency", "H5-1: source=ged/hal/current_freqency");
        CHECK(probe.find("util=R") != std::string::npos, "H5-1: probe util=R");
        CHECK(probe.find("curfreq=R:700") != std::string::npos, "H5-1: probe curfreq=R:700");
        CHECK(probe.find("boost=N") != std::string::npos, "H5-1: probe boost=N (absent)");
        CHECK(probe.find("upbound=N") != std::string::npos, "H5-1: probe upbound=N (absent)");
    }

    printf("\n=== Diag H5: GED + writable custom_boost_gpu_freq -> ctrl=1 ===\n");
    {
        char tmp[] = "/tmp/hotfix_g2_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/kernel/ged/hal");
        writeRo(root + "/sys/kernel/ged/hal/gpu_utilization", "gpu_utilization: 55\n");
        writeRo(root + "/sys/kernel/ged/hal/current_freqency", "400000\n");
        writeFile(root + "/sys/kernel/ged/hal/custom_boost_gpu_freq", "700000\n");
        mkdirp(root + "/state");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        aatpe::diag::writeStateFiles((root + "/state").c_str());
        std::string line = readAll(root + "/state/gpu_load");
        std::string probe = readAll(root + "/state/gpu_probe");
        printf("  gpu_load: %s\n", line.c_str());
        printf("  gpu_probe: %s\n", probe.c_str());
        CHECK(kv(line, "ctrl") == "1", "H5-2: ctrl=1 (writable custom_boost_gpu_freq)");
        CHECK(kv(line, "gpu_freq") == "400000", "H5-2: gpu_freq=400000 (kHz as-is)");
        CHECK(kv(line, "freq_avail") == "1", "H5-2: freq_avail=1");
        CHECK(probe.find("boost=W") != std::string::npos, "H5-2: probe boost=W (writable)");
    }

    printf("\n=== Diag H5: all nodes absent -> available=0 ===\n");
    {
        char tmp[] = "/tmp/hotfix_g3_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/state");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        aatpe::diag::writeStateFiles((root + "/state").c_str());
        std::string line = readAll(root + "/state/gpu_load");
        printf("  gpu_load: %s\n", line.c_str());
        CHECK(kv(line, "available") == "0", "H5-3: available=0 (no nodes)");
        CHECK(kv(line, "ctrl") == "0", "H5-3: ctrl=0");
        CHECK(kv(line, "freq_avail") == "0", "H5-3: freq_avail=0");
        CHECK(kv(line, "gpu_freq") == "0", "H5-3: gpu_freq=0");
    }

    printf("\n=== Diag H5: GED current_freqency as OPP index -> level, no fake MHz ===\n");
    {
        char tmp[] = "/tmp/hotfix_g4_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/kernel/ged/hal");
        writeRo(root + "/sys/kernel/ged/hal/gpu_utilization", "gpu_utilization: 10\n");
        writeRo(root + "/sys/kernel/ged/hal/current_freqency", "3\n");
        mkdirp(root + "/state");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        aatpe::diag::writeStateFiles((root + "/state").c_str());
        std::string line = readAll(root + "/state/gpu_load");
        printf("  gpu_load: %s\n", line.c_str());
        CHECK(s.gpuFreqIsLevel == true, "H5-4: freq classified as level");
        CHECK(kv(line, "gpu_freq") == "0", "H5-4: gpu_freq=0 (no fabricated MHz)");
        CHECK(kv(line, "freq_unit") == "level", "H5-4: freq_unit=level");
        CHECK(kv(line, "freq_raw") == "3", "H5-4: freq_raw=3");
        CHECK(kv(line, "freq_avail") == "1", "H5-4: freq_avail=1 (we still read a current level)");
    }

    printf("\n=== Diag WD-03: missing cpufreq dir skips (continue), not break ===\n");
    {
        char tmp[] = "/tmp/diag_cpu_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/devices/system/cpu/cpu0/cpufreq");
        writeFile(root + "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "2000000\n");
        writeFile(root + "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "2000000\n");
        mkdirp(root + "/sys/devices/system/cpu/cpu2/cpufreq");
        writeFile(root + "/sys/devices/system/cpu/cpu2/cpufreq/cpuinfo_max_freq", "2500000\n");
        writeFile(root + "/sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq", "2500000\n");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.cpuHwMaxKhz == 2500000, "WD-03: cpu2 scanned despite missing cpu1 (hwMax=2.5GHz)");
        CHECK(s.cpuCapKhz == 2000000, "WD-03: curCap=min scaling_max across scanned cores (2.0GHz)");
    }

    printf("\n=== Diag WD-07: online CPU enumeration (no hardcoded cpu<16) ===\n");
    {
        char tmp[] = "/tmp/diag_cpuN_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/devices/system/cpu");
        writeFile(root + "/sys/devices/system/cpu/online", "0-19\n");
        for (int c = 0; c < 20; c++) {
            mkdirp(root + "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq");
            writeFile(root + "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/cpuinfo_max_freq",
                      "2000000\n");
            writeFile(root + "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/scaling_max_freq",
                      "2000000\n");
        }
        writeFile(root + "/sys/devices/system/cpu/cpu19/cpufreq/cpuinfo_max_freq", "3000000\n");
        writeFile(root + "/sys/devices/system/cpu/cpu19/cpufreq/scaling_max_freq", "2500000\n");

        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.cpuHwMaxKhz == 3000000,
              "WD-07: cpu19 scanned (hwMax=3.0GHz from >16-core part)");
        CHECK(s.cpuCapKhz == 2000000,
              "WD-07: curCap=min scaling_max across all 20 cores (2.0GHz)");
    }

    printf("\n=== Diag WD-07b: online mask with gaps (\"0-3,7,16-19\") ===\n");
    {
        char tmp[] = "/tmp/diag_cpuG_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/sys/devices/system/cpu");
        writeFile(root + "/sys/devices/system/cpu/online", "0-3,7,16-19\n");
        int expected[] = {0,1,2,3,7,16,17,18,19};
        for (int c : expected) {
            mkdirp(root + "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq");
            writeFile(root + "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/cpuinfo_max_freq",
                      "2200000\n");
            writeFile(root + "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/scaling_max_freq",
                      "2200000\n");
        }
        writeFile(root + "/sys/devices/system/cpu/cpu19/cpufreq/cpuinfo_max_freq", "3100000\n");
        aatpe::diag::setTestRoot(root);
        aatpe::diag::collect();
        auto s = aatpe::diag::getSystemPressureSnapshot();
        CHECK(s.cpuHwMaxKhz == 3100000, "WD-07b: cpu19 in sparse online list scanned");
    }

    printf("\n=== Diag WD-04: rq_wait_top wired (setRqWaitTop -> writeStateFiles) ===\n");
    {
        char tmp[] = "/tmp/diag_rqw_XXXXXX";
        mkdirp(mkdtemp(tmp));
        std::string root = tmp;
        mkdirp(root + "/state");
        aatpe::diag::setTestRoot(root);
        std::vector<aatpe::diag::SystemPressure::RqWaitEntry> top;
        top.push_back({4821, 120});
        top.push_back({910, 45});
        aatpe::diag::setRqWaitTop(top);
        aatpe::diag::collect();
        aatpe::diag::writeStateFiles((root + "/state").c_str());
        std::string line = readAll(root + "/state/rq_wait_top");
        printf("  rq_wait_top: %s\n", line.c_str());
        CHECK(line.find("4821:120") != std::string::npos, "WD-04: rq_wait_top carries tid 4821 wait 120ms");
        CHECK(line.find("910:45") != std::string::npos, "WD-04: rq_wait_top carries tid 910 wait 45ms");
        aatpe::diag::setRqWaitTop({});
        aatpe::diag::writeStateFiles((root + "/state").c_str());
        std::string cleared = readAll(root + "/state/rq_wait_top");
        CHECK(cleared.empty(), "WD-04: empty top -> empty rq_wait_top");
    }

    printf("\n========================================\n");
    printf("Diag results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
