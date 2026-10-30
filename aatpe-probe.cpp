
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/statfs.h>
#include "aatpe-constants.h"
#include "aatpe-version.h"

using namespace aatpe;

static constexpr const char* kDumpsysPath = "/system/bin/dumpsys";
static constexpr const char* kTasksetPath = "/system/bin/taskset";
static constexpr const char* kVersionPath = "/dev/aatpe/version";
#define kPidPath aatpe::paths::kPidFile

#define PROBE_VERSION AATPE_VERSION

const char* RED    = "\033[0;31m";
const char* GREEN  = "\033[0;32m";
const char* YELLOW = "\033[0;33m";
const char* BLUE   = "\033[0;34m";
const char* CYAN   = "\033[0;36m";
const char* NC     = "\033[0m";

bool g_useColor = true;
const char* color(const char* c) { return g_useColor ? c : ""; }

int g_pass = 0, g_warn = 0, g_fail = 0, g_total = 0;

std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

int readInt(const char* path, int def) {
    std::string s = readFile(path);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

void probeItem(const char* section, const char* item, const char* result,
               const char* reason, const char* advice, const char* detail) {
    g_total++;
    if (strcmp(result, "OK") == 0) {
        g_pass++;
        printf("%s[OK]%s %s.%s\n", color(GREEN), color(NC), section, item);
    } else if (strcmp(result, "WARN") == 0) {
        g_warn++;
        printf("%s[WARN]%s %s.%s\n", color(YELLOW), color(NC), section, item);
        if (reason && strlen(reason) > 0) printf("   %s原因:%s %s\n", color(YELLOW), color(NC), reason);
        if (advice && strlen(advice) > 0) printf("   %s建议:%s %s\n", color(BLUE), color(NC), advice);
    } else if (strcmp(result, "FAIL") == 0) {
        g_fail++;
        printf("%s[FAIL]%s %s.%s\n", color(RED), color(NC), section, item);
        if (reason && strlen(reason) > 0) printf("   %s原因:%s %s\n", color(YELLOW), color(NC), reason);
        if (advice && strlen(advice) > 0) printf("   %s建议:%s %s\n", color(BLUE), color(NC), advice);
    } else if (strcmp(result, "INFO") == 0) {
        printf("%s[INFO]%s %s.%s", color(CYAN), color(NC), section, item);
        if (detail && strlen(detail) > 0) printf(" %s", detail);
        printf("\n");
    }
}

void probeBasic() {
    printf("===================================================================\n");
    printf("  SECTION 0: 基础环境\n");
    printf("===================================================================\n\n");

    struct utsname u;
    memset(&u, 0, sizeof(u));
    int unameRet = uname(&u);
    const char* kernelRelease = (unameRet == 0 && u.release[0] != '\0') ? u.release : "unknown";
    probeItem("0", "内核版本", "INFO", nullptr, nullptr, kernelRelease);

    {
        std::string arch = readFile("/proc/cpuinfo");

        auto extractField = [&](const char* field) -> std::string {
            size_t p = arch.find(field);
            if (p == std::string::npos) return "";
            size_t colon = arch.find(':', p);
            if (colon == std::string::npos) return "";
            size_t nl = arch.find('\n', colon);
            if (nl == std::string::npos) nl = arch.size();
            std::string v = arch.substr(colon + 1, nl - colon - 1);
            size_t s = v.find_first_not_of(" \t\r\n");
            size_t e = v.find_last_not_of(" \t\r\n");
            if (s == std::string::npos) return "";
            return v.substr(s, e - s + 1);
        };

        std::string implementer = extractField("CPU implementer");
        std::string part        = extractField("CPU part");
        std::string variant     = extractField("CPU variant");
        std::string revision    = extractField("CPU revision");

        if (!implementer.empty()) {
            std::string detail = "ARM64 (aarch64)";
            detail += " implementer=" + implementer;
            if (!part.empty())     detail += " part=" + part;
            if (!variant.empty())  detail += " variant=" + variant;
            if (!revision.empty()) detail += " rev=" + revision;
            probeItem("0", "CPU架构", "OK", nullptr, nullptr, detail.c_str());
        } else {
            size_t pos = arch.find("CPU architecture");
            if (pos != std::string::npos) {
                size_t nl = arch.find('\n', pos);
                if (nl == std::string::npos) nl = arch.size();
                std::string line = arch.substr(pos, nl - pos);
                probeItem("0", "CPU架构", "OK", nullptr, nullptr, line.c_str());
            } else if (u.machine[0] != '\0') {
                std::string detail = std::string("Unknown cpuinfo layout; uname machine=") + u.machine;
                probeItem("0", "CPU架构", "WARN", "cpuinfo arch fields absent",
                          "Using uname() fallback", detail.c_str());
            } else {
                probeItem("0", "CPU架构", "FAIL", "Cannot detect CPU architecture",
                          "cpuinfo and uname both unavailable", "Unknown CPU architecture");
            }
        }
    }

    std::string selinux = readFile("/sys/fs/selinux/enforce");
    if (selinux == "1") {
        probeItem("0", "SELinux", "WARN", "可能阻止某些操作", "如需调试可临时设为permissive", "Enforcing");
    } else {
        probeItem("0", "SELinux", "OK", nullptr, nullptr, "Permissive/Disabled");
    }

    std::ifstream mf("/proc/meminfo");
    if (mf) {
        std::string line;
        while (std::getline(mf, line)) {
            if (line.find("MemAvailable:") == 0) {
                size_t p = line.find_first_of("0123456789");
                if (p != std::string::npos) {
                    try {
                        int mem = std::stoi(line.substr(p));
                        probeItem("0", "内存可用", "OK", nullptr, nullptr, (std::to_string(mem) + "KB").c_str());
                    } catch (...) {
                        probeItem("0", "内存可用", "WARN", "Failed to parse MemAvailable", "Check /proc/meminfo format", "parse error");
                    }
                }
                break;
            }
        }
    }

    struct statfs st;
    if (statfs("/data", &st) == 0) {
        unsigned long long avail = (unsigned long long)st.f_bavail * st.f_bsize / 1024;
        probeItem("0", "存储可用", "OK", nullptr, nullptr, (std::to_string(avail) + "KB").c_str());
    }

    printf("\n");
}

void probeCpu() {
    printf("===================================================================\n");
    printf("  SECTION 1: CPU 拓扑与调度\n");
    printf("===================================================================\n\n");

    int cpuCount = 0;
    DIR* dir = opendir("/sys/devices/system/cpu");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "cpu", 3) != 0) continue;
            errno = 0;
            char* end = nullptr;
            long cpuIdL = strtol(entry->d_name + 3, &end, 10);
            if (*end != '\0' || errno == ERANGE || cpuIdL < 0 || cpuIdL > 1023) continue;
            int cpuId = (int)cpuIdL;

            std::string base = std::string("/sys/devices/system/cpu/") + entry->d_name;
            int cap = readInt((base + "/cpu_capacity").c_str(), -1);
            int freq = readInt((base + "/cpufreq/scaling_max_freq").c_str(), -1);

            char detail[128];
            snprintf(detail, sizeof(detail), "capacity=%d max_freq=%d", cap, freq);
            char item[32];
            snprintf(item, sizeof(item), "CPU%d", cpuId);
            probeItem("1", item, "INFO", nullptr, nullptr, detail);
            cpuCount++;
        }
        closedir(dir);
    }

    if (cpuCount > 0) {
        probeItem("1", "CPU总数", "OK", nullptr, nullptr, (std::to_string(cpuCount)).c_str());
    }

    std::string gov = readFile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (!gov.empty()) {
        probeItem("1", "调度器", "INFO", nullptr, nullptr, gov.c_str());
    }

    struct stat st;
    if (stat("/sys/devices/system/cpu/cpu0/cpufreq/schedutil/hispeed_freq", &st) == 0) {
        probeItem("1", "EAS", "OK", nullptr, nullptr, "已启用");
    }

    if (stat("/proc/pressure", &st) == 0) {
        probeItem("1", "PSI", "OK", nullptr, nullptr, "可用");
    }

    if (stat("/proc/sys/kernel/sched_uclamp_util_min", &st) == 0) {
        probeItem("1", "uclamp", "OK", nullptr, nullptr, "支持");
    } else {
        probeItem("1", "uclamp", "WARN", "内核不支持uclamp", "V1-V2部分功能受限", "不支持");
    }

    printf("\n");
}

