#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include "event_monitor.h"
using namespace aatpe::event;
using std::this_thread::sleep_for;
using std::chrono::milliseconds;
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)
int main() {
    EventMonitor& em = EventMonitor::instance();
    printf("=== EventMonitor: priority routing ===\n");
    em.consumeHighPriorityEvent(); em.consumeMediumPriorityEvent();
    long long j0 = em.getEventCount(EventType::JANK);
    em.triggerEvent(EventType::JANK, "drop");
    CHECK(em.getEventCount(EventType::JANK) == j0 + 1, "JANK counted");
    CHECK(em.consumeHighPriorityEvent() == true, "JANK raises high-priority");
    CHECK(em.consumeHighPriorityEvent() == false, "high-priority consumed (once)");
    sleep_for(milliseconds(80));
    long long t0 = em.getEventCount(EventType::THERMAL_SPIKE);
    em.triggerEvent(EventType::THERMAL_SPIKE, "hot");
    CHECK(em.getEventCount(EventType::THERMAL_SPIKE) == t0 + 1, "THERMAL counted");
    CHECK(em.consumeHighPriorityEvent() == true, "THERMAL raises high-priority");
    em.consumeHighPriorityEvent(); sleep_for(milliseconds(120));
    long long f0 = em.getEventCount(EventType::FREQ_CHANGE);
    em.triggerEvent(EventType::FREQ_CHANGE, "freq");
    CHECK(em.getEventCount(EventType::FREQ_CHANGE) == f0 + 1, "FREQ counted");
    CHECK(em.consumeMediumPriorityEvent() == false, "FREQ does NOT raise medium-priority");
    CHECK(em.consumeHighPriorityEvent() == false, "FREQ does NOT raise high-priority");
    sleep_for(milliseconds(1060));
    long long h0 = em.getEventCount(EventType::HOTPLUG);
    em.triggerEvent(EventType::HOTPLUG, "plug");
    CHECK(em.getEventCount(EventType::HOTPLUG) == h0 + 1, "HOTPLUG counted");
    CHECK(em.consumeMediumPriorityEvent() == true, "HOTPLUG raises medium-priority");
    em.consumeMediumPriorityEvent();
    long long a0 = em.getEventCount(EventType::APP_SWITCH);
    em.triggerEvent(EventType::APP_SWITCH, "app");
    CHECK(em.getEventCount(EventType::APP_SWITCH) == a0 + 1, "APP_SWITCH counted");
    CHECK(em.consumeHighPriorityEvent() == false, "APP_SWITCH does NOT raise high-priority");
    CHECK(em.consumeMediumPriorityEvent() == false, "APP_SWITCH does NOT raise medium-priority");
    printf("=== EventMonitor: per-type cooldown dedup ===\n");
    sleep_for(milliseconds(1100));
    long long tb = em.getEventCount(EventType::THERMAL_SPIKE);
    em.triggerEvent(EventType::THERMAL_SPIKE, "rapid");
    em.triggerEvent(EventType::THERMAL_SPIKE, "rapid");
    em.triggerEvent(EventType::THERMAL_SPIKE, "rapid");
    CHECK(em.getEventCount(EventType::THERMAL_SPIKE) == tb + 1, "3 rapid THERMAL within 1000ms count once");
    long long jb = em.getEventCount(EventType::JANK);
    em.triggerEvent(EventType::JANK, "a"); em.triggerEvent(EventType::JANK, "b");
    CHECK(em.getEventCount(EventType::JANK) == jb + 2, "JANK no cooldown (counts both)");
    sleep_for(milliseconds(600));
    long long fb = em.getEventCount(EventType::FREQ_CHANGE);
    em.triggerEvent(EventType::FREQ_CHANGE, "x"); em.triggerEvent(EventType::FREQ_CHANGE, "x");
    CHECK(em.getEventCount(EventType::FREQ_CHANGE) == fb + 1, "2 rapid FREQ within 500ms count once");
    printf("=== EventMonitor: totals & layer ===\n");
    long long total = em.getEventCount(EventType::JANK) + em.getEventCount(EventType::THERMAL_SPIKE)
        + em.getEventCount(EventType::FREQ_CHANGE) + em.getEventCount(EventType::HOTPLUG)
        + em.getEventCount(EventType::APP_SWITCH);
    CHECK(em.getTotalEvents() == total, "getTotalEvents == sum of per-type counts");
    const char* layer = em.getActiveLayer();
    CHECK(layer != nullptr, "getActiveLayer non-null");
    std::string l(layer);
    CHECK(l == "uevent" || l == "inotify" || l == "polling", "layer is uevent/inotify/polling");
    printf("  [INFO] active layer = %s\n", layer);
    em.shutdown();
    CHECK(em.isInotifyActive() == false, "shutdown clears inotifyActive");
    printf("\n========================================\nEventMonitor results: %d passed, %d failed\n========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
