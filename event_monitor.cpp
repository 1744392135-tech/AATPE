// AATPE Event Monitor Implementation
// Multi-source inotify event listener with polling fallback.
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
// .cpp files may use using namespace (not included elsewhere)
using namespace aatpe;
using namespace aatpe_ml;


namespace aatpe {
namespace event {

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// P1-4 fix: lightweight /sys/class/thermal scan that mirrors readThermal()'s
// zone-selection priority so the inotify watch on THERMAL_SPIKE tracks the SAME
// authoritative CPU die sensor (mtktscpu on MediaTek), never the battery/PMIC
// zone that thermal_zone0 tends to be on this class of device. Returns the
// /temp path to watch, or "" if no CPU-typed zone exists (caller skips watch).
// Deliberately self-contained (no thermal.cpp dependency) so it works before the
// first readThermal() has populated g_thermalCpuZonePath.
static std::string discoverCpuThermalZoneTempPath() {
    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) return "";
    std::string primary;  // mtktscpu — highest priority
    std::string cpuTyped; // cpu/soc/cluster — secondary
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
            break;  // primary sensor: stop scanning immediately
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

    // Add watches for key system files
    // CPU frequency (big core representative)
    // P3-042: dynamic big-core path instead of hardcoded cpu7 (assumes 8 cores)
    {
        char bigCorePath[128];
        // LED-096 (R7-011): g_cpuCount is written by detectCpuTopology() under
        // g_topologyRwMutex; snapshot it under the same lock rather than reading it
        // unlocked here.
        // B-008: removed the hardcoded `: 7` fallback. When topology has not been
        // detected yet (g_cpuCount==0 at early init) we DO NOT fabricate cpu7; the
        // cpu0 cpufreq watch below already covers FREQ_CHANGE, so deferring the
        // big-core representative watch is both safe and generic (no 8-core
        // assumption, works for 1+3+4 / 1+2+2+3 / all-big / any vendor layout).
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

    // P1-4 fix: do NOT hardcode thermal_zone0/temp. On Redmi Note10 Pro (Dimensity
    // 1100) thermal_zone0 is the battery/PMIC sensor; the authoritative CPU die
    // temperature is thermal_zone4 type "mtktscpu". Watching zone0 made the
    // THERMAL_SPIKE inotify fire on battery/PMIC noise instead of real die heat.
    // Resolve the SAME zone readThermal() uses:
    //   1. aatpe_getThermalCpuZonePath() — populated by readThermal()'s scan.
    //   2. Fallback: lightweight inline scan for type "mtktscpu" (primary MTK die),
    //      then "cpu"/"soc"/"cluster", mirroring thermal.cpp's priority. EventMonitor
    //      starts BEFORE the first readThermal(), so g_thermalCpuZonePath may still
    //      be empty at init time and the inline scan is what actually picks the zone.
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

    // B-006: removed the dead inotify watch on /sys/devices/system/cpu/online. The
    // sysfs "online" file is a global read-only bitmask; inotify IN_MODIFY on it is
    // unreliable (kernel hotplug events are delivered via uevent_monitor's netlink
    // UEVENT path, which is the real hotplug source). This watch only consumed an
    // inotify slot and misled the monitoring-source accounting.

    inotifyActive_.store(true);
    running_.store(true);
    listenerThread_ = std::thread(&EventMonitor::listenerLoop, this);

    LOG_INFO("EventMonitor: inotify active, %d watches registered", (int)watches_.size());

    // Start netlink uevent listener (real-time events, replaces unreliable inotify on sysfs)
    // Uevent callback maps UeventEventType to EventType and calls triggerEvent.
    // Global cooldown in triggerEvent prevents double-counting with polling fallback.
    auto ueventCallback = [this](aatpe::event::UeventEventType ueType, const std::string& detail) {
        EventType evType;
        switch (ueType) {
            case aatpe::event::UeventEventType::FREQ_CHANGE:
                evType = EventType::FREQ_CHANGE;
                // magnitude gate is now centralized in triggerEvent(),
                // so we no longer call freqChangePassesGate() here. This ensures
                // ALL paths (uevent/inotify/polling) share the same gate state.
                break;
            case aatpe::event::UeventEventType::HOTPLUG:
                evType = EventType::HOTPLUG; break;
            case aatpe::event::UeventEventType::THERMAL_SPIKE:
                evType = EventType::THERMAL_SPIKE; break;
            default:
                return;  // Unknown type, ignore
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

    // P2: inotify watch limit (256) — prevent exceeding per-user inotify
    // watch quota which would cause ENOSPC and silent event loss.
    static constexpr int kMaxWatches = 256;
    {
        std::lock_guard<std::mutex> lock(watchesMutex_);
        if ((int)watches_.size() >= kMaxWatches) {
            LOG_WARN("EventMonitor: inotify watch limit (%d) reached, skipping: %s",
                     kMaxWatches, path.c_str());
            return false;
        }
    }

    // Check if path exists
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
        tv.tv_usec = 100000; // 100ms poll interval for fallback

        int ret = select(inotifyFd_ + 1, &fds, nullptr, nullptr, &tv);
        // P2-3 fix: distinguish EINTR from real errors. Previously `if (ret <= 0)
        // continue` treated EBADF/EFAULT identically to EINTR and to the 100ms
        // timeout, so a broken inotify fd produced a 100ms tight busy-loop. EINTR
        // (signal) simply re-polls; any other errno is fatal to this fd — log and
        // exit the loop rather than spin.
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("EventMonitor: select failed (%s), exiting listener loop", aatpeErrStr(errno));
            break;
        }
        if (ret == 0) continue;  // 100ms timeout — normal fallback cadence

        ssize_t len = read(inotifyFd_, buf, sizeof(buf));
        if (len <= 0) continue;

        // Collect matching events under watchesMutex_, then
        // release the lock BEFORE calling triggerEvent(). Holding watchesMutex_
        // during triggerEvent() can deadlock because triggerEvent acquires
        // logging locks and may invoke user callbacks that acquire other locks.
        // LOCK ORDERING — watchesMutex_ is the innermost lock.
        // It must NEVER be held while calling triggerEvent(), LOG_*(), or any
        // function that may acquire callbacksMutex_ / event history locks.
        struct PendingEvent { EventType type; std::string detail; };
        std::vector<PendingEvent> pending;

        for (char* ptr = buf; ptr < buf + len; ) {
            // P2-11: a short read can leave a truncated event at the tail. Validate
            // the fixed header AND the variable name length against the buffer
            // before dereferencing, otherwise event->wd / event->len read OOB.
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
            } // watchesMutex_ released here
            ptr += sizeof(struct inotify_event) + event->len;
        }

        // Trigger events outside the lock (P2#4)
        // magnitude gate is centralized in triggerEvent(), so no
        // per-type gate check is needed here. All paths share lastAcceptedFreqKhz_.
        for (const auto& pe : pending) {
            triggerEvent(pe.type, pe.detail);
        }
    }
}

// Frequency magnitude gate for FREQ_CHANGE events from uevent/inotify.
// Reads current perf max freq across big+mid cores, compares with last accepted.
// raised to >=25% relative OR >=400MHz absolute. On Dimensity
// 1100 the big core oscillates 1938-2600MHz; 2292->1938 is 354MHz = 15.4%,
// which used to straddle the old 15%/200MHz gate and fire ~1 event/sec, each
// waking an immediate full thread scan+migrate (main thread pinned at 53.5% CPU).
// The larger gate absorbs normal DVFS hunting while still reacting to real
// working-point jumps (e.g. entering/leaving a game governor step).
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
    if (curFreq <= 0) return true;  // can't read sysfs, don't block the event

