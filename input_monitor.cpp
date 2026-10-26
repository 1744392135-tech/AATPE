// AATPE Touch-Boost Monitor implementation (P1-6 / P1-5/P1-6/P1-11/P1-12)
#include "input_monitor.h"
#include "gpu_boost.h"        // P1-5: shared boost bridge
#include "aatped.h"           // ROUND-014 P3: frameaware::getRefreshRate
#include "aatped_logging.h"
#include "placement.h"       // ROUND-007b: event-driven spillover promotion
// v4.3.6: per-thread uclamp touch boost wiring (verified touch_uclamp core).
#include "touch_uclamp_act.h"      // controller + actuator mapping
#include "aatped_state.h"          // aatpeGetForegroundPackage() (thread-safe reader)
#include "aatpe_placement_mech.h"  // aatpe_mech::aatpeUclampSetMin (per-task uclamp)

#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/eventfd.h>

#include <linux/input.h>

#include <chrono>
#include <algorithm>   // P2-12: std::remove (erase-remove on deviceFds_)

namespace aatpe {

// Boost parameters. kTouchBoostValue is the schedtune.boost hint (0..100);
// 80 is the same order of magnitude the stock touch boost uses on MTK.
static constexpr int kEpollTimeoutMs      = 200;   // wake up periodically to check release/hotplug
static constexpr int kHotplugRetryMs       = 5000; // P1-6: retry openTouchDevices every 5s
// v4.3.6: while a per-thread boost window is live, poll epoll more often so the
// core's expiry sweep RELEASES promptly (windows are 33..80ms). Idle stays at
// kEpollTimeoutMs to avoid burning CPU.
static constexpr int kLiveBoostEpollMs    = 16;

// v4.3.6: production actuator seams. The controller maps a TouchDecision onto
// these; they are the ONLY side-effect points. Per-thread uclamp goes through
// the existing aatpe_mech::aatpeUclampSetMin (sched_setattr, 0..1024). The global
// schedtune floor goes through the shared boost bridge as the "touch" producer
// (max() across producers, so a concurrent GPU boost still wins).
static int prodTouchSetTaskUclampMin(int tid, unsigned int utilMin, void* /*ctx*/) {
    return aatpe_mech::aatpeUclampSetMin(tid, utilMin);
}
static void prodTouchSetGlobalBoostFloor(int level, void* /*ctx*/) {
    if (level > 0) aatpeBoostSet("touch", level);
    else           aatpeBoostClear("touch");
}

InputMonitor& InputMonitor::instance() {
    static InputMonitor inst;
    return inst;
}

InputMonitor::InputMonitor() {}

InputMonitor::~InputMonitor() {
    stop();
}

int InputMonitor::openTouchDevices(int epollFd) {
    DIR* dir = ::opendir("/dev/input");
    if (!dir) return 0;
    int found = 0;
    struct dirent* ent;
    while ((ent = ::readdir(dir)) != nullptr) {
        // Only event* nodes.
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + ent->d_name;
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        // Decide if this looks like a touch device: must advertise EV_ABS.
        // G6-013: BITS_TO_LONGS(n) = ceil(n / BITS_PER_LONG). The old formula
        // (EV_MAX+1+7)/8/sizeof(unsigned long) evaluated to 0 on 64-bit
        // (39 bytes / 8 = 0 longs), producing a zero-length array and an
        // out-of-bounds evbits[0] read. Use the kernel idiom so the buffer always
        // holds at least one unsigned long.
        #define AATPE_BITS_TO_LONGS(n) \
            (((n) + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long)))
        unsigned long evbits[AATPE_BITS_TO_LONGS(EV_MAX + 1)] = {};
        int looksLikeTouch = 0;
        if (::ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) >= 0) {
            if (evbits[EV_ABS / (8 * sizeof(unsigned long))] &
                (1UL << (EV_ABS % (8 * sizeof(unsigned long))))) {
                // Multitouch: probe ABS_MT_POSITION_X (0x35).
                unsigned long absbits[AATPE_BITS_TO_LONGS(ABS_MAX + 1)] = {};
                if (::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
                    if (absbits[0x35 / (8 * sizeof(unsigned long))] &
                        (1UL << (0x35 % (8 * sizeof(unsigned long))))) {
                        looksLikeTouch = 1;
                    }
                }
            }
        }
        #undef AATPE_BITS_TO_LONGS
        // ROUND-014 P3: require INPUT_PROP_DIRECT (actual touchscreen), reject
        // gamepads/joysticks that also advertise ABS_MT.
        if (looksLikeTouch) {
            unsigned long props = 0;
            if (::ioctl(fd, EVIOCGPROP(sizeof(props)), &props) < 0 ||
                !(props & (1UL << (0 /*INPUT_PROP_DIRECT*/)))) {
                looksLikeTouch = 0;
            }
        }
        if (!looksLikeTouch) {
            ::close(fd);
            continue;
        }

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (::epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            deviceFds_.push_back(fd);
            found++;
        } else {
            ::close(fd);
        }
    }
    ::closedir(dir);
    return found;
}

