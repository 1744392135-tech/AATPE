// topology_deps.cpp - extra host link deps for aatped_topology.cpp. v94_daemon_deps
// supplies pools/cfg/readFile/readInt/writeFile; this adds trim() and the
// topology generation counter that live elsewhere on device.
#include "aatped.h"
void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}
std::atomic<uint64_t> g_topologyGeneration{0};

// atomicWriteFile lives in aatped_utils.cpp on device; v94_daemon_deps only
// provides the plain writeFile overloads. Provide them here so topology.cpp
// links without pulling in utils.cpp (which would redefine readFile/writeFile).
bool atomicWriteFile(const char*, const std::string&) { return true; }
bool atomicWriteFile(const char*, int) { return true; }
// initScoringArrays lives in aatped_control.cpp; topology calls it after a
// hotplug re-detect. Neutral no-op for the topology unit test.
void initScoringArrays(int) {}
// H13 polish: detectCpuTopology() now calls seedOnlineCacheFromSysfs() (real home
// is placement.cpp). The topology hermetic recipe does NOT link placement.o, so
// provide a host no-op; on host sysfs is the mock tree and the h13 recipe (which
// links placement.o) uses the real definition.
void seedOnlineCacheFromSysfs() {}
