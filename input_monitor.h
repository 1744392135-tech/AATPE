
#ifndef AATPE_INPUT_MONITOR_H
#define AATPE_INPUT_MONITOR_H

#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <cstdint>

namespace aatpe {

class InputMonitor {
public:
    static InputMonitor& instance();

    bool start();
    void stop();

    bool isTouchActive() const { return touchActive_.load(std::memory_order_relaxed); }
    bool isActive() const { return active_.load(std::memory_order_relaxed); }

    uint64_t getTouchEvents() const { return touchEvents_.load(std::memory_order_relaxed); }
    uint64_t getBoostOns()    const { return boostOnCount_.load(std::memory_order_relaxed); }

    uint64_t touchEventsTotal() const { return touchEvents_.load(std::memory_order_relaxed); }
    int64_t  msSinceLastTouch() const {
        int64_t last = lastTouchSteadyMs_.load(std::memory_order_relaxed);
        if (last == 0) return (int64_t)1 << 30;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t d = now - last;
        return d < 0 ? 0 : d;
    }

private:
    InputMonitor();
    ~InputMonitor();
    InputMonitor(const InputMonitor&) = delete;
    InputMonitor& operator=(const InputMonitor&) = delete;

    int openTouchDevices(int epollFd);

    void loop();

    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> touchActive_{false};
    std::atomic<bool> loopExited_{false};
    std::atomic<uint64_t> touchEvents_{0};
    std::atomic<uint64_t> boostOnCount_{0};
    std::atomic<int64_t> lastTouchSteadyMs_{0};
    std::atomic<int> deviceCount_{0};

    std::vector<int> deviceFds_;
    int wakeupPipeFds_[2] = {-1, -1};
    std::atomic<bool> pipeReady_{false};
    std::atomic<int> epollFd_{-1};
    std::thread worker_;
};

}

#endif