void InputMonitor::loop() {
    int epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollFd < 0) {
        LOG_WARN("InputMonitor: epoll_create1 failed: %s", aatpeErrStr(errno));
        loopExited_.store(true);
        return;
    }
    epollFd_.store(epollFd, std::memory_order_relaxed);  // LED-103: visible to stop()

    // Set up the self-pipe FIRST so stop() can always break epoll_wait, even
    // during the hotplug-retry phase before any touch device exists.
    int pipeFds[2] = {-1, -1};
    if (::pipe2(pipeFds, O_NONBLOCK | O_CLOEXEC) == 0) {
        wakeupPipeFds_[0] = pipeFds[0];
        wakeupPipeFds_[1] = pipeFds[1];
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = pipeFds[0];
        ::epoll_ctl(epollFd, EPOLL_CTL_ADD, pipeFds[0], &ev);
    }
    // B-009: publish the now-fully-initialized self-pipe. stop() acquires this flag
    // before touching wakeupPipeFds_, so it never reads/writes a half-built pipe.
    pipeReady_.store(true, std::memory_order_release);

    // P1-6: no touch device at start is NOT fatal. Keep retrying every 5s until
    // one appears (or stop() fires). The touchscreen driver may not have
    // registered /dev/input/event* yet when the daemon starts.
    long long lastProbeAttemptMs = 0;
    auto nowMs = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    struct epoll_event events[8];
    bool loggedNoDevice = false;

    // v4.3.6: per-thread uclamp touch controller. Owns the TouchState/TouchConfig
    // for this epoll thread; maps the verified touch_uclamp decision onto the
    // production actuator (per-task sched_setattr uclamp.min + global schedtune
    // floor). Replaces the old blanket aatpeBoostSet("touch", critical=80).
    TouchUclampController touchCtrl;
    {
        TouchUclampActuator act;
        act.setTaskUclampMin    = prodTouchSetTaskUclampMin;
        act.setGlobalBoostFloor = prodTouchSetGlobalBoostFloor;
        act.ctx                 = nullptr;
        touchCtrl.setActuator(act);
    }
    bool wasBoostLive = false;

    while (running_.load(std::memory_order_relaxed)) {
        // P1-6: hotplug retry.
        if (deviceCount_.load(std::memory_order_relaxed) == 0) {
            long long now = nowMs();
            if (now - lastProbeAttemptMs >= kHotplugRetryMs) {
                lastProbeAttemptMs = now;
                int n = openTouchDevices(epollFd);
                if (n > 0) {
                    // P2-13: deviceFds_.size() is the source of truth (openTouchDevices
                    // appends to it). Use that rather than the returned count n so
                    // deviceCount_ can never drift out of sync with the tracked fds.
                    deviceCount_.store(deviceFds_.size(), std::memory_order_relaxed);
                    active_.store(true);
                    LOG_INFO("InputMonitor: found %d touch device(s) after hotplug retry", n);
                    loggedNoDevice = false;
                } else if (!loggedNoDevice) {
                    LOG_INFO("InputMonitor: no touch device yet; will retry every %dms",
                             kHotplugRetryMs);
                    loggedNoDevice = true;
                }
            }
        }

        // v4.3.6: while a per-thread boost window is live, poll at kLiveBoostEpollMs
        // so the core's expiry sweep RELEASES within the window; idle stays at the
        // 200ms cadence (hotplug / bookkeeping).
        int epollTimeoutMs = touchCtrl.isBoostLive() ? kLiveBoostEpollMs : kEpollTimeoutMs;
        int r = ::epoll_wait(epollFd, events, 8, epollTimeoutMs);
        if (r < 0) {
            if (errno == EINTR) continue;
            usleep(100 * 1000);
            continue;
        }
        bool sawTouch = false;
        for (int i = 0; i < r; ++i) {
            int fd = events[i].data.fd;
            if (fd == wakeupPipeFds_[0]) {
                // stop() woke us; drain and exit.
                char buf[64];
                while (::read(fd, buf, sizeof(buf)) > 0) {}
                continue;
            }
            // P2-12: touch device unplugged / error. epoll signals HUP/ERR on the
            // watched fd; without handling it the fd stays registered and leaks
            // (read() keeps returning 0). Remove from epoll, close, drop from the
            // tracked list, and re-sync deviceCount_.
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                ::epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                deviceFds_.erase(std::remove(deviceFds_.begin(), deviceFds_.end(), fd),
                                 deviceFds_.end());
                deviceCount_.store(deviceFds_.size(), std::memory_order_relaxed);
                if (deviceFds_.empty()) {
                    active_.store(false);
                    // v4.3.6: drop any per-thread uclamp floors AND clear the global
                    // "touch" producer on full device loss.
                    touchCtrl.releaseAll((uint32_t)nowMs(), /*clearGlobal=*/true);
                    touchActive_.store(false);
                    wasBoostLive = false;
                }
                LOG_INFO("InputMonitor: touch fd=%d removed (HUP/ERR), %zu device(s) left",
                         fd, deviceFds_.size());
                continue;
            }
            // Drain the input event record(s).
            struct input_event ie;
            while (::read(fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
                sawTouch = true;
            }
        }

        auto now = std::chrono::steady_clock::now();
        auto nowMsCount = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now.time_since_epoch()).count();
        if (sawTouch) {
            touchEvents_.fetch_add(1, std::memory_order_relaxed);
            // v4.3.9: record the most-recent touch time for the arbitrator's
            // last-interaction signal (lock-free atomic; read elsewhere).
            lastTouchSteadyMs_.store(nowMsCount, std::memory_order_relaxed);
            // ROUND-007b: notify placement layer for event-driven spillover
            // promotion. Throttle to once per 40ms to avoid flooding the
            // evaluator on rapid touch storms. onInputEvent() is noexcept
            // and self-guards (no-op in EMERGENCY / no key threads).
            static std::atomic<int64_t> s_lastPlacementEventMs{0};
            int64_t prev = s_lastPlacementEventMs.load(std::memory_order_relaxed);
            if (nowMsCount - prev >= 40 &&
                s_lastPlacementEventMs.compare_exchange_strong(prev, nowMsCount)) {
                aatpe::placement::onInputEvent();
            }
        }

        // v4.3.6: one touch-decision tick. Build the foreground thread set from
        // thread_scan, feed refresh rate + frame target, and let the verified
        // touch_uclamp core pick which threads to SET/RELEASE. The actuator lands
        // per-thread uclamp.min (UI/Render/high-contrib only; BG never) and holds
        // the global schedtune floor at idle40/active50 (was the old maxed 80).
        // Empty foreground -> nThreads==0 -> no per-thread action, only the floor.
        {
            std::string fg = aatpeGetForegroundPackage();
            FgThread threads[TOUCH_FF_MAX_THREADS];
            uint32_t nThreads = touchUclampCollectForegroundThreads(
                fg, threads, TOUCH_FF_MAX_THREADS);
            float hz = (float)aatpe::frameaware::getRefreshRate();
            float targetMs = (float)aatpe::frameaware::getFrameIntervalMs();
            // frame_actual unknown on this path (<=0 -> core skips overrun bonus).
            touchCtrl.tick((uint32_t)nowMsCount, sawTouch, threads, nThreads,
                           hz, 0.0f, targetMs);
            bool live = touchCtrl.isBoostLive();
            touchActive_.store(live);
            if (live && !wasBoostLive) {
                boostOnCount_.fetch_add(1, std::memory_order_relaxed);
            }
            wasBoostLive = live;
        }
    }

    // On exit: release any active per-thread boost and clear the global producer.
    touchCtrl.releaseAll((uint32_t)nowMs(), /*clearGlobal=*/true);
    touchActive_.store(false);

    for (int fd : deviceFds_) ::close(fd);
    deviceFds_.clear();
    deviceCount_.store(0, std::memory_order_relaxed);
    if (wakeupPipeFds_[0] >= 0) { ::close(wakeupPipeFds_[0]); wakeupPipeFds_[0] = -1; }
    if (wakeupPipeFds_[1] >= 0) { ::close(wakeupPipeFds_[1]); wakeupPipeFds_[1] = -1; }
    ::close(epollFd);
    epollFd_.store(-1, std::memory_order_relaxed);  // LED-103
    loopExited_.store(true);
}

