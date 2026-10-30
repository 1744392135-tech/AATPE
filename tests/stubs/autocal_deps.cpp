#include <string>
const char* STATE_DIR = "/tmp/aatpe_v42_a3";
std::string readFile(const char*) { return std::string(); }
bool writeFile(const char*, const std::string&) { return true; }
bool atomicWriteFile(const char*, const std::string&) { return true; }
namespace aatpe_ml { std::string computeDeviceTopologyHash() { return "topo-real"; } }
