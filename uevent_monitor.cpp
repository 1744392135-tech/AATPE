
#include "uevent_monitor.h"
#include "aatped.h"
#include "aatped_uevent.h"

#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <fstream>
#include <sstream>
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
    addr.nl_groups = 1;

    int rcvbuf = 1024 * 1024;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        char ebuf[64]; int e = errno;
        LOG_DEBUG("UeventMonitor: setsockopt(SO_RCVBUF) failed: %s (using default)",
                  aatpeStrError(e, ebuf, sizeof(ebuf)));
        (void)e; (void)ebuf;
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
    UeventMessage msg;
    msg.type = UeventEventType::UNKNOWN;
    if (!buffer || len == 0) return msg;

    msg.raw.assign(buffer, len);
    msg.type = UeventEventType::UNKNOWN;

    size_t pos = 0;
    bool first = true;

    while (pos < len) {
        size_t end = pos;
        while (end < len && buffer[end] != '\0') end++;

        if (end == pos) {
            pos = end + 1;
            continue;
        }

        std::string field(buffer + pos, end - pos);

        if (first) {
            size_t atPos = field.find('@');
            if (atPos != std::string::npos) {
                msg.action = field.substr(0, atPos);
                msg.devpath = field.substr(atPos + 1);
            } else {
                msg.action = field;
            }
            first = false;
        } else {
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
    if (msg.devpath.find("cpufreq") != std::string::npos) {
        return UeventEventType::FREQ_CHANGE;
    }

    auto isHotplugCpuNode = [](const std::string& devpath) -> bool {
        size_t pos = 0;
        while ((pos = devpath.find("/cpu", pos)) != std::string::npos) {
            size_t after = pos + 4;
            if (after < devpath.size() && devpath[after] >= '0' && devpath[after] <= '9') {
                return true;
            }
            pos = after;
        }
        return false;
    };

    if (isHotplugCpuNode(msg.devpath)) {
        if (msg.action == "add" || msg.action == "remove" || msg.action == "change") {
            return UeventEventType::HOTPLUG;
        }
    }

    if (msg.devpath.find("thermal_zone") != std::string::npos ||
        msg.subsystem == "thermal") {
        return UeventEventType::THERMAL_SPIKE;
    }

    return UeventEventType::UNKNOWN;
}

void UeventMonitor::listenerLoop() {
    static constexpr size_t kBufSize = 32768;
    char buffer[kBufSize];

    static int s_consecRecvErrors = 0;

    while (running_.load()) {
        int fd = sockFd_.load(std::memory_order_relaxed);
        if (fd < 0) break;

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
        if (pr == 0) continue;

        memset(buffer, 0, sizeof(buffer));
        ssize_t n = recv(fd, buffer, sizeof(buffer), MSG_TRUNC);

        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            s_consecRecvErrors++;
            int sleepUs = (s_consecRecvErrors >= 10) ? 1000000 : 100000;
            char ebuf[64]; int e = errno;
            LOG_WARN("UeventMonitor: recv() failed (#%d): %s; sleeping %dms",
                     s_consecRecvErrors, aatpeStrError(e, ebuf, sizeof(ebuf)), sleepUs / 1000);
            usleep(sleepUs);
            continue;
        }
        s_consecRecvErrors = 0;

        if (n == 0) continue;

        if ((size_t)n > sizeof(buffer)) {
            LOG_WARN("UeventMonitor: uevent truncated (datagram=%zd buf=%zu), dropping",
                     n, sizeof(buffer));
            continue;
        }

        totalCount_.fetch_add(1);
        g_ueventEventCount.fetch_add(1, std::memory_order_relaxed);

        UeventMessage msg = parseUevent(buffer, (size_t)n);

        if (msg.type == UeventEventType::UNKNOWN) continue;

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

        if (callback_) {
            std::string detail = "action:" + msg.action + " devpath:" + msg.devpath;
            callback_(msg.type, detail);
        }

        static int s_eventCount = 0;
        static uint32_t s_lastDrops = 0;
        static const int kDropCheckInterval = 100;
        if (++s_eventCount >= kDropCheckInterval) {
            s_eventCount = 0;
            std::ifstream nlFile("/proc/net/netlink");
            if (nlFile.good()) {
                std::string line;
                int pidCol = -1, dropsCol = -1;
                if (std::getline(nlFile, line)) {
                    std::istringstream hdr(line);
                    std::string col;
                    int idx = 0;
                    while (hdr >> col) {
                        idx++;
                        std::string low = col;
                        for (auto& c : low) c = (char)tolower((unsigned char)c);
                        if (low == "pid") pidCol = idx;
                        else if (low == "drops") dropsCol = idx;
                    }
                }
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

    int fd = sockFd_.exchange(-1);
    if (fd >= 0) {
        ::close(fd);
    }

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
    loopExited_.store(false);

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

    running_.store(false);

    bool exited = false;
    if (listenerThread_.joinable()) {
        const auto deadline = std::chrono::steady_clock::now() + kJoinTimeout;
        while (!loopExited_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        exited = loopExited_.load(std::memory_order_acquire);
    }

    int fd = sockFd_.exchange(-1);
    if (fd >= 0) {
        if (!exited) {
            if (::close(fd) != 0) {
                char ebuf[64]; int e = errno;
                LOG_WARN("UeventMonitor: close(sockFd=%d) fallback wakeup failed: %s",
                         fd, aatpeStrError(e, ebuf, sizeof(ebuf)));
            } else {
                LOG_INFO("UeventMonitor: netlink socket closed to wake wedged recv()");
            }
        }
    }

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

}
}
