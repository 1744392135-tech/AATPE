#include <string>
#include <atomic>

const char* STATE_DIR = "/tmp/aatpe_fuzz";

bool writeFile(const char* path, const std::string& content) { return true; }
bool writeFile(const char* p, int v) { return true; }
bool writeFile(const char* p, long long v) { return true; }
bool atomicWriteFile(const char* path, const std::string& content) { return true; }
std::string readFile(const char*) { return {}; }

namespace aatpe_priority {
    bool uclampSupportState() { return false; }
}