    int last = lastAcceptedFreqKhz_.load(std::memory_order_relaxed);
    if (last <= 0) {
        // First reading - accept and seed the baseline
        lastAcceptedFreqKhz_.store(curFreq, std::memory_order_relaxed);
        return true;
    }

    int diff = std::abs(curFreq - last);
    int relGate = last * 25 / 100;  // 25% relative threshold (was 15% — too sensitive)
    const int absGate = 400000;     // 400MHz absolute threshold (kHz) (was 200MHz)

    if (diff >= relGate || diff >= absGate) {
        lastAcceptedFreqKhz_.store(curFreq, std::memory_order_relaxed);
        return true;
    }
    return false;  // change too small, skip
}

void EventMonitor::triggerEvent(EventType type, const std::string& detail) {
    long long now = nowMs();

    // Global per-event-type cooldown to prevent double-counting.
    // Both inotify and polling may detect the same event; this ensures
    // each event type is counted at most once per cooldown window.
    int typeIdx = static_cast<int>(type);
    // P3-14: bound the index by the enum sentinel rather than a hardcoded 5, so
    // adding a new EventType doesn't silently index past the cooldown array.
    if (typeIdx >= 0 && typeIdx < (int)EventType::EVENT_TYPE_COUNT && kEventCooldownMs[typeIdx] > 0) {
        // Atomic cooldown claim: CAS loop ensures only one thread per cooldown
        // window wins the right to process this event type. Prevents two threads
        // from both loading the same `last` timestamp and both passing the gate.
        long long prev = lastEventTimeMs_[typeIdx].load(std::memory_order_acquire);
        while (true) {
            if (now - prev < kEventCooldownMs[typeIdx]) {
                return;  // Within cooldown, skip
            }
            if (lastEventTimeMs_[typeIdx].compare_exchange_weak(
                    prev, now, std::memory_order_acq_rel, std::memory_order_acquire)) {
                break;  // won the cooldown claim
            }
            // prev was updated by CAS failure; retry the check.
        }
    }

    // Centralized magnitude gate for FREQ_CHANGE.
    // All paths (uevent, inotify, polling) funnel through here so the gate
    // cannot be bypassed by any caller. Small DVFS oscillations (<25% relative
    // OR <400MHz absolute) are silently dropped without logging or counting.
    // Reads scaling_cur_freq from perf cores and compares against the last
    // ACCEPTED frequency (not the last sampled value), preventing slow drift
    // from accumulating into false triggers.
    if (type == EventType::FREQ_CHANGE) {
        if (!freqChangePassesGate()) {
            return;  // magnitude too small, drop event silently
        }
    }

    // P3-2: Log event with detail for diagnostics
    const char* typeName = "UNKNOWN";
    switch (type) {
        case EventType::JANK: typeName = "JANK"; break;
        case EventType::THERMAL_SPIKE: typeName = "THERMAL_SPIKE"; break;
        case EventType::FREQ_CHANGE: typeName = "FREQ_CHANGE"; break;
        case EventType::HOTPLUG: typeName = "HOTPLUG"; break;
        case EventType::APP_SWITCH: typeName = "APP_SWITCH"; break;
        default: break; // EVENT_TYPE_COUNT
    }
    if (!detail.empty()) {
        LOG_INFO("EventMonitor: %s event triggered (detail: %s)", typeName, detail.c_str());
    }

    switch (type) {
        case EventType::JANK:
            jankCount_++;
            // P3-15: the check-then-set on lastHighEventMs_ was a non-atomic
            // read-modify-write window; two concurrent triggers could both pass
            // the cooldown check. highPriorityPending_ is idempotent, but we
            // still want only one winner per cooldown window. Use a CAS on the
            // timestamp: the thread that successfully publishes `now` claims the
            // wake; losers observe the updated timestamp and skip.
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
            // FREQ_CHANGE must NOT wake the main loop for a full
            // decision. Under thermal throttling the DVFS governor flips
            // scaling_cur_freq several times/sec across 1451/1938/2410/2600 MHz;
            // the old medium-priority wakeup turned each DVFS flip into an
            // immediate full placement pass, which is what pegged the daemon at
            // 125-144% CPU (state=R busy-spin). FREQ_CHANGE is now LOW priority:
            // it is only counted (for diagnostics); topology/capacity is picked
            // up lazily by the next regular readCpuMetrics() tick. No wakeup, no
            // frame/refresh re-detect, no placement.
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
            // Low priority - no immediate trigger, normal polling handles it
            break;
        default: break; // EVENT_TYPE_COUNT
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
        default: return 0; // EVENT_TYPE_COUNT
    }
    return 0;
}

