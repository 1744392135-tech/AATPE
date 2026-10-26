// stub_a3.cpp — host link deps for aatpe_caps.o (autocal path only).
#include <string>
const char* STATE_DIR = "/tmp/aatpe_v42_a3";
std::string readFile(const char*) { return std::string(); }
bool writeFile(const char*, const std::string&) { return true; }
bool atomicWriteFile(const char*, const std::string&) { return true; }
// caps.cpp references this only when the test fingerprint override is empty;
// provide a definition so the link resolves.
namespace aatpe_ml { std::string computeDeviceTopologyHash() { return "topo-real"; } }
