// fz_thread_stub.cpp - host link deps for fz_threadrules (real loadThreadRules
// from thread_scan.o). Provides g_rules (the real vector the parser swaps into)
// plus the device-side hooks thread_scan.cpp references. Does NOT redefine
// trim (real aatped_utils.o provides it) or CONF_DIR (dirs_light.o provides it).
#include <string>
#include <vector>
#include "aatped.h"
#include "aatped_types.h"
#include "aatped_threadutil.h"
#include "aatpe_recovery.h"
#include "aatpe_placement_mech.h"

std::vector<ThreadRule> g_rules;

std::string aatpeDetectForegroundApp(bool) { return std::string(); }
int findPidByPackage(const std::string&) { return -1; }
bool isKernelThread(const std::string&, int) { return false; }
// readThreadCurrentCpu is provided by aatped_topology.o (in SUPPORT); not stubbed here.

namespace aatpe_recovery { void recoveryErase(int) {} }
namespace aatpe_mech { void aatpeUclampFloorReap(int) {} }
