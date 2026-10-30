
#include "frame_binder.h"
#include "aatped_logging.h"
#include "aatpe_ml.h"
#include "aatped_types.h"
#include "aatped_utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <linux/android/binder.h>
#include <sys/system_properties.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <vector>
#include <chrono>
#include <atomic>

namespace aatpe {
namespace frame_binder {

static const char* kBinderDev       = "/dev/binder";
static const char* kSvcName         = "SurfaceFlinger";
static const char* kLatencyArg      = "--latency";

static constexpr uint32_t kDumpTransaction = 0x5F444D50u;

static constexpr uint32_t kSmCheckService = 1u;

static constexpr int64_t kCacheTtlMs = 500;

static constexpr int kDumpReadTimeoutMs = 1500;

static constexpr int kBinderTxnTimeoutMs = 2000;

static constexpr int kMaxFailures = 3;

static int                    g_binderFd     = -1;
static void*                  g_binderMmap  = MAP_FAILED;
static constexpr size_t      kBinderMmapSize = (1u << 20);
static uint32_t               g_sfHandle     = 0;
static BinderState            g_state        = BinderState::UNINITIALIZED;
static int                    g_sdkLevel     = 0;
static std::mutex             g_mu;
static FrameStats             g_cached;
static int64_t                g_lastRefreshMs = 0;
static int                    g_failCount    = 0;
static int64_t                g_lastFailMs  = 0;
static constexpr int64_t      kFallbackRetryBackoffMs = 30000;

static size_t writeString16(uint8_t* buf, size_t bufLen, const char* utf8) {
    size_t len = strlen(utf8);
    size_t needed = sizeof(int32_t) + len * 2 + 2;
    if (needed > bufLen) {
        LOG_WARN("FRAME_BINDER: writeString16 overflow (need=%zu buf=%zu)",
                 needed, bufLen);
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if ((uint8_t)utf8[i] >= 0x80) {
            LOG_WARN("FRAME_BINDER: writeString16: non-ASCII byte at offset %zu "
                     "(service names must be ASCII; got 0x%02x)", i, (uint8_t)utf8[i]);
            return 0;
        }
    }
    int32_t outLen = (int32_t)len;
    size_t off = 0;
    memcpy(buf + off, &outLen, sizeof(outLen));
    off += sizeof(outLen);
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)utf8[i];
        buf[off++] = c;
        buf[off++] = 0;
    }
    buf[off++] = 0;
    buf[off++] = 0;
    return off;
}

enum class ReadBatch { REPLY, MORE, ERROR };
static ReadBatch binderParseReadBatch(const uint8_t* readBuf, size_t readBufSize,
                                      struct binder_transaction_data* outReply) {
    size_t pos = 0;
    while (pos + sizeof(uint32_t) <= readBufSize) {
        uint32_t cmd;
        memcpy(&cmd, readBuf + pos, sizeof(cmd));
        pos += sizeof(cmd);
        switch (cmd) {
        case BR_NOOP:
            break;
        case BR_TRANSACTION_COMPLETE:
            break;
        case BR_TRANSACTION:
        case BR_REPLY: {
            if (pos + sizeof(*outReply) > readBufSize) {
                LOG_WARN("FRAME_BINDER: reply batch truncated (need=%zu have=%zu)",
                         sizeof(*outReply), readBufSize - pos);
                return ReadBatch::ERROR;
            }
            if (outReply) memcpy(outReply, readBuf + pos, sizeof(*outReply));
            return ReadBatch::REPLY;
        }
        case BR_DEAD_REPLY:
            LOG_WARN("FRAME_BINDER: BR_DEAD_REPLY (service died)");
            return ReadBatch::ERROR;
        case BR_FAILED_REPLY:
            LOG_WARN("FRAME_BINDER: BR_FAILED_REPLY");
            return ReadBatch::ERROR;
        case BR_ERROR: {
            int32_t err;
            if (pos + sizeof(err) > readBufSize) return ReadBatch::ERROR;
            memcpy(&err, readBuf + pos, sizeof(err));
            pos += sizeof(err);
            LOG_WARN("FRAME_BINDER: BR_ERROR %d", err);
            return ReadBatch::ERROR;
        }
        case BR_OK:
            return ReadBatch::MORE;
        default:
            LOG_WARN("FRAME_BINDER: unknown BR cmd=0x%08x", cmd);
            return ReadBatch::ERROR;
        }
    }
    return ReadBatch::MORE;
}

