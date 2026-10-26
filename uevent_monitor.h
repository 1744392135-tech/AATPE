// AATPE Uevent Monitor (True event-driven via netlink uevent)
// Replaces unreliable inotify on sysfs (which often doesn't trigger IN_MODIFY).
// Uses NETLINK_KOBJECT_UEVENT socket to receive kernel uevents in real-time.
// Three-layer architecture: uevent (real-time) -> inotify (backup) -> polling (fallback).
#ifndef AATPE_UEVENT_MONITOR_H
#define AATPE_UEVENT_MONITOR_H

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace aatpe {
namespace event {

// Event types detected by uevent (maps to EventMonitor EventType)
enum class UeventEventType {
    FREQ_CHANGE = 0,
    HOTPLUG = 1,
    THERMAL_SPIKE = 2,
    UNKNOWN = 99,
};

// Parsed uevent message
struct UeventMessage {
    std::string action;      // "add", "remove", "change", etc.
    std::string devpath;     // e.g. "/devices/system/cpu/cpu0/cpufreq"
    std::string subsystem;   // e.g. "cpu", "thermal"
    std::string raw;         // raw message for debugging
    UeventEventType type;    // classified event type
};

// Callback type: invoked when a matching uevent is received
using UeventCallback = std::function<void(UeventEventType type, const std::string& detail)>;

class UeventMonitor {
public:
    static UeventMonitor& instance();

    // Start uevent listener thread. Returns false if socket creation fails.
    bool start(const UeventCallback& callback);

    // Stop listener thread and close socket
    void stop();

    // Check if uevent listener is active and receiving events
    bool isActive() const { return active_.load(); }

    // Total uevents received (all types)
    long long getTotalUevents() const { return totalCount_.load(); }

    // Per-type counts
    long long getFreqCount() const { return freqCount_.load(); }
    long long getHotplugCount() const { return hotplugCount_.load(); }
    long long getThermalCount() const { return thermalCount_.load(); }

    // Parse a raw uevent buffer into a UeventMessage (public for testing)
    static UeventMessage parseUevent(const char* buffer, size_t len);

    // Classify a parsed message into event type (public for testing)
    static UeventEventType classifyMessage(const UeventMessage& msg);

private:
    UeventMonitor();
    ~UeventMonitor();

    // Main listener thread loop
    void listenerLoop();

    // Create and bind netlink socket. Returns socket fd or -1 on failure.
    int createSocket();

    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    // set to true as the very last action of listenerLoop(); stop() polls
    // this to bound its join() instead of blocking forever on a wedged recv().
    std::atomic<bool> loopExited_{false};
    // max time stop() waits for the listener thread to drain before
    // detaching it as a last-resort fallback (hot reload / shutdown must not hang).
    static constexpr auto kJoinTimeout = std::chrono::milliseconds(3000);
    std::thread listenerThread_;
    UeventCallback callback_;

    // LED-087 (R7-002): sockFd_ is written by stop() (-> -1) on the caller thread
    // while listenerLoop() blocks in recv(sockFd_). A plain int cross-thread access
    // is a C++ data race (UB); make it atomic. stop() uses exchange(-1); the loop
    // loads the fd for its recv().
    std::atomic<int> sockFd_{-1};

    // Event counters
    std::atomic<long long> totalCount_{0};
    std::atomic<long long> freqCount_{0};
    std::atomic<long long> hotplugCount_{0};
    std::atomic<long long> thermalCount_{0};
};

} // namespace event
} // namespace aatpe

#endif // AATPE_UEVENT_MONITOR_H
