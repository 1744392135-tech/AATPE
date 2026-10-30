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

namespace aatpe_recovery { void recoveryErase(int) {} }
namespace aatpe_mech { void aatpeUclampFloorReap(int) {} }
