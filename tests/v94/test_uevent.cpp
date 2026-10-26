// test_uevent.cpp - Direct unit tests for uevent_monitor.cpp
// Covers: parseUevent (null-separated action@devpath + KEY=VALUE),
//         classifyMessage (FREQ_CHANGE / HOTPLUG / THERMAL_SPIKE / UNKNOWN),
//         counters & start/stop/isActive lifecycle (socket likely unavailable on host).
// Build: g++ -std=c++17 -I<SRC> -I<STUB> test_uevent.cpp uevent_monitor.cpp log_stub.cpp -o t -lpthread
#include <cstdio>
#include <cstring>
#include <string>
#include "uevent_monitor.h"

using namespace aatpe::event;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

// helper: build a null-separated uevent buffer into std::string
static std::string packUevent(std::initializer_list<const char*> parts) {
    std::string out;
    for (const char* p : parts) { out.append(p); out.push_back('\0'); }
    return out;
}

int main() {
    printf("=== Uevent: parseUevent ===\n");
    {
        auto buf = packUevent({
            "change@/devices/system/cpu/cpu0/cpufreq",
            "SUBSYSTEM=cpu",
            "ACTION=change"
        });
        UeventMessage m = UeventMonitor::parseUevent(buf.data(), buf.size());
        CHECK(m.action == "change", "parsed action");
        CHECK(m.devpath == "/devices/system/cpu/cpu0/cpufreq", "parsed devpath");
        CHECK(m.subsystem == "cpu", "parsed SUBSYSTEM");
        CHECK(m.type == UeventEventType::FREQ_CHANGE, "classified FREQ_CHANGE via cpufreq");
    }
    {
        // empty / null input (note: production leaves msg.type uninitialized on
        // this early-return path; caller never uses it. Assert the safe contract.)
        UeventMessage m = UeventMonitor::parseUevent(nullptr, 0);
        CHECK(m.action.empty() && m.devpath.empty(), "null input -> empty action/devpath");
    }
    {
        // action without @
        auto buf2 = packUevent(std::initializer_list<const char*>{"online"});
        UeventMessage m = UeventMonitor::parseUevent(buf2.data(), buf2.size());
        CHECK(m.action == "online" && m.devpath.empty(), "bare action kept, devpath empty");
    }

    printf("=== Uevent: classifyMessage ===\n");
    {
        UeventMessage m;
        m.devpath = "/devices/system/cpu/cpu3"; m.action = "add"; m.subsystem = "cpu";
        CHECK(UeventMonitor::classifyMessage(m) == UeventEventType::HOTPLUG, "add /cpu -> HOTPLUG");
        m.action = "remove";
        CHECK(UeventMonitor::classifyMessage(m) == UeventEventType::HOTPLUG, "remove /cpu -> HOTPLUG");
        // G4-003: on ARM64 a generic subsystem=cpu action=change on a /cpuN node is
        // an online/offline hotplug, NOT a frequency change. It must no longer be
        // mis-routed to FREQ_CHANGE (which the magnitude gate would silently drop).
        m.action = "change";
        CHECK(UeventMonitor::classifyMessage(m) == UeventEventType::HOTPLUG,
              "subsystem=cpu change on /cpuN -> HOTPLUG (G4-003)");
    }
    {
        // G4-003: a cpufreq subtree change is still FREQ_CHANGE even though it
        // contains "/cpu".
        UeventMessage m;
        m.devpath = "/devices/system/cpu/cpu0/cpufreq"; m.action = "change"; m.subsystem = "cpu";
        CHECK(UeventMonitor::classifyMessage(m) == UeventEventType::FREQ_CHANGE,
              "cpufreq subtree change -> FREQ_CHANGE (not hotplug)");
    }
    {
        // G4-003: end-to-end netlink-style buffer for a CPU-online uevent must parse
        // and classify as HOTPLUG (pure-function path, no socket needed on host).
        auto buf = packUevent({
            "change@/devices/system/cpu/cpu3",
            "SUBSYSTEM=cpu",
            "ACTION=change"
        });
        UeventMessage m = UeventMonitor::parseUevent(buf.data(), buf.size());
        CHECK(m.action == "change" && m.devpath == "/devices/system/cpu/cpu3",
              "hotplug uevent parsed");
        CHECK(UeventMonitor::classifyMessage(m) == UeventEventType::HOTPLUG,
              "CPU online uevent buffer -> HOTPLUG (G4-003)");
    }
    {
        UeventMessage m;
        m.devpath = "/devices/virtual/thermal/thermal_zone0"; m.subsystem = "thermal";
        CHECK(UeventMonitor::classifyMessage(m) == UeventEventType::THERMAL_SPIKE, "thermal_zone devpath -> THERMAL");
    }
    {
        UeventMessage m;
        m.devpath = "/devices/usb/001"; m.subsystem = "usb"; m.action = "change";
        CHECK(UeventMonitor::classifyMessage(m) == UeventEventType::UNKNOWN, "usb change -> UNKNOWN");
    }
    {
        // cpu subsystem change but devpath does NOT contain "/cpu" -> not freq
        UeventMessage m;
        m.devpath = "/devices/virtual/misc/timer"; m.subsystem = "cpu"; m.action = "change";
        CHECK(UeventMonitor::classifyMessage(m) == UeventEventType::UNKNOWN,
              "subsystem=cpu change without /cpu substring -> UNKNOWN");
    }

    printf("=== Uevent: counters & lifecycle ===\n");
    UeventMonitor& mon = UeventMonitor::instance();
    CHECK(mon.isActive() == false, "not active before start");
    CHECK(mon.getTotalUevents() == 0, "total counter starts 0");
    // On a host without NETLINK_KOBJECT_UEVENT privilege, start() must fail cleanly
    bool started = mon.start([](UeventEventType, const std::string&) {});
    printf("  [INFO] start() returned %d (expected 0 without netlink priv on host)\n", (int)started);
    CHECK(mon.isActive() == started, "isActive consistent with start() result");
    mon.stop();
    CHECK(mon.isActive() == false, "stop() clears active");
    // Double stop must not crash AND must leave the monitor inactive.
    mon.stop();
    CHECK(mon.isActive() == false, "double stop() leaves monitor inactive (idempotent)");

    printf("\n========================================\n");
    printf("Uevent results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}
