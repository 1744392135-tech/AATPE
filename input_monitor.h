// AATPE Touch-Boost Monitor (P1-6 / P1-5/P1-6/P1-11/P1-12)
//
// Competitive schedulers (Scene / Xiaomi) boost the CPU IMMEDIATELY on touch,
// instead of waiting for the ML loop to react. This monitor epoll-watches the
// touchscreen /dev/input/event* devices and, on touch, raises the schedtune
// boost via the SHARED boost bridge (aatpeBoostSet("touch", 80)) so it no
// longer clobbers the GPU-pegged boost, and releases it 500ms after the last
// touch event via aatpeBoostClear("touch").
//
// Design goals:
//   - Best-effort: if no touch device can be opened (host test env, SELinux),
//     the thread keeps retrying every 5s until a device appears or stop() fires.
//   - One dedicated epoll thread; bounded join on stop() (no detach).

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

    // Start the epoll thread. Safe to call when already running.
    bool start();
    // Stop and join the epoll thread; release any active touch boost.
    void stop();

    bool isTouchActive() const { return touchActive_.load(std::memory_order_relaxed); }
    bool isActive() const { return active_.load(std::memory_order_relaxed); }

    // Counters for WebUI / state JSON.
    uint64_t getTouchEvents() const { return touchEvents_.load(std::memory_order_relaxed); }
    uint64_t getBoostOns()    const { return boostOnCount_.load(std::memory_order_relaxed); }

    // v4.3.9: READ-ONLY accessors for the arbitrator (audit4_pipeline F1). These
    // only snapshot atomics; they never touch touchCtrl or the boost actuator.
    //   touchEventsTotal()  : cumulative touch-event count since start (delta
    //                        between two calls == events in that window).
    //   msSinceLastTouch()  : ms since the most recent touch event (steady clock);
    //                        returns a large sentinel (1<<30) when no touch has
    //                        ever been seen, so the context core treats it as idle.
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

    // Open touch-like /dev/input/event* devices, add to epoll. Returns count.
    int openTouchDevices(int epollFd);

    void loop();

    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> touchActive_{false};
    std::atomic<bool> loopExited_{false};
    std::atomic<uint64_t> touchEvents_{0};
    std::atomic<uint64_t> boostOnCount_{0};
    // v4.3.9: steady-clock ms of the most recent touch event (written by the loop
    // thread, read lock-free by msSinceLastTouch()). 0 = never touched.
    std::atomic<int64_t> lastTouchSteadyMs_{0};
    // P1-12: deviceFds_ is only touched by the loop thread. start() must not
    // read the vector from another thread; it only reads this atomic count.
    std::atomic<int> deviceCount_{0};

    std::vector<int> deviceFds_;
    int wakeupPipeFds_[2] = {-1, -1};  // self-pipe to break epoll_wait on stop
    // B-009: pipe-ready handoff flag. loop() writes wakeupPipeFds_ then releases
    // pipeReady_; stop() acquires pipeReady_ before reading/writing wakeupPipeFds_,
    // closing the create-vs-stop race (stop no longer writes a half-initialized or
    // stale self-pipe, and the read/write of wakeupPipeFds_ is now happens-before
    // ordered — TSan-clean).
    std::atomic<bool> pipeReady_{false};
    // LED-103 (R7-018): epoll fd was a loop()-local; stop()'s "loop did not exit
    // in 1s" fallback claimed to close it to force epoll_wait() to EBADF but had
    // no access to it (and never did). Expose it as an atomic member so the
    // fallback path can actually close it.
    std::atomic<int> epollFd_{-1};
    std::thread worker_;
};

}  // namespace aatpe

#endif  // AATPE_INPUT_MONITOR_H
