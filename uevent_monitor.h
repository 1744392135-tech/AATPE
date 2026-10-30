#ifndef AATPE_UEVENT_MONITOR_H
#define AATPE_UEVENT_MONITOR_H

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace aatpe {
namespace event {

enum class UeventEventType {
    FREQ_CHANGE = 0,
    HOTPLUG = 1,
    THERMAL_SPIKE = 2,
    UNKNOWN = 99,
};

struct UeventMessage {
    std::string action;
    std::string devpath;
    std::string subsystem;
    std::string raw;
    UeventEventType type;
};

using UeventCallback = std::function<void(UeventEventType type, const std::string& detail)>;

class UeventMonitor {
public:
    static UeventMonitor& instance();

    bool start(const UeventCallback& callback);

    void stop();

    bool isActive() const { return active_.load(); }

    long long getTotalUevents() const { return totalCount_.load(); }

    long long getFreqCount() const { return freqCount_.load(); }
    long long getHotplugCount() const { return hotplugCount_.load(); }
    long long getThermalCount() const { return thermalCount_.load(); }

    static UeventMessage parseUevent(const char* buffer, size_t len);

    static UeventEventType classifyMessage(const UeventMessage& msg);

private:
    UeventMonitor();
    ~UeventMonitor();

    void listenerLoop();

    int createSocket();

    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> loopExited_{false};
    static constexpr auto kJoinTimeout = std::chrono::milliseconds(3000);
    std::thread listenerThread_;
    UeventCallback callback_;

    std::atomic<int> sockFd_{-1};

    std::atomic<long long> totalCount_{0};
    std::atomic<long long> freqCount_{0};
    std::atomic<long long> hotplugCount_{0};
    std::atomic<long long> thermalCount_{0};
};

}
}

#endif