bool InputMonitor::start() {
    if (running_.load()) {
        return active_.load();
    }
    running_.store(true);
    loopExited_.store(false);
    // B-009: reset the self-pipe handoff state BEFORE the worker creates a fresh
    // pipe, so a start/stop/start cycle never writes to fds left over from the
    // previous run (which stop() already closed). pipeReady_ is cleared here under
    // the start() caller's control and is only re-published once loop() has set up
    // the new pipe.
    wakeupPipeFds_[0] = -1;
    wakeupPipeFds_[1] = -1;
    pipeReady_.store(false, std::memory_order_relaxed);
    try {
        worker_ = std::thread(&InputMonitor::loop, this);
    } catch (const std::exception& e) {
        LOG_WARN("InputMonitor: failed to start thread: %s", e.what());
        running_.store(false);
        active_.store(false);
        return false;
    }
    // P1-12: do NOT touch deviceFds_ (owned by the loop thread). Read the atomic
    // count instead. It may still be 0 during hotplug retry; active_ follows the
    // count and flips true once a device appears.
    usleep(50 * 1000);
    bool hasDevices = (deviceCount_.load(std::memory_order_acquire) > 0);
    active_.store(hasDevices);
    if (hasDevices) {
        LOG_INFO("InputMonitor: touch boost thread started");
    } else {
        LOG_INFO("InputMonitor: touch boost thread idle (no touch device yet, will retry)");
    }
    return hasDevices;
}