static int waitBinderReadable(int fd, int timeoutMs) {
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int rc = poll(&pfd, 1, timeoutMs);
    if (rc < 0) {
        LOG_WARN("FRAME_BINDER: poll() failed: %s", aatpeErrStr(errno));
        return -1;
    }
    if (rc == 0) {
        LOG_WARN("FRAME_BINDER: poll() timeout after %dms (SurfaceFlinger/servicemanager unresponsive)",
                 timeoutMs);
        return -2;
    }
    return 0;
}

enum class BwrWriteRc { TXN_SENT, WRITE_PENDING, FATAL };
static BwrWriteRc classifyWriteRead(int ioctlRc, int ioctlErrno,
                                    size_t writeConsumed, size_t writeSize) {
    if (ioctlRc == 0) return BwrWriteRc::TXN_SENT;
    if (ioctlErrno == EAGAIN || ioctlErrno == EWOULDBLOCK) {
        if (writeConsumed == writeSize) return BwrWriteRc::TXN_SENT;
        return BwrWriteRc::WRITE_PENDING;
    }
    return BwrWriteRc::FATAL;
}

static int sendTransaction(int fd, struct binder_transaction_data* txn,
                           struct binder_transaction_data* outReply) {
    struct {
        uint32_t cmd;
        struct binder_transaction_data txn;
    } __attribute__((packed)) writeCmd;
    writeCmd.cmd = BC_TRANSACTION;
    writeCmd.txn = *txn;

    uint8_t readBuf[8192];
    memset(readBuf, 0, sizeof(readBuf));

    struct binder_write_read bwr = {};
    bwr.write_size   = sizeof(writeCmd);
    bwr.write_buffer = reinterpret_cast<binder_uintptr_t>(&writeCmd);
    bwr.read_size    = sizeof(readBuf);
    bwr.read_buffer  = reinterpret_cast<binder_uintptr_t>(readBuf);

    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(kBinderTxnTimeoutMs);

    bool txnSent = false;
    while (!txnSent) {
        int rc = ioctl(fd, BINDER_WRITE_READ, &bwr);
        BwrWriteRc wrc = classifyWriteRead(rc, errno, bwr.write_consumed,
                                           bwr.write_size);
        if (wrc == BwrWriteRc::FATAL) {
            LOG_WARN("FRAME_BINDER: BINDER_WRITE_READ (txn) failed: %s",
                     aatpeErrStr(errno));
            return -1;
        }
        if (wrc == BwrWriteRc::TXN_SENT) {
            txnSent = true;
            break;
        }
        bwr.write_buffer += bwr.write_consumed;
        bwr.write_size   -= bwr.write_consumed;
        if (std::chrono::steady_clock::now() >= deadline) {
            LOG_WARN("FRAME_BINDER: txn write pump deadline exceeded");
            return -1;
        }
        usleep(2000);
    }

    struct binder_transaction_data reply = {};
    while (true) {
        ReadBatch rb = binderParseReadBatch(readBuf, bwr.read_consumed, &reply);
        if (rb == ReadBatch::REPLY) {
            if (outReply) *outReply = reply;
            return 0;
        }
        if (rb == ReadBatch::ERROR) return -1;

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            LOG_WARN("FRAME_BINDER: reply wait deadline exceeded (%dms, SurfaceFlinger/"
                     "servicemanager unresponsive)", kBinderTxnTimeoutMs);
            return -1;
        }
        int64_t remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - now).count();
        if (remainMs <= 0) return -1;
        if (waitBinderReadable(fd, (int)remainMs) != 0) return -1;

        memset(readBuf, 0, sizeof(readBuf));
        struct binder_write_read rbwr = {};
        rbwr.read_size   = sizeof(readBuf);
        rbwr.read_buffer = reinterpret_cast<binder_uintptr_t>(readBuf);
        if (ioctl(fd, BINDER_WRITE_READ, &rbwr) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG_WARN("FRAME_BINDER: BINDER_WRITE_READ (reply drain) failed: %s",
                     aatpeErrStr(errno));
            return -1;
        }
        bwr = rbwr;
    }
}

