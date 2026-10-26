// AATPE Event Monitor (fix84 Feature 1: Event-Driven Architecture)
// Multi-source inotify event listener for real-time scheduling triggers.
// Monitors: CPU freq changes, thermal changes, CPU hotplug, jank events.
// Falls back to polling if inotify unavailable.
#ifndef AATPE_EVENT_MONITOR_H
#define AATPE_EVENT_MONITOR_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "uevent_monitor.h"  // netlink uevent listener

namespace aatpe {
namespace event {

// Event types with priority levels
enum class EventType {
    JANK = 0,           // High priority - immediate emergency migration
    THERMAL_SPIKE = 1,  // High priority - immediate emergency migration
    FREQ_CHANGE = 2,    // Medium priority - trigger full decision
    HOTPLUG = 3,        // Medium priority - trigger full decision
    APP_SWITCH = 4,     // Low priority - normal polling
    EVENT_TYPE_COUNT = 5 // sentinel for array sizing
};

// Event priority levels
enum class EventPriority {
    HIGH = 0,    // Immediate emergency migration
    MEDIUM = 1,  // Trigger one full decision
    LOW = 2,     // Normal polling
};

struct EventRecord {
    EventType type;
    EventPriority priority;
    long long timestamp_ms;
    std::string detail;
};

class EventMonitor {
public:
    static EventMonitor& instance();

    // Initialize inotify and start listener thread
    // Returns false if inotify unavailable (falls back to polling)
    bool init();

    // Stop listener thread
    void shutdown();

    // Check and consume pending high-priority event
    // Returns true if a high-priority event is pending (triggers emergency migration)
    bool consumeHighPriorityEvent();

    // Check and consume pending medium-priority event
    // Returns true if a medium-priority event is pending (triggers full decision)
    bool consumeMediumPriorityEvent();

    // Manually trigger an event (used by jank detection in daemonLoop)
    void triggerEvent(EventType type, const std::string& detail = "");

    // Event counters for diagnostics
    long long getEventCount(EventType type) const;
    long long getTotalEvents() const;

    // Check if inotify is active (not in fallback mode)
    bool isInotifyActive() const { return inotifyActive_.load(); }

    // Check if netlink uevent listener is active
    bool isUeventActive() const { return aatpe::event::UeventMonitor::instance().isActive(); }
    const char* getActiveLayer() const;

    // Write event stats to state files
    void writeStats(const std::string& stateDir);

private:
    EventMonitor();
    ~EventMonitor();

    // Main listener thread loop
    void listenerLoop();

    // Add inotify watch for a file path
    bool addWatch(const std::string& path, EventType type);

    // Frequency magnitude gate for FREQ_CHANGE events from uevent/inotify.
    // Reads current perf max freq, compares with last accepted value.
    // passes when change >= 25% relative OR >= 400MHz absolute.
    // Updates lastAcceptedFreqKhz_ on acceptance.
    bool freqChangePassesGate();

    std::atomic<bool> running_{false};
    std::atomic<bool> inotifyActive_{false};
    std::thread listenerThread_;

    int inotifyFd_{-1};

    // Watch descriptor -> (path, event type)
    struct WatchInfo {
        int wd;
        std::string path;
        EventType type;
    };
    std::vector<WatchInfo> watches_;
    std::mutex watchesMutex_;

    // Pending event flags
    std::atomic<bool> highPriorityPending_{false};
    std::atomic<bool> mediumPriorityPending_{false};

    // Event counters
    std::atomic<long long> jankCount_{0};
    std::atomic<long long> thermalCount_{0};
    std::atomic<long long> freqCount_{0};
    std::atomic<long long> hotplugCount_{0};
    std::atomic<long long> appSwitchCount_{0};

    // Last event timestamp for cooldown
    std::atomic<long long> lastHighEventMs_{0};
    std::atomic<long long> lastMediumEventMs_{0};

    // Per-event-type last trigger time for global dedup.
    // Prevents double-counting when both inotify and polling detect the same event.
    std::atomic<long long> lastEventTimeMs_[(int)EventType::EVENT_TYPE_COUNT]{}; // dynamic size;

    // Per-event-type cooldown (ms). 0 = no cooldown (always count).
    // FREQ: 500ms (freq changes can be frequent), THERMAL: 1000ms (stable temp).
    static constexpr long long kEventCooldownMs[(int)EventType::EVENT_TYPE_COUNT] = { // dynamic size
        0,    // JANK - count every rising edge
        1000, // THERMAL_SPIKE
        500,  // FREQ_CHANGE
        0,    // HOTPLUG - rare event
        0     // APP_SWITCH - rare event
    };

    // Last accepted perf max frequency (kHz) for FREQ_CHANGE magnitude gate.
    // Prevents uevent/inotify from firing on small oscillating DVFS changes
    // (e.g. 2200->2127->2200) that would otherwise trigger full decisions
    // every 500ms after the cooldown expires.
    std::atomic<int> lastAcceptedFreqKhz_{0};

    static constexpr long long kHighEventCooldownMs = 50;   // Min interval between high events
    // raised from 200ms to 500ms. FREQ_CHANGE used to fire 2-3x/sec
    // (DVFS hunting) and each accepted event set mediumPriorityPending_. Combined
    // with the 500ms throttle in aatped_main.cpp, immediate full decisions are now
    // capped at <=2/sec (was ~5/sec), cutting the main-thread wake/scan overhead.
    // raised again 500ms -> 1000ms. On-device DVFS hunting still
    // logged "medium-priority event detected, immediate full decision" several
    // times per second, each forcing a full thread scan that almost always ended
    // in a low-confidence no-op migration. 1s cooldown caps medium-driven full
    // decisions at <=1/sec; the main poll cycle still reacts within its own period.
    static constexpr long long kMediumEventCooldownMs = 1000; // Min interval between medium events
};

} // namespace event
} // namespace aatpe

#endif // AATPE_EVENT_MONITOR_H
