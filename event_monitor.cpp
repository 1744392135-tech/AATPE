#include "event_monitor.h"
#include "aatped.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace aatpe;
using namespace aatpe_ml;

namespace aatpe {
namespace event {

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string discoverCpuThermalZoneTempPath() {
    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) return "";
    std::string primary;
    std::string cpuTyped;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        std::string base = std::string("/sys/class/thermal/") + e->d_name;
        std::string typeStr = readFile((base + "/type").c_str());
        std::string typeLower = typeStr;
        std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), ::tolower);
        bool tempExists = (access((base + "/temp").c_str(), R_OK) == 0);
        if (!tempExists) continue;
        if (typeLower.find("mtktscpu") != std::string::npos) {
            primary = base + "/temp";
            break;
        }
        if (cpuTyped.empty() &&
            (typeLower.find("cpu") != std::string::npos ||
             typeLower.find("soc") != std::string::npos ||
             typeLower.find("cluster") != std::string::npos)) {
            cpuTyped = base + "/temp";
        }
    }
    closedir(dir);
    if (!primary.empty()) return primary;
    return cpuTyped;
}

EventMonitor& EventMonitor::instance() {
    static EventMonitor inst;
    return inst;
}

EventMonitor::EventMonitor() {}

EventMonitor::~EventMonitor() {
    shutdown();
}

bool EventMonitor::init() {
    if (running_.load()) return true;

    inotifyFd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotifyFd_ < 0) {
        LOG_WARN("EventMonitor: inotify_init failed (%s), falling back to polling", aatpeErrStr(errno));
        inotifyActive_.store(false);
        running_.store(true);
        return false;
    }

    {
        char bigCorePath[128];
        int bigCoreId;
        {
            std::lock_guard<std::mutex> topoCpuLk(g_topologyRwMutex);
            bigCoreId = (g_cpuCount > 0) ? g_cpuCount - 1 : -1;
        }
        if (bigCoreId >= 0) {
            snprintf(bigCorePath, sizeof(bigCorePath),
                     "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", bigCoreId);
            addWatch(bigCorePath, EventType::FREQ_CHANGE);
        }
    }
    addWatch("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", EventType::FREQ_CHANGE);

    {
        std::string cpuTempPath = aatpe_getThermalCpuZonePath();
        if (cpuTempPath.empty()) {
            cpuTempPath = discoverCpuThermalZoneTempPath();
        }
        if (!cpuTempPath.empty()) {
            addWatch(cpuTempPath, EventType::THERMAL_SPIKE);
            LOG_INFO("EventMonitor: watching CPU thermal zone %s", cpuTempPath.c_str());
        } else {
            LOG_WARN("EventMonitor: no CPU thermal zone discovered; THERMAL_SPIKE inotify watch skipped (uevent/netlink path still active)");
        }
    }

    inotifyActive_.store(true);
    running_.store(true);
    listenerThread_ = std::thread(&EventMonitor::listenerLoop, this);

    LOG_INFO("EventMonitor: inotify active, %d watches registered", (int)watches_.size());

    auto ueventCallback = [this](aatpe::event::UeventEventType ueType, const std::string& detail) {
        EventType evType;
        switch (ueType) {
            case aatpe::event::UeventEventType::FREQ_CHANGE:
                evType = EventType::FREQ_CHANGE;
                break;
            case aatpe::event::UeventEventType::HOTPLUG:
                evType = EventType::HOTPLUG; break;
            case aatpe::event::UeventEventType::THERMAL_SPIKE:
                evType = EventType::THERMAL_SPIKE; break;
            default:
                return;
        }
        this->triggerEvent(evType, "uevent:" + detail);
    };
    bool ueventOk = aatpe::event::UeventMonitor::instance().start(ueventCallback);
    if (ueventOk) {
        LOG_INFO("EventMonitor: uevent listener active (real-time netlink events)");
    } else {
        LOG_WARN("EventMonitor: uevent listener failed, falling back to inotify + polling");
    }

    return true;
}