static uint32_t lookupSurfaceFlinger(int fd) {
    uint8_t parcel[128];
    memset(parcel, 0, sizeof(parcel));
    size_t parcelLen = writeString16(parcel, sizeof(parcel), kSvcName);
    if (parcelLen == 0) {
        LOG_WARN("FRAME_BINDER: service name too long for parcel");
        return 0;
    }

    struct binder_transaction_data txn = {};
    txn.target.handle  = 0;
    txn.code           = kSmCheckService;
    txn.flags          = 0;
    txn.data_size      = parcelLen;
    txn.offsets_size   = 0;
    txn.data.ptr.buffer  = reinterpret_cast<binder_uintptr_t>(parcel);
    txn.data.ptr.offsets = 0;

    struct binder_transaction_data reply = {};
    if (sendTransaction(fd, &txn, &reply) != 0) {
        LOG_WARN("FRAME_BINDER: servicemanager checkService failed");
        return 0;
    }

    if (reply.data_size < sizeof(flat_binder_object) || reply.data.ptr.buffer == 0) {
        LOG_WARN("FRAME_BINDER: SM reply invalid (size=%zu buffer=%llx, expect size>=%zu && buffer!=0)",
                 (size_t)reply.data_size, (unsigned long long)reply.data.ptr.buffer,
                 sizeof(flat_binder_object));
        return 0;
    }
    uintptr_t bufPtr = static_cast<uintptr_t>(reply.data.ptr.buffer);
    uintptr_t mmapStart = reinterpret_cast<uintptr_t>(g_binderMmap);
    uintptr_t bufEnd = bufPtr + reply.data_size;
    uintptr_t mmapEnd = mmapStart + kBinderMmapSize;
    if (g_binderMmap == MAP_FAILED ||
        bufPtr < mmapStart ||
        bufEnd < bufPtr ||
        mmapEnd < mmapStart ||
        bufEnd > mmapEnd) {
        LOG_WARN("FRAME_BINDER: reply buffer %p outside mmap region [%p, %p+%zu)",
                 (void*)bufPtr, (void*)mmapStart, (void*)mmapStart, kBinderMmapSize);
        return 0;
    }
    flat_binder_object* obj = reinterpret_cast<flat_binder_object*>(bufPtr);
    if (obj->hdr.type != BINDER_TYPE_HANDLE) {
        LOG_WARN("FRAME_BINDER: SM reply type=0x%08x (expected BINDER_TYPE_HANDLE)",
                 obj->hdr.type);
        return 0;
    }
    LOG_INFO("FRAME_BINDER: SurfaceFlinger handle = %u", obj->handle);
    return obj->handle;
}

static constexpr size_t kPipeReadMaxBytes = 256UL * 1024UL;
static constexpr int kPipeReadTotalMs = 5000;
static std::string readPipeBlocking(int fd, int timeoutMs) {
    std::string out;
    char buf[4096];
    int idleMs = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (idleMs < timeoutMs) {
        auto now = std::chrono::steady_clock::now();
        int elapsedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - t0).count();
        if (elapsedMs >= kPipeReadTotalMs) {
            LOG_WARN("FRAME_BINDER: pipe read total cap %dms hit (got %zu bytes)",
                     (int)kPipeReadTotalMs, out.size());
            break;
        }
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            size_t add = (size_t)n;
            if (out.size() + add > kPipeReadMaxBytes) {
                add = kPipeReadMaxBytes - out.size();
                if (add > 0) out.append(buf, add);
                LOG_WARN("FRAME_BINDER: pipe output capped at %zu bytes",
                         (size_t)kPipeReadMaxBytes);
                break;
            }
            out.append(buf, n);
            idleMs = 0;
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(5000);
                idleMs += 5;
            } else {
                break;
            }
        }
    }
    return out;
}

#ifdef AATPE_HOST_TEST
std::string readPipeBlockingPublic(int fd, int timeoutMs) {
    return readPipeBlocking(fd, timeoutMs);
}
#endif