void probeThermal() {
    printf("===================================================================\n");
    printf("  SECTION 2: 热管理\n");
    printf("===================================================================\n\n");

    int tzCount = 0;
    DIR* dir = opendir("/sys/class/thermal");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;
            std::string tempPath = std::string("/sys/class/thermal/") + entry->d_name + "/temp";
            int t = readInt(tempPath.c_str(), -1);
            if (t >= 0) tzCount++;
        }
        closedir(dir);
    }

    if (tzCount > 0) {
        probeItem("2", "热区数量", "OK", nullptr, nullptr, (std::to_string(tzCount)).c_str());
    } else {
        probeItem("2", "热区数量", "FAIL", "无热区可读", "热管理将受限", "0");
    }

    struct stat st;
    if (stat("/sys/class/power_supply/battery/temp", &st) == 0) {
        probeItem("2", "电池温度", "OK", nullptr, nullptr, "可读");
    }

    printf("\n");
}

void probeFrame() {
    printf("===================================================================\n");
    printf("  SECTION 3: V3 帧感知\n");
    printf("===================================================================\n\n");

    int sfPid = 0;
    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            int pid = parseIntSafe(entry->d_name);
            if (pid <= 0) continue;
            std::string commPath = "/proc/" + std::to_string(pid) + "/comm";
            std::string comm = readFile(commPath.c_str());
            if (comm == "surfaceflinger") {
                sfPid = pid;
                break;
            }
        }
        closedir(dir);
    }

    if (sfPid > 0) {
        probeItem("3", "SurfaceFlinger", "OK", nullptr, nullptr, ("PID=" + std::to_string(sfPid)).c_str());
    } else {
        probeItem("3", "SurfaceFlinger", "FAIL", "SF未运行", "V3帧感知不可用", nullptr);
    }

    struct stat st;
    if (stat(kDumpsysPath, &st) == 0) {
        probeItem("3", "dumpsys", "OK", nullptr, nullptr, "可用");
    } else {
        probeItem("3", "dumpsys", "WARN", "不可用", "需要前台应用才能读取", nullptr);
    }

    printf("\n");
}

