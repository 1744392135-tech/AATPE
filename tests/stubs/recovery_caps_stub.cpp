// recovery_caps_stub.cpp - host link deps for aatpe_recovery.o's D1 restore path.
//
// D1 (v4.2): aatpe_recovery.cpp now calls aatpe::caps::writeTidToCgroupGroup /
// writeTidToCpuCgroupGroup when restoring a record that carries a cgroup membership
// (restoreRc|=8). Recipes that link aatpe_recovery.o but NOT the real aatpe_caps.o
// (recovery, coldstart, main) need these symbols resolved on the host. The live
// cgroup write requires a real /proc/<pid>/task/<tid>; on host we neutral-stub.
//
// weak so a recipe that ALSO links the real aatpe_caps.o (placement graph) wins
// instead of a multiple-definition error; the recovery/coldstart/main recipes do
// NOT link aatpe_caps.o, so the weak definition is the one they resolve.
#include <string>

namespace aatpe { namespace caps {
__attribute__((weak)) int writeTidToCgroupGroup(int, const std::string&) { return 0; }
__attribute__((weak)) int writeTidToCpuCgroupGroup(int, const std::string&) { return 0; }
}}
