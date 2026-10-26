// audio_deps.cpp - host link deps for aatped_audio.cpp.
#include "aatped.h"

const char* STATE_DIR = "/tmp/aatpe_v94_audio";
std::vector<int> g_midPool{3, 4, 5};
std::vector<int> g_bigPool{6, 7};
AudioState g_audioState;
// v94 stub-integration: detectAudioPlayback() snapshots the pools under
// g_topologyRwMutex (real home placement.cpp:72) and publishes the audio state
// under g_stateDiagMutex (real home aatped_state.cpp:82). The audio hermetic
// recipe links aatped_audio.o in isolation, so provide both mutexes here.
std::mutex g_topologyRwMutex;
std::mutex g_stateDiagMutex;

float readCpuUtilizationSchedstat(int /*cpu*/) { return 10.0f; }

#include <fstream>
#include <sstream>
// aatped_audio.cpp:28 calls readFile() in the cached-PID verification branch
// (reads /proc/<pid>/cmdline). audio_deps previously provided writeFile() but not
// readFile(), so the audio test failed to link. Provide a real file reader so the
// cached-PID identity check is exercised; do NOT pull in aatped_utils.o (would
// multiply-define writeFile/readInt which this TU already defines).
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

// H11: stub aatpeExecArgv for host test. Returns empty (simulating dumpsys
// unavailable). Real implementation in aatped_utils.cpp does fork/exec.
std::string aatpeExecArgv(const std::vector<std::string>&, const char*,
                          int*, bool) {
    return std::string();
}
