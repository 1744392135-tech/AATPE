#include "input_monitor.h"
#include "gpu_boost.h"
#include "aatped.h"
#include "aatped_logging.h"
#include "placement.h"
#include "touch_uclamp_act.h"
#include "aatped_state.h"
#include "aatpe_placement_mech.h"

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
#include <algorithm>

namespace aatpe {

static constexpr int kEpollTimeoutMs      = 200;
static constexpr int kHotplugRetryMs       = 5000;
static constexpr int kLiveBoostEpollMs    = 16;

static int prodTouchSetTaskUclampMin(int tid, unsigned int utilMin, void* ) {
    return aatpe_mech::aatpeUclampSetMin(tid, utilMin);
}
static void prodTouchSetGlobalBoostFloor(int level, void* ) {
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
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + ent->d_name;
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        #define AATPE_BITS_TO_LONGS(n) \
            (((n) + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long)))
        unsigned long evbits[AATPE_BITS_TO_LONGS(EV_MAX + 1)] = {};
        int looksLikeTouch = 0;
        if (::ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) >= 0) {
            if (evbits[EV_ABS / (8 * sizeof(unsigned long))] &
                (1UL << (EV_ABS % (8 * sizeof(unsigned long))))) {
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
        if (looksLikeTouch) {
            unsigned long props = 0;
            if (::ioctl(fd, EVIOCGPROP(sizeof(props)), &props) < 0 ||
                !(props & (1UL << (0 )))) {
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
    epollFd_.store(epollFd, std::memory_order_relaxed);

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
    pipeReady_.store(true, std::memory_order_release);

    long long lastProbeAttemptMs = 0;
    auto nowMs = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    struct epoll_event events[8];
    bool loggedNoDevice = false;

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
        if (deviceCount_.load(std::memory_order_relaxed) == 0) {
            long long now = nowMs();
            if (now - lastProbeAttemptMs >= kHotplugRetryMs) {
                lastProbeAttemptMs = now;
                int n = openTouchDevices(epollFd);
                if (n > 0) {
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
                char buf[64];
                while (::read(fd, buf, sizeof(buf)) > 0) {}
                continue;
            }
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                ::epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                deviceFds_.erase(std::remove(deviceFds_.begin(), deviceFds_.end(), fd),
                                 deviceFds_.end());
                deviceCount_.store(deviceFds_.size(), std::memory_order_relaxed);
                if (deviceFds_.empty()) {
                    active_.store(false);
                    touchCtrl.releaseAll((uint32_t)nowMs(), true);
                    touchActive_.store(false);
                    wasBoostLive = false;
                }
                LOG_INFO("InputMonitor: touch fd=%d removed (HUP/ERR), %zu device(s) left",
                         fd, deviceFds_.size());
                continue;
            }
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
            lastTouchSteadyMs_.store(nowMsCount, std::memory_order_relaxed);
            static std::atomic<int64_t> s_lastPlacementEventMs{0};
            int64_t prev = s_lastPlacementEventMs.load(std::memory_order_relaxed);
            if (nowMsCount - prev >= 40 &&
                s_lastPlacementEventMs.compare_exchange_strong(prev, nowMsCount)) {
                aatpe::placement::onInputEvent();
            }
        }

        {
            std::string fg = aatpeGetForegroundPackage();
            FgThread threads[TOUCH_FF_MAX_THREADS];
            uint32_t nThreads = touchUclampCollectForegroundThreads(
                fg, threads, TOUCH_FF_MAX_THREADS);
            float hz = (float)aatpe::frameaware::getRefreshRate();
            float targetMs = (float)aatpe::frameaware::getFrameIntervalMs();
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

    touchCtrl.releaseAll((uint32_t)nowMs(), true);
    touchActive_.store(false);

    for (int fd : deviceFds_) ::close(fd);
    deviceFds_.clear();
    deviceCount_.store(0, std::memory_order_relaxed);
    if (wakeupPipeFds_[0] >= 0) { ::close(wakeupPipeFds_[0]); wakeupPipeFds_[0] = -1; }
    if (wakeupPipeFds_[1] >= 0) { ::close(wakeupPipeFds_[1]); wakeupPipeFds_[1] = -1; }
    ::close(epollFd);
    epollFd_.store(-1, std::memory_order_relaxed);
    loopExited_.store(true);
}

bool InputMonitor::start() {
    if (running_.load()) {
        return active_.load();
    }
    running_.store(true);
    loopExited_.store(false);
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

}