long long EventMonitor::getTotalEvents() const {
    return jankCount_.load() + thermalCount_.load() + freqCount_.load()
         + hotplugCount_.load() + appSwitchCount_.load();
}

// merge: phase1 item9 - 事件层查询
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
    // P2-5: Add app_switch count so event_total breakdown is complete.
    // Previously event_total included APP_SWITCH but no per-type file existed.
    writeFile((stateDir + "/event_app_switch_count").c_str(), std::to_string(appSwitchCount_.load()));
    writeFile((stateDir + "/event_total").c_str(), std::to_string(getTotalEvents()));
    writeFile((stateDir + "/event_inotify_active").c_str(), inotifyActive_.load() ? "1" : "0");
    // uevent active status for diagnostics
    writeFile((stateDir + "/event_uevent_active").c_str(),
              aatpe::event::UeventMonitor::instance().isActive() ? "1" : "0");
    writeFile((stateDir + "/event_active_layer").c_str(), getActiveLayer());
}

void EventMonitor::shutdown() {
    running_.store(false);
    // Stop uevent listener first (unblocks netlink recv)
    aatpe::event::UeventMonitor::instance().stop();
    if (listenerThread_.joinable()) {
        listenerThread_.join();
    }
    if (inotifyFd_ >= 0) {
        close(inotifyFd_);
        inotifyFd_ = -1;
    }
    // LED-102 (R7-017): the inotify watch descriptors (wd) are invalid once the fd
    // is closed. Without clearing watches_ a subsequent init() would append new
    // watches onto a vector of stale wds — the stale entries would still count
    // against the kMaxWatches budget and could match nothing (or a recycled wd).
    // Drop them so re-init starts from a clean watch list.
    {
        std::lock_guard<std::mutex> wlock(watchesMutex_);
        watches_.clear();
    }
    inotifyActive_.store(false);
}

} // namespace event
} // namespace aatpe