static void parseLatencyOutput(const std::string& text, FrameStats& stats) {
    std::vector<long long> frameDurationsUs;
    long long minIntervalNs = 0;
    long long firstPresentNs = 0, lastPresentNs = 0;

    std::istringstream iss(text);
    std::string line;
    bool firstLine = true;
    while (std::getline(iss, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end   = line.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        std::string s = line.substr(start, end - start + 1);

        if (firstLine) {
            firstLine = false;
            long long period = 0;
            if (sscanf(s.c_str(), "%lld", &period) == 1 &&
                period >= 5000000LL && period <= 200000000LL) {
                minIntervalNs = period;
            }
            continue;
        }

        long long v[3] = {0, 0, 0};
        int n = sscanf(s.c_str(), "%lld %lld %lld", &v[0], &v[1], &v[2]);
        if (n != 2 && n != 3) continue;
        if (v[0] == -32768 || v[1] == -32768) continue;
        const long long kTsMaxNs = 2000000000000LL;
        if (v[0] <= 0 || v[1] <= 0 || v[0] > kTsMaxNs || v[1] > kTsMaxNs) continue;
        long long latNs = v[1] - v[0];
        if (latNs > 0 && latNs < 200000000LL) {
            frameDurationsUs.push_back(latNs / 1000);
            if (firstPresentNs == 0) firstPresentNs = v[1];
            lastPresentNs = v[1];
        }
    }

    if (frameDurationsUs.empty()) return;

    std::vector<long long> sorted = frameDurationsUs;
    std::sort(sorted.begin(), sorted.end());
    size_t n = sorted.size();

    long long sum = 0;
    for (auto v : sorted) sum += v;
    double avg = (double)sum / (double)n;

    auto pct = [&](double p) -> double {
        if (n == 0) return -1;
        size_t idx = (size_t)(p * (double)(n - 1));
        if (idx >= n) idx = n - 1;
        return (double)sorted[idx];
    };

    stats.valid = true;
    stats.currentFrameTimeUs = (double)frameDurationsUs.back();
    stats.averageFrameTimeUs = avg;
    stats.p90FrameTimeUs = pct(0.90);
    stats.p95FrameTimeUs = pct(0.95);
    stats.p99FrameTimeUs = pct(0.99);
    stats.sampleCount = (int)n;
    stats.vsyncPeriodNs = minIntervalNs;
    stats.firstPresentNs = firstPresentNs;
    stats.lastPresentNs = lastPresentNs;

    if (minIntervalNs > 0) {
        double thresholdUs = (double)(minIntervalNs * 2) / 1000.0;
        int64_t jank = 0;
        for (auto v : sorted) {
            if ((double)v > thresholdUs) jank++;
        }
        stats.jankCount = jank;
    }
}

static bool fetchLatency(int fd, uint32_t sfHandle,
                         const std::vector<std::string>& argv, FrameStats& out) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) {
        LOG_WARN("FRAME_BINDER: pipe2 failed: %s", aatpeErrStr(errno));
        return false;
    }

    uint8_t dataBuf[8192];
    memset(dataBuf, 0, sizeof(dataBuf));

    struct binder_fd_object fdObj = {};
    fdObj.hdr.type = BINDER_TYPE_FD;
    fdObj.pad_flags = 0;
    fdObj.fd = pipefd[1];
    fdObj.cookie = 0;
    memcpy(dataBuf, &fdObj, sizeof(fdObj));

    size_t off = sizeof(fdObj);
    int32_t count = (int32_t)argv.size();
    memcpy(dataBuf + off, &count, sizeof(count));
    off += sizeof(count);
    for (const auto& arg : argv) {
        size_t strBytes = writeString16(dataBuf + off, sizeof(dataBuf) - off, arg.c_str());
        if (strBytes == 0) {
            LOG_WARN("FRAME_BINDER: latency arg too long for dataBuf (arg='%s')", arg.c_str());
            close(pipefd[0]);
            close(pipefd[1]);
            return false;
        }
        off += strBytes;
    }

    binder_size_t offsets[1] = { 0 };

    struct binder_transaction_data txn = {};
    txn.target.handle  = sfHandle;
    txn.code           = kDumpTransaction;
    txn.flags          = 0;
    txn.data_size      = off;
    txn.offsets_size   = sizeof(offsets);
    txn.data.ptr.buffer  = reinterpret_cast<binder_uintptr_t>(dataBuf);
    txn.data.ptr.offsets = reinterpret_cast<binder_uintptr_t>(offsets);

    struct binder_transaction_data reply = {};
    int rc = sendTransaction(fd, &txn, &reply);

    close(pipefd[1]);

    if (rc != 0) {
        LOG_WARN("FRAME_BINDER: DUMP_TRANSACTION to SurfaceFlinger failed");
        close(pipefd[0]);
        return false;
    }

    std::string dump = readPipeBlocking(pipefd[0], kDumpReadTimeoutMs);
    close(pipefd[0]);

    if (dump.empty()) {
        LOG_WARN("FRAME_BINDER: SurfaceFlinger --latency pipe empty");
        return false;
    }

    parseLatencyOutput(dump, out);
    if (!out.valid) {
        LOG_WARN("FRAME_BINDER: latency parse produced no valid samples (text len=%zu)",
                 dump.size());
        return false;
    }
    return true;
}

