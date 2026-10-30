#include "aatped.h"

const char* STATE_DIR = "/tmp/aatpe_v94_audio";
std::vector<int> g_midPool{3, 4, 5};
std::vector<int> g_bigPool{6, 7};
AudioState g_audioState;
std::mutex g_topologyRwMutex;
std::mutex g_stateDiagMutex;

float readCpuUtilizationSchedstat(int ) { return 10.0f; }

#include <fstream>
#include <sstream>
std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int) { return true; }
bool writeFile(const char*, float) { return true; }
bool writeFile(const char*, long long) { return true; }

std::string aatpeExecArgv(const std::vector<std::string>&, const char*,
                          int*, bool) {
    return std::string();
}