void probeControl() {
    printf("===================================================================\n");
    printf("  SECTION 4: 控制接口\n");
    printf("===================================================================\n\n");

    struct stat st;
    if (stat("/dev/cpuset", &st) == 0 && S_ISDIR(st.st_mode)) {
        probeItem("4", "cpuset", "OK", nullptr, nullptr, "可用");
    } else {
        probeItem("4", "cpuset", "FAIL", "cpuset不可用", "核心功能受限", nullptr);
    }

    if (stat(kTasksetPath, &st) == 0) {
        probeItem("4", "taskset", "OK", nullptr, nullptr, "可用");
    }

    if (stat("/sys/fs/cgroup", &st) == 0 && S_ISDIR(st.st_mode)) {
        probeItem("4", "cgroup", "OK", nullptr, nullptr, "可用");
    }

    printf("\n");
}

void probeAatpe() {
    printf("===================================================================\n");
    printf("  SECTION 5: AATPE 状态\n");
    printf("===================================================================\n\n");

    std::string version = readFile(kVersionPath);
    if (version.empty()) version = PROBE_VERSION;
    probeItem("5", "AATPE版本", "INFO", nullptr, nullptr, version.c_str());

    int dpid = readInt(kPidPath, 0);
    std::string daemonComm;
    bool daemonLooksAlive = false;
    if (dpid > 0 && kill(dpid, 0) == 0) {
        std::string commPath = "/proc/" + std::to_string(dpid) + "/comm";
        daemonComm = readFile(commPath.c_str());
        daemonLooksAlive = (daemonComm == "aatped");
    }
    if (daemonLooksAlive) {
        probeItem("5", "Daemon", "OK", nullptr, nullptr, "运行中");
    } else if (dpid > 0 && kill(dpid, 0) == 0) {
        probeItem("5", "Daemon", "WARN",
                  ("PID " + std::to_string(dpid) + " alive but comm=" + daemonComm +
                   " (not aatped; PID reuse?)").c_str(),
                  "daemon not actually running", nullptr);
    } else {
        probeItem("5", "Daemon", "WARN", "未运行", "安装后重启生效", nullptr);
    }

    printf("\n");
}