static void detectSdkLevel() {
    g_sdkLevel = aatpeAndroidSdkInt();
    if (g_sdkLevel > 0) {
        LOG_INFO("FRAME_BINDER: Android SDK level = %d", g_sdkLevel);
    } else {
        LOG_WARN("FRAME_BINDER: could not detect Android SDK level, assuming generic");
    }
}

bool init() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_state == BinderState::READY) return true;
    if (g_state == BinderState::DEAD) return false;

    if (g_binderFd >= 0) {
        close(g_binderFd);
        g_binderFd = -1;
    }
    if (g_binderMmap != MAP_FAILED) {
        munmap(g_binderMmap, kBinderMmapSize);
        g_binderMmap = MAP_FAILED;
    }

    detectSdkLevel();

    g_binderFd = open(kBinderDev, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (g_binderFd < 0) {
        LOG_WARN("FRAME_BINDER: cannot open %s: %s", kBinderDev, aatpeErrStr(errno));
        g_state = BinderState::FALLBACK_NEEDED;
        return false;
    }

    struct binder_version bv = {};
    if (ioctl(g_binderFd, BINDER_VERSION, &bv) < 0) {
        LOG_WARN("FRAME_BINDER: BINDER_VERSION ioctl failed: %s", aatpeErrStr(errno));
        close(g_binderFd);
        g_binderFd = -1;
        g_state = BinderState::FALLBACK_NEEDED;
        return false;
    }
    LOG_INFO("FRAME_BINDER: binder protocol version = %d", bv.protocol_version);

    g_binderMmap = mmap(nullptr, kBinderMmapSize, PROT_READ,
                        MAP_PRIVATE, g_binderFd, 0);
    if (g_binderMmap == MAP_FAILED) {
        LOG_WARN("FRAME_BINDER: mmap(%zu) failed: %s (direct path unusable)",
                 (size_t)kBinderMmapSize, aatpeErrStr(errno));
        close(g_binderFd);
        g_binderFd = -1;
        g_binderMmap = MAP_FAILED;
        g_state = BinderState::FALLBACK_NEEDED;
        return false;
    }

    uint32_t maxThreads = 1;
    if (ioctl(g_binderFd, BINDER_SET_MAX_THREADS, &maxThreads) < 0) {
        LOG_WARN("FRAME_BINDER: BINDER_SET_MAX_THREADS failed: %s", aatpeErrStr(errno));
    }

    uint32_t enterCmd = BC_ENTER_LOOPER;
    {
        struct binder_write_read bwr = {};
        bwr.write_size   = sizeof(enterCmd);
        bwr.write_buffer = reinterpret_cast<binder_uintptr_t>(&enterCmd);
        int ec = ioctl(g_binderFd, BINDER_WRITE_READ, &bwr);
        if (ec < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            bwr.write_consumed == sizeof(enterCmd)) {
            ec = 0;
        }
        if (ec < 0) {
            LOG_WARN("FRAME_BINDER: BC_ENTER_LOOPER failed: %s", aatpeErrStr(errno));
            if (g_binderMmap != MAP_FAILED) {
                munmap(g_binderMmap, kBinderMmapSize);
                g_binderMmap = MAP_FAILED;
            }
            close(g_binderFd);
            g_binderFd = -1;
            g_state = BinderState::FALLBACK_NEEDED;
            return false;
        }
    }

    g_sfHandle = lookupSurfaceFlinger(g_binderFd);
    if (g_sfHandle == 0) {
        LOG_WARN("FRAME_BINDER: SurfaceFlinger service not found via Binder");
        if (g_binderMmap != MAP_FAILED) {
            munmap(g_binderMmap, kBinderMmapSize);
            g_binderMmap = MAP_FAILED;
        }
        close(g_binderFd);
        g_binderFd = -1;
        g_state = BinderState::FALLBACK_NEEDED;
        return false;
    }

    g_state = BinderState::READY;
    g_failCount = 0;
    LOG_INFO("FRAME_BINDER: Binder SurfaceFlinger connection established (handle=%u, sdk=%d)",
             g_sfHandle, g_sdkLevel);
    return true;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_binderMmap != MAP_FAILED) {
        munmap(g_binderMmap, kBinderMmapSize);
        g_binderMmap = MAP_FAILED;
    }
    if (g_binderFd >= 0) {
        close(g_binderFd);
        g_binderFd = -1;
    }
    g_state = BinderState::UNINITIALIZED;
    g_sfHandle = 0;
    g_cached = FrameStats{};
    g_lastRefreshMs = 0;
    g_lastFailMs = 0;
    g_failCount = 0;
}