void InputMonitor::stop() {
    if (!running_.load()) return;
    running_.store(false);
    // Wake the epoll loop via the self-pipe.
    // B-009: the pipe is created by loop() AFTER start() spawns the worker, so a
    // stop() racing that creation may not see it ready yet. Wait a short bounded
    // window for pipeReady_ (acquire) so the write is ordered against loop()'s
    // release of wakeupPipeFds_. If the pipe never becomes ready (loop wedged or
    // already exiting), skip the write — the loop either already saw running_==false
    // and is exiting, or will create the pipe then immediately fall out of the
    // while(running_) gate. Either way the join below bounds the wait.
    const auto pipeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (!pipeReady_.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < pipeDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (pipeReady_.load(std::memory_order_acquire) && wakeupPipeFds_[1] >= 0) {
        char c = 'x';
        ssize_t w = ::write(wakeupPipeFds_[1], &c, 1);
        (void)w;
    }
    if (worker_.joinable()) {
        // MON-03 fix: bumped the join deadline from 1s to 5s. The loop's epoll_wait
        // is bounded by kEpollTimeoutMs (200ms) and all device fds are O_NONBLOCK,
        // so the loop must observe running_==false within ~200ms after the self-pipe
        // write. 5s covers any unexpected blocking syscall in touchCtrl.releaseAll()
        // or the foreground-thread enumeration. If the worker STILL has not exited by
        // 5s, we detach as a last resort: InputMonitor is a process-lifetime
        // singleton (started once at daemon boot, stopped once at shutdown), so the
        // only caller that destroys it is the process teardown path, where the OS
        // reclaims the address space regardless. The detached thread will be reaped
        // by the OS; it cannot corrupt a live daemon.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!loopExited_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (loopExited_.load(std::memory_order_acquire)) {
            worker_.join();
        } else {
            LOG_WARN("InputMonitor: loop did not exit in 5s; detaching worker "
                     "(process-lifetime singleton; OS reaps at teardown)");
            worker_.detach();
        }
    }
    active_.store(false);
    touchActive_.store(false);
}

}  // namespace aatpe
