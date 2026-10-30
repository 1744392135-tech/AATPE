#include <cstdio>
#include <string>
#include "uevent_monitor.h"

using namespace aatpe::event;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

static UeventMessage msg(const std::string& action, const std::string& devpath,
                        const std::string& subsystem = "cpu") {
    UeventMessage m;
    m.action = action;
    m.devpath = devpath;
    m.subsystem = subsystem;
    m.type = UeventEventType::UNKNOWN;
    return m;
}

int main() {
    printf("=== R2-L4 uevent /cpu hotplug tightening ===\n");

    CHECK(UeventMonitor::classifyMessage(
              msg("change", "/devices/system/cpu/cpu0/online"))
              == UeventEventType::HOTPLUG,
          "/cpu0/online change -> HOTPLUG");
    CHECK(UeventMonitor::classifyMessage(
              msg("change", "/devices/system/cpu/cpu128"))
              == UeventEventType::HOTPLUG,
          "/cpu128 change -> HOTPLUG");
    CHECK(UeventMonitor::classifyMessage(
              msg("add", "/devices/system/cpu/cpu3"))
              == UeventEventType::HOTPLUG,
          "/cpu3 add -> HOTPLUG");
    CHECK(UeventMonitor::classifyMessage(
              msg("remove", "/devices/system/cpu/cpu7"))
              == UeventEventType::HOTPLUG,
          "/cpu7 remove -> HOTPLUG");

    CHECK(UeventMonitor::classifyMessage(
              msg("change", "/devices/system/cpu/cpu0/cpufreq"))
              == UeventEventType::FREQ_CHANGE,
          "cpu0/cpufreq change -> FREQ_CHANGE (not HOTPLUG)");

    CHECK(UeventMonitor::classifyMessage(
              msg("change", "/devices/system/cpu/cpuidle"))
              != UeventEventType::HOTPLUG,
          "/cpu/cpuidle change -> NOT HOTPLUG (was false positive)");
    CHECK(UeventMonitor::classifyMessage(
              msg("change", "/devices/system/cpu/cpuidle"))
              == UeventEventType::UNKNOWN,
          "/cpu/cpuidle change -> UNKNOWN");
    CHECK(UeventMonitor::classifyMessage(
              msg("change", "/devices/system/cpu"))
              != UeventEventType::HOTPLUG,
          "bare /devices/system/cpu container change -> NOT HOTPLUG");
    CHECK(UeventMonitor::classifyMessage(
              msg("change", "/devices/system/cpu/cpufreq/policy0"))
              == UeventEventType::FREQ_CHANGE,
          "cpufreq/policy0 -> FREQ_CHANGE");

    printf("=== R2-L4 result: pass=%d fail=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