bool EventMonitor::addWatch(const std::string& path, EventType type) {
    if (inotifyFd_ < 0) return false;

    static constexpr int kMaxWatches = 256;
    {
        std::lock_guard<std::mutex> lock(watchesMutex_);
        if ((int)watches_.size() >= kMaxWatches) {
            LOG_WARN("EventMonitor: inotify watch limit (%d) reached, skipping: %s",
                     kMaxWatches, path.c_str());
            return false;
        }
    }

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        LOG_WARN("EventMonitor: watch path not found: %s", path.c_str());
        return false;
    }

    int wd = inotify_add_watch(inotifyFd_, path.c_str(), IN_MODIFY | IN_ATTRIB);
    if (wd < 0) {
        LOG_WARN("EventMonitor: inotify_add_watch failed for %s: %s", path.c_str(), aatpeErrStr(errno));
        return false;
    }

    std::lock_guard<std::mutex> lock(watchesMutex_);
    watches_.push_back({wd, path, type});
    return true;
}

void EventMonitor::listenerLoop() {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (running_.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(inotifyFd_, &fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int ret = select(inotifyFd_ + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("EventMonitor: select failed (%s), exiting listener loop", aatpeErrStr(errno));
            break;
        }
        if (ret == 0) continue;

        ssize_t len = read(inotifyFd_, buf, sizeof(buf));
        if (len <= 0) continue;

        struct PendingEvent { EventType type; std::string detail; };
        std::vector<PendingEvent> pending;

        for (char* ptr = buf; ptr < buf + len; ) {
            if (ptr + sizeof(struct inotify_event) > buf + len) break;
            const struct inotify_event* event = (const struct inotify_event*)ptr;
            if (ptr + sizeof(struct inotify_event) + event->len > buf + len) break;

            {
                std::lock_guard<std::mutex> lock(watchesMutex_);
                bool matched = false;
                for (const auto& w : watches_) {
                    if (w.wd == event->wd) {
                        pending.push_back({w.type, w.path});
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    LOG_DEBUG("EventMonitor: inotify event for unknown wd=%d (watch removed?)",
                              event->wd);
                }
            }
            ptr += sizeof(struct inotify_event) + event->len;
        }

        for (const auto& pe : pending) {
            triggerEvent(pe.type, pe.detail);
        }
    }
}

bool EventMonitor::freqChangePassesGate() {
    int curFreq = 0;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        std::vector<int> perfCores;
        for (int c : g_bigPool) perfCores.push_back(c);
        if (!g_midPool.empty()) perfCores.push_back(g_midPool[0]);
        if (perfCores.empty()) {
            perfCores.push_back(0);
            if (!g_allCpus.empty()) perfCores.push_back(g_allCpus.back());
        }
        for (int cpu : perfCores) {
            std::string freqPath = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq";
            int f = readInt(freqPath.c_str(), 0);
            if (f > curFreq) curFreq = f;
        }
    }
    if (curFreq <= 0) return true;

    int last = lastAcceptedFreqKhz_.load(std::memory_order_relaxed);
    if (last <= 0) {
        lastAcceptedFreqKhz_.store(curFreq, std::memory_order_relaxed);
        return true;
    }

    int diff = std::abs(curFreq - last);
    int relGate = last * 25 / 100;
    const int absGate = 400000;

    if (diff >= relGate || diff >= absGate) {
        lastAcceptedFreqKhz_.store(curFreq, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void EventMonitor::triggerEvent(EventType type, const std::string& detail) {
    long long now = nowMs();

    int typeIdx = static_cast<int>(type);
    if (typeIdx >= 0 && typeIdx < (int)EventType::EVENT_TYPE_COUNT && kEventCooldownMs[typeIdx] > 0) {
        long long prev = lastEventTimeMs_[typeIdx].load(std::memory_order_acquire);
        while (true) {
            if (now - prev < kEventCooldownMs[typeIdx]) {
                return;
            }
            if (lastEventTimeMs_[typeIdx].compare_exchange_weak(
                    prev, now, std::memory_order_acq_rel, std::memory_order_acquire)) {
                break;
            }
        }
    }

    if (type == EventType::FREQ_CHANGE) {
        if (!freqChangePassesGate()) {
            return;
        }
    }

    const char* typeName = "UNKNOWN";
    switch (type) {
        case EventType::JANK: typeName = "JANK"; break;
        case EventType::THERMAL_SPIKE: typeName = "THERMAL_SPIKE"; break;
        case EventType::FREQ_CHANGE: typeName = "FREQ_CHANGE"; break;
        case EventType::HOTPLUG: typeName = "HOTPLUG"; break;
        case EventType::APP_SWITCH: typeName = "APP_SWITCH"; break;
        default: break;
    }
    if (!detail.empty()) {
        LOG_INFO("EventMonitor: %s event triggered (detail: %s)", typeName, detail.c_str());
    }

    switch (type) {
        case EventType::JANK:
            jankCount_++;
            {
                long long prev = lastHighEventMs_.load(std::memory_order_relaxed);
                if (now - prev > kHighEventCooldownMs &&
                    lastHighEventMs_.compare_exchange_strong(prev, now,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    highPriorityPending_.store(true, std::memory_order_relaxed);
                }
            }
            break;
        case EventType::THERMAL_SPIKE:
            thermalCount_++;
            {
                long long prev = lastHighEventMs_.load(std::memory_order_relaxed);
                if (now - prev > kHighEventCooldownMs &&
                    lastHighEventMs_.compare_exchange_strong(prev, now,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    highPriorityPending_.store(true, std::memory_order_relaxed);
                }
            }
            break;
        case EventType::FREQ_CHANGE:
            freqCount_++;
            break;
        case EventType::HOTPLUG:
            hotplugCount_++;
            {
                long long prev = lastMediumEventMs_.load(std::memory_order_relaxed);
                if (now - prev > kMediumEventCooldownMs &&
                    lastMediumEventMs_.compare_exchange_strong(prev, now,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    mediumPriorityPending_.store(true, std::memory_order_relaxed);
                }
            }
            break;
        case EventType::APP_SWITCH:
            appSwitchCount_++;
            break;
        default: break;
    }
}

bool EventMonitor::consumeHighPriorityEvent() {
    return highPriorityPending_.exchange(false);
}

bool EventMonitor::consumeMediumPriorityEvent() {
    return mediumPriorityPending_.exchange(false);
}

long long EventMonitor::getEventCount(EventType type) const {
    switch (type) {
        case EventType::JANK: return jankCount_.load();
        case EventType::THERMAL_SPIKE: return thermalCount_.load();
        case EventType::FREQ_CHANGE: return freqCount_.load();
        case EventType::HOTPLUG: return hotplugCount_.load();
        case EventType::APP_SWITCH: return appSwitchCount_.load();
        default: return 0;
    }
    return 0;
}

long long EventMonitor::getTotalEvents() const {
    return jankCount_.load() + thermalCount_.load() + freqCount_.load()
         + hotplugCount_.load() + appSwitchCount_.load();
}

const char* EventMonitor::getActiveLayer() const {
    if (aatpe::event::UeventMonitor::instance().isActive()) return "uevent";
    if (inotifyActive_.load()) return "inotify";
    return "polling";
}

void EventMonitor::writeStats(const std::string& stateDir) {
    writeFile((stateDir + "/event_jank_count").c_str(), std::to_string(jankCount_.load()));
    writeFile((stateDir + "/event_thermal_count").c_str(), std::to_string(thermalCount_.load()));
    writeFile((stateDir + "/event_freq_count").c_str(), std::to_string(freqCount_.load()));
    writeFile((stateDir + "/event_hotplug_count").c_str(), std::to_string(hotplugCount_.load()));
    writeFile((stateDir + "/event_app_switch_count").c_str(), std::to_string(appSwitchCount_.load()));
    writeFile((stateDir + "/event_total").c_str(), std::to_string(getTotalEvents()));
    writeFile((stateDir + "/event_inotify_active").c_str(), inotifyActive_.load() ? "1" : "0");
    writeFile((stateDir + "/event_uevent_active").c_str(),
              aatpe::event::UeventMonitor::instance().isActive() ? "1" : "0");
    writeFile((stateDir + "/event_active_layer").c_str(), getActiveLayer());
}

void EventMonitor::shutdown() {
    running_.store(false);
    aatpe::event::UeventMonitor::instance().stop();
    if (listenerThread_.joinable()) {
        listenerThread_.join();
    }
    if (inotifyFd_ >= 0) {
        close(inotifyFd_);
        inotifyFd_ = -1;
    }
    {
        std::lock_guard<std::mutex> wlock(watchesMutex_);
        watches_.clear();
    }
    inotifyActive_.store(false);
}

}
}