bool isAlive() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_state == BinderState::READY && g_binderFd >= 0;
}

BinderState getState() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_state;
}

int getAndroidSdkLevel() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_sdkLevel;
}

bool refresh() {
    int fdDup = -1;
    uint32_t sfHandle;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto now = std::chrono::steady_clock::now();
        int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count();

        if (g_state == BinderState::FALLBACK_NEEDED) {
            if (g_lastFailMs > 0 &&
                (nowMs - g_lastFailMs) < kFallbackRetryBackoffMs) {
                return g_cached.valid;
            }
            g_state = BinderState::READY;
            g_failCount = 0;
            LOG_INFO("FRAME_BINDER: backoff elapsed, retrying direct path");
        }

        if (g_state != BinderState::READY || g_binderFd < 0) return false;

        if (g_lastRefreshMs > 0 && (nowMs - g_lastRefreshMs) < kCacheTtlMs) {
            return g_cached.valid;
        }

        fdDup = fcntl(g_binderFd, F_DUPFD_CLOEXEC, 0);
        if (fdDup < 0) {
            LOG_WARN("FRAME_BINDER: F_DUPFD_CLOEXEC failed: %s", aatpeErrStr(errno));
            return false;
        }
        sfHandle = g_sfHandle;
    }

    FrameStats stats = {};
    bool ok = fetchLatency(fdDup, sfHandle, {kLatencyArg}, stats);

    auto nowAfter = std::chrono::steady_clock::now();
    int64_t nowAfterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              nowAfter.time_since_epoch()).count();

    std::lock_guard<std::mutex> lk(g_mu);
    close(fdDup);
    if (g_state != BinderState::READY || g_binderFd < 0) return false;
    if (ok) {
        g_cached = stats;
        g_failCount = 0;
        g_lastFailMs = 0;
        LOG_INFO("FRAME_BINDER: refresh ok: avg=%.0fus p95=%.0fus p99=%.0fus jank=%lld n=%d",
                 stats.averageFrameTimeUs, stats.p95FrameTimeUs, stats.p99FrameTimeUs,
                 (long long)stats.jankCount, stats.sampleCount);
    } else {
        g_failCount++;
        g_lastFailMs = nowAfterMs;
        if (g_failCount >= kMaxFailures) {
            LOG_WARN("FRAME_BINDER: %d consecutive failures, marking Binder path dead",
                     g_failCount);
            close(g_binderFd);
            g_binderFd = -1;
            if (g_binderMmap != MAP_FAILED) {
                munmap(g_binderMmap, kBinderMmapSize);
                g_binderMmap = MAP_FAILED;
            }
            g_state = BinderState::DEAD;
        } else {
            g_state = BinderState::FALLBACK_NEEDED;
        }
    }
    g_lastRefreshMs = nowAfterMs;
    return ok;
}

FrameStats getStats() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cached;
}

bool refreshLayerLatency(const std::string& layerName, FrameStats& out) {
    if (layerName.empty()) return false;

    int fdDup = -1;
    uint32_t sfHandle;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_state != BinderState::READY || g_binderFd < 0) return false;
        fdDup = fcntl(g_binderFd, F_DUPFD_CLOEXEC, 0);
        if (fdDup < 0) return false;
        sfHandle = g_sfHandle;
    }

    FrameStats stats = {};
    bool ok = fetchLatency(fdDup, sfHandle, {kLatencyArg, layerName}, stats);

    {
        std::lock_guard<std::mutex> lk(g_mu);
        close(fdDup);
    }
    if (ok) out = stats;
    return ok;
}

}
}
