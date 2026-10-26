// AATPE Uevent Monitor Implementation (fix90)
// NETLINK_KOBJECT_UEVENT socket listener for real-time kernel events.
// Replaces unreliable inotify on sysfs files.

#include "uevent_monitor.h"
#include "aatped.h"  // for LOG_INFO/LOG_WARN
#include "aatped_uevent.h"  // LED-099: g_ueventEventCount

#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <fstream>   // v66 audit P2#20: /proc/net/netlink drop check
#include <sstream>   // v66 audit P2#20: parse netlink stats
// .cpp files may use using namespace (not included elsewhere)
using namespace aatpe;
using namespace aatpe_ml;


namespace aatpe {
namespace event {

UeventMonitor& UeventMonitor::instance() {
    static UeventMonitor inst;
    return inst;
}

UeventMonitor::UeventMonitor() {}

UeventMonitor::~UeventMonitor() {
    stop();
}

int UeventMonitor::createSocket() {
    int sock = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (sock < 0) {
        char ebuf[64]; int e = errno;
        LOG_WARN("UeventMonitor: socket() failed: %s", aatpeStrError(e, ebuf, sizeof(ebuf)));
        return -1;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    // Only subscribe to kobject uevent group (1<<0), not all
    // groups (0xffffffff). Subscribing to all multicast groups causes unnecessary
    // message flooding and increased kernel overhead. Group 1 is the standard
    // KOBJECT_UEVENT group for device hotplug/change events.
    addr.nl_groups = 1;

    // Set receive buffer to 1MB to reduce drop probability under
    // high event load (e.g. thermal storm, CPU hotplug burst). Default SO_RCVBUF
    // is often only 128KB, insufficient for bursty uevent traffic.
    int rcvbuf = 1024 * 1024;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        char ebuf[64]; int e = errno;
        LOG_DEBUG("UeventMonitor: setsockopt(SO_RCVBUF) failed: %s (using default)",
                  aatpeStrError(e, ebuf, sizeof(ebuf)));
        (void)e; (void)ebuf;  // LOG_DEBUG is a no-op in release
    }

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        char ebuf[64]; int e = errno;
        LOG_WARN("UeventMonitor: bind() failed: %s", aatpeStrError(e, ebuf, sizeof(ebuf)));
        close(sock);
        return -1;
    }

