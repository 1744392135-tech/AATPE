// utils_deps.cpp - host link deps for aatped_utils.cpp. utils.cpp defines
// readFile/writeFile/readInt/parseCpusetCpuList/etc itself; it only needs the
// CPU-topology globals and getOnlineCpus that live in aatped_topology.cpp on device.
#include "aatped.h"

// opt#1: aatped_utils.cpp's stateCacheNoteWrite() references STATE_DIR to
// decide whether a write belongs to the observable state tree. Provide a host
// default (no real state dir on the hermetic host).
const char* STATE_DIR = "/tmp/aatpe_v94_utils_state";

std::vector<int> g_allCpus{0, 1, 2, 3, 4, 5, 6, 7};

std::vector<int> getOnlineCpus() {
    return g_allCpus;  // host: report all 8 CPUs online
}