void printSummary() {
    printf("===================================================================\n");
    printf("  综合评估\n");
    printf("===================================================================\n\n");
    printf("总探测项: %d\n", g_total);
    printf("  通过: %s%d%s\n", color(GREEN), g_pass, color(NC));
    printf("  警告: %s%d%s\n", color(YELLOW), g_warn, color(NC));
    printf("  失败: %s%d%s\n", color(RED), g_fail, color(NC));
    printf("\n");

    if (g_fail == 0 && g_warn <= probe::kFullCompatMaxWarn) {
        printf("%s评估: 设备完全兼容 AATPE V1-V3%s\n", color(GREEN), color(NC));
    } else if (g_fail <= probe::kBasicCompatMaxFail) {
        printf("%s评估: 设备基本兼容，部分高级功能受限%s\n", color(YELLOW), color(NC));
    } else {
        printf("%s评估: 设备兼容性较差，建议检查内核/Root环境%s\n", color(RED), color(NC));
    }
    printf("\n");

    printf("推荐部署:\n");
    printf("  1. 安装 AATPE 模块并重启\n");
    printf("  2. 先用 passive 模式观察\n");
    printf("  3. 24小时后切换到 assist 模式\n");
    printf("  4. 安装 AATPE-Helper APK 解锁完整 V3 帧感知\n");
    printf("\n");
}

int probeExitCode() {
    return (g_fail > probe::kBasicCompatMaxFail) ? 1 : 0;
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v" || a == "version") {
            printf("%s\n", PROBE_VERSION);
            return 0;
        }
        if (a == "--help" || a == "-h" || a == "help") {
            printf("aatpe-probe %s — read-only AATPE environment probe\n", PROBE_VERSION);
            printf("Usage: aatpe-probe [--version|--help]\n");
            printf("Exit code: 0 = compatible, 1 = compatibility poor (see summary).\n");
            return 0;
        }
    }

    if (!isatty(STDOUT_FILENO)) g_useColor = false;

    printf("\n");
    printf("+=================================================================+\n");
    printf("|         AATPE V3 Environment Probe                              |\n");
    printf("|         C++ Edition | Hardware Adaptive | %s         |\n", PROBE_VERSION);
    printf("+=================================================================+\n");
    printf("\n");

    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    printf("开始时间: %s\n", ts);

    struct utsname u;
    memset(&u, 0, sizeof(u));
    if (uname(&u) != 0) { u.release[0] = '\0'; u.machine[0] = '\0'; }
    printf("内核: %s\n", u.release[0] ? u.release : "unknown");
    printf("架构: %s\n", u.machine[0] ? u.machine : "unknown");
    printf("\n");

    probeBasic();
    probeCpu();
    probeThermal();
    probeFrame();
    probeControl();
    probeAatpe();
    printSummary();

    return probeExitCode();
}