    LOG_INFO("UeventMonitor: netlink socket created successfully (fd=%d)", sock);
    return sock;
}

UeventMessage UeventMonitor::parseUevent(const char* buffer, size_t len) {
    // Fix-B: UNKNOWN=99 (not 0), so value-initialization alone doesn't give us
    // UNKNOWN. Explicitly set type to the safe default on every entry, including
    // the early-return paths (nullptr/empty).
    UeventMessage msg;
    msg.type = UeventEventType::UNKNOWN;
    if (!buffer || len == 0) return msg;

    msg.raw.assign(buffer, len);
    msg.type = UeventEventType::UNKNOWN;

    // Uevent format: null-separated strings
    // First string: "action@devpath" (e.g. "change@/devices/system/cpu/cpu0/cpufreq")
    // Subsequent strings: "KEY=VALUE" (e.g. "SUBSYSTEM=cpu")
    size_t pos = 0;
    bool first = true;

    while (pos < len) {
        // Find next null terminator
        size_t end = pos;
        while (end < len && buffer[end] != '\0') end++;

        if (end == pos) {
            pos = end + 1;
            continue;
        }

        std::string field(buffer + pos, end - pos);

        if (first) {
            // First field: "action@devpath"
            size_t atPos = field.find('@');
            if (atPos != std::string::npos) {
                msg.action = field.substr(0, atPos);
                msg.devpath = field.substr(atPos + 1);
            } else {
                msg.action = field;
            }
            first = false;
        } else {
            // KEY=VALUE pairs
            size_t eqPos = field.find('=');
            if (eqPos != std::string::npos) {
                std::string key = field.substr(0, eqPos);
                std::string value = field.substr(eqPos + 1);
                if (key == "SUBSYSTEM") {
                    msg.subsystem = value;
                }
            }
        }

        pos = end + 1;
    }

    msg.type = classifyMessage(msg);
    return msg;
}

UeventEventType UeventMonitor::classifyMessage(const UeventMessage& msg) {
    // CPU frequency change: the cpufreq node is unambiguous and is the only thing
    // that should wake a FREQ_CHANGE. (G4-003: a generic subsystem=cpu change on
    // ARM64 is almost always a CPU online/offline hotplug, NOT a freq change; it
    // used to be mis-routed to FREQ_CHANGE and then silently dropped by the
    // magnitude gate, leaving the HOTPLUG netlink path dead.)
    if (msg.devpath.find("cpufreq") != std::string::npos) {
        return UeventEventType::FREQ_CHANGE;
    }

    // R2-L4: a real CPU hotplug node looks like /cpu0, /cpu128/... — the "/cpu"
    // token is immediately followed by a DIGIT. The old bare find("/cpu") also
    // matched "/devices/system/cpu/cpuidle/..." (after "/cpu" comes 'i'), producing
    // a spurious HOTPLUG -> mediumPriorityPending_ wakeup on every idle-tick uevent
    // (harmless but wasteful). We scan for "/cpu<digit>" and explicitly require the
    // digit, which excludes cpuidle ("/cpu/cpuidle") and the bare "/cpu" container.
    auto isHotplugCpuNode = [](const std::string& devpath) -> bool {
        size_t pos = 0;
        while ((pos = devpath.find("/cpu", pos)) != std::string::npos) {
            size_t after = pos + 4; // strlen("/cpu")
            if (after < devpath.size() && devpath[after] >= '0' && devpath[after] <= '9') {
                return true;
            }
            pos = after;
        }
        return false;
    };

    // CPU hotplug:
    //  (a) device add/remove under a cpu node (rare on ARM64; cpus are present at
    //      boot and only go online/offline), and
    //  (b) action=change on a cpu kobject (online/offline transition) whose devpath
    //      references a /cpuN node but is NOT the cpufreq subtree.
    if (isHotplugCpuNode(msg.devpath)) {
        if (msg.action == "add" || msg.action == "remove" || msg.action == "change") {
            return UeventEventType::HOTPLUG;
        }
    }

    // Thermal: devpath contains thermal_zone or subsystem=thermal
    if (msg.devpath.find("thermal_zone") != std::string::npos ||
        msg.subsystem == "thermal") {
        return UeventEventType::THERMAL_SPIKE;
    }

    return UeventEventType::UNKNOWN;
}

void UeventMonitor::listenerLoop() {
    // 32KB buffer: netlink uevents can exceed 8KB (especially with many env vars).
    // Use MSG_TRUNC to detect truncated datagrams and drop them rather than
    // parsing a half-complete message.
    static constexpr size_t kBufSize = 32768;
    char buffer[kBufSize];

    static int s_consecRecvErrors = 0;  // H2: persistent-error backoff counter

    while (running_.load()) {
        // LED-087: load the fd once per iteration; stop() may exchange it to -1.
        int fd = sockFd_.load(std::memory_order_relaxed);
        if (fd < 0) break;

        // Poll with a 200ms timeout so we can re-check running_ without relying
        // on close() to wake a blocking recv(). The socket is O_NONBLOCK.
        struct pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            char ebuf[64]; int e = errno;
            LOG_WARN("UeventMonitor: poll() failed: %s", aatpeStrError(e, ebuf, sizeof(ebuf)));
            usleep(100000);
            continue;
        }
        if (pr == 0) continue;  // timeout: re-check running_

        memset(buffer, 0, sizeof(buffer));
        // MSG_TRUNC returns the full datagram length even if it exceeds the buffer,
        // so we can detect truncation and drop the message rather than parse garbage.
        ssize_t n = recv(fd, buffer, sizeof(buffer), MSG_TRUNC);

        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF) continue;  // stop() closed the fd
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // spurious wakeup
            // H2: backoff on persistent recv errors. After 10 consecutive hard
            // errors, sleep 1s instead of 100ms to avoid log-spam / busy loop.
            s_consecRecvErrors++;
            int sleepUs = (s_consecRecvErrors >= 10) ? 1000000 : 100000;
            char ebuf[64]; int e = errno;
            LOG_WARN("UeventMonitor: recv() failed (#%d): %s; sleeping %dms",
                     s_consecRecvErrors, aatpeStrError(e, ebuf, sizeof(ebuf)), sleepUs / 1000);
            usleep(sleepUs);
            continue;
        }
        // Success: reset error counter.
        s_consecRecvErrors = 0;

        if (n == 0) continue;

        // If the datagram was truncated (n > buffer size), drop it — parsing a
        // half-complete uevent would produce wrong classification.
        if ((size_t)n > sizeof(buffer)) {
            LOG_WARN("UeventMonitor: uevent truncated (datagram=%zd buf=%zu), dropping",
                     n, sizeof(buffer));
            continue;
        }

        totalCount_.fetch_add(1);
        // LED-099 (R7-014): checkUeventFallback() activates sysfs polling when
        // g_ueventEventCount stays 0 after the grace window. Nothing ever
        // incremented this counter, so the fallback ALWAYS activated even while
        // netlink uevents were flowing. Bump it here on every received uevent so
        // a working netlink listener suppresses the redundant sysfs poll.
        g_ueventEventCount.fetch_add(1, std::memory_order_relaxed);

        UeventMessage msg = parseUevent(buffer, (size_t)n);

        if (msg.type == UeventEventType::UNKNOWN) continue;

        // Update per-type counters
        switch (msg.type) {
            case UeventEventType::FREQ_CHANGE:
                freqCount_.fetch_add(1);
                break;
            case UeventEventType::HOTPLUG:
                hotplugCount_.fetch_add(1);
                break;
            case UeventEventType::THERMAL_SPIKE:
                thermalCount_.fetch_add(1);
                break;
            default:
                break;
        }

        // Invoke callback
        if (callback_) {
            std::string detail = "action:" + msg.action + " devpath:" + msg.devpath;
            callback_(msg.type, detail);
        }

        // Periodically check netlink drop counters via
        // /proc/net/netlink. If drops are increasing, log a warning so the
        // operator can increase SO_RCVBUF or investigate event processing latency.
        static int s_eventCount = 0;
        static uint32_t s_lastDrops = 0;
        static const int kDropCheckInterval = 100;
        if (++s_eventCount >= kDropCheckInterval) {
            s_eventCount = 0;
            std::ifstream nlFile("/proc/net/netlink");
            if (nlFile.good()) {
                std::string line;
                // LED-105 (R7-020): /proc/net/netlink's column layout can change
                // across kernel versions. Parse the header to locate the "Drops"
                // (and "Pid") columns by NAME instead of hardcoding field #9/#3.
                int pidCol = -1, dropsCol = -1;
                if (std::getline(nlFile, line)) {
                    std::istringstream hdr(line);
                    std::string col;
                    int idx = 0;
                    while (hdr >> col) {
                        idx++;  // 1-based
                        std::string low = col;
                        for (auto& c : low) c = (char)tolower((unsigned char)c);
                        if (low == "pid") pidCol = idx;
                        else if (low == "drops") dropsCol = idx;
                    }
                }
                // Fall back to the long-standing layout if the header was
                // unparseable (Pid=3, Drops=9, 1-based).
                if (pidCol < 0) pidCol = 3;
                if (dropsCol < 0) dropsCol = 9;
                int myPid = (int)getpid();
                while (std::getline(nlFile, line)) {
                    std::istringstream iss(line);
                    std::string token;
                    int skPid = 0;
                    uint32_t drops = 0;
                    int field = 0;
                    bool gotPid = false, gotDrops = false;
                    while (iss >> token) {
                        field++;
                        if (field == pidCol)  { skPid = parseIntSafe(token); gotPid = true; }
                        if (field == dropsCol){ drops = (uint32_t)strtoul(token.c_str(), nullptr, 10); gotDrops = true; }
                    }
                    if (gotPid && skPid == myPid) {
                        if (gotDrops && drops > s_lastDrops) {
                            LOG_WARN("UeventMonitor: netlink drops increased: %u (delta=%u)",
                                     drops, drops - s_lastDrops);
                            s_lastDrops = drops;
                        }
                        break;
                    }
                }
            }
        }
    }

    // MON-05 fix: the listener closes its OWN fd right before signalling exit.
    // This eliminates the close-then-reuse race where stop() closed the fd while
    // the listener was still in poll(): between close() and the listener's next
    // poll(), the kernel could hand the fd number to a different socket. Now the
    // listener is the only thread that closes its fd, and it does so AFTER the
    // final recv/poll returns, so no other thread ever references the number.
    int fd = sockFd_.exchange(-1);
    if (fd >= 0) {
        ::close(fd);
    }

    // signal that the listener loop has returned. stop() polls this to
    // bound its join(); the store is the last action so stop() can safely join
    // immediately after observing loopExited_ == true.
    loopExited_.store(true);
}

