#include <string>

namespace aatpe { namespace caps {
__attribute__((weak)) int writeTidToCgroupGroup(int, const std::string&) { return 0; }
__attribute__((weak)) int writeTidToCpuCgroupGroup(int, const std::string&) { return 0; }
}}
