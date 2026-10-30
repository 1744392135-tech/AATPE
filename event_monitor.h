#ifndef AATPE_EVENT_MONITOR_H
#define AATPE_EVENT_MONITOR_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "uevent_monitor.h"

namespace aatpe {
namespace event {

enum class EventType {
    JANK = 0,
    THERMAL_SPIKE = 1,
    FREQ_CHANGE = 2,
    HOTPLUG = 3,
    APP_SWITCH = 4,
    EVENT_TYPE_COUNT = 5
};

enum class EventPriority {
    HIGH = 0,
    MEDIUM = 1,
    LOW = 2,
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

    bool init();

    void shutdown();

    bool consumeHighPriorityEvent();

    bool consumeMediumPriorityEvent();

    void triggerEvent(EventType type, const std::string& detail = "");

    long long getEventCount(EventType type) const;
    long long getTotalEvents() const;

    bool isInotifyActive() const { return inotifyActive_.load(); }

    bool isUeventActive() const { return aatpe::event::UeventMonitor::instance().isActive(); }
    const char* getActiveLayer() const;

    void writeStats(const std::string& stateDir);

private:
    EventMonitor();
    ~EventMonitor();

    void listenerLoop();

    bool addWatch(const std::string& path, EventType type);

    bool freqChangePassesGate();

    std::atomic<bool> running_{false};
    std::atomic<bool> inotifyActive_{false};
    std::thread listenerThread_;

    int inotifyFd_{-1};

    struct WatchInfo {
        int wd;
        std::string path;
        EventType type;
    };
    std::vector<WatchInfo> watches_;
    std::mutex watchesMutex_;

    std::atomic<bool> highPriorityPending_{false};
    std::atomic<bool> mediumPriorityPending_{false};

    std::atomic<long long> jankCount_{0};
    std::atomic<long long> thermalCount_{0};
    std::atomic<long long> freqCount_{0};
    std::atomic<long long> hotplugCount_{0};
    std::atomic<long long> appSwitchCount_{0};

    std::atomic<long long> lastHighEventMs_{0};
    std::atomic<long long> lastMediumEventMs_{0};

    std::atomic<long long> lastEventTimeMs_[(int)EventType::EVENT_TYPE_COUNT]{};

    static constexpr long long kEventCooldownMs[(int)EventType::EVENT_TYPE_COUNT] = {
        0,
        1000,
        500,
        0,
        0
    };

    std::atomic<int> lastAcceptedFreqKhz_{0};

    static constexpr long long kHighEventCooldownMs = 50;
    static constexpr long long kMediumEventCooldownMs = 1000;
};

}
}

#endif