bool UeventMonitor::start(const UeventCallback& callback) {
    if (running_.load()) {
        LOG_WARN("UeventMonitor: already running");
        return active_.load();
    }

    callback_ = callback;

    sockFd_.store(createSocket());
    if (sockFd_.load() < 0) {
        active_.store(false);
        return false;
    }

    running_.store(true);
    active_.store(true);
    loopExited_.store(false);  // arm the exit flag for this listener session

    try {
        listenerThread_ = std::thread(&UeventMonitor::listenerLoop, this);
    } catch (const std::exception& e) {
        LOG_WARN("UeventMonitor: failed to start listener thread: %s", e.what());
        running_.store(false);
        active_.store(false);
        int fd = sockFd_.exchange(-1);
        if (fd >= 0) close(fd);
        return false;
    }

    LOG_INFO("UeventMonitor: listener thread started successfully");
    return true;
}

void UeventMonitor::stop() {
    if (!running_.load()) return;

    // clear the atomic stop flag FIRST, before touching the socket, so that as
    // soon as recv() returns for any reason (EINTR, error, poll timeout) the loop
    // observes running_ == false and exits.
    running_.store(false);

    // B-007 (fd-reuse race): WAIT for the listener thread to leave the recv/poll
    // loop BEFORE we close() its fd. The socket is O_NONBLOCK and the loop polls
    // with a 200ms timeout, so clearing running_ reliably makes the loop exit on its
    // own within ~one poll tick. The previous code close()d the fd first; between
    // that close() and the listener's next poll(fd), the kernel could immediately
    // hand the just-freed fd number to a different open()/socket() call, so the
    // listener's poll() then operated on the WRONG descriptor (classic
    // close-then-reuse race). Closing only AFTER loopExited_ == true guarantees the
    // listener is no longer referencing the number.
    bool exited = false;
    if (listenerThread_.joinable()) {
        const auto deadline = std::chrono::steady_clock::now() + kJoinTimeout;
        while (!loopExited_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        exited = loopExited_.load(std::memory_order_acquire);
    }

    // MON-05 fix: in the NORMAL path the listener has already closed its own fd
    // (see listenerLoop exit). We still exchange() to -1 here so that:
    //   1) any in-flight sockFd_.load() in the loop sees -1 and exits, and
    //   2) if the listener is wedged past kJoinTimeout (exited==false), the close
    //      below doubles as the wakeup. In that fallback window the close may race
    //      with a still-blocked poll(), but that is the documented last-resort
    //      path (process-lifetime singleton; OS reaps the detached thread).
    int fd = sockFd_.exchange(-1);
    if (fd >= 0) {
        if (!exited) {
            // Listener is wedged; close to wake its poll().
            if (::close(fd) != 0) {
                char ebuf[64]; int e = errno;
                LOG_WARN("UeventMonitor: close(sockFd=%d) fallback wakeup failed: %s",
                         fd, aatpeStrError(e, ebuf, sizeof(ebuf)));
            } else {
                LOG_INFO("UeventMonitor: netlink socket closed to wake wedged recv()");
            }
        }
        // If exited==true, the listener already closed fd; don't double-close.
    }

    // Bounded join. The loop-exit flag was already waited above; join() only
    // reaps the now-finished thread. If an odd kernel wedged the listener past the
    // budget even after the close() wakeup, detach() it as a last-resort fallback
    // (left for process teardown) rather than hanging the hot-reload path.
    if (listenerThread_.joinable()) {
        if (exited) {
            listenerThread_.join();
        } else {
            LOG_WARN("UeventMonitor: listener thread did not exit within %d ms; "
                     "detaching as last-resort fallback (left for process teardown)",
                     (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                         kJoinTimeout).count());
            listenerThread_.detach();
        }
    }

    active_.store(false);
    LOG_INFO("UeventMonitor: stopped");
}

} // namespace event
} // namespace aatpe
