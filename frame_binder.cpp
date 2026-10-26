// frame_binder.cpp - Raw /dev/binder direct SurfaceFlinger latency capture
//
// Architecture:
//   1. Open /dev/binder, negotiate protocol version, enter looper.
//   2. Resolve "SurfaceFlinger" via servicemanager (handle=0, CHECK_SERVICE).
//   3. Issue DUMP_TRANSACTION on the SF handle with args ["--latency"].
//   4. Read dump output from a pipe (write-end passed as BINDER_TYPE_FD).
//   5. Parse three-column latency lines -> FrameStats.
//   6. Cache results under mutex; refresh() called every 500ms.
//
// Three-level fallback handled by caller (frame.cpp):
//   Binder (this module) -> sysfs DRM -> dumpsys SurfaceFlinger.

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

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char* kBinderDev       = "/dev/binder";
static const char* kSvcName         = "SurfaceFlinger";
static const char* kLatencyArg      = "--latency";

// IBinder DUMP_TRANSACTION = ('_'<<24)|('D'<<16)|('M'<<8)|'P' = 0x5F444D50
static constexpr uint32_t kDumpTransaction = 0x5F444D50u;

// servicemanager CHECK_SERVICE_TRANSACTION = 1
static constexpr uint32_t kSmCheckService = 1u;

// Cache TTL: refresh at most once per 500 ms.
static constexpr int64_t kCacheTtlMs = 500;

// Dump read timeout (ms).
static constexpr int kDumpReadTimeoutMs = 1500;

// Binder transaction timeout (ms). Non-blocking ioctl + poll() ensures
// the daemon main loop is NEVER blocked by a slow/non-responsive
// SurfaceFlinger or servicemanager. On timeout, the Binder path is
// marked dead and the caller falls back to dumpsys.
static constexpr int kBinderTxnTimeoutMs = 2000;

// Max consecutive failures before declaring path dead.
static constexpr int kMaxFailures = 3;

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static int                    g_binderFd     = -1;
// P1-1: user-space binder receive buffer. The kernel needs a mapped region to
// deliver BR_REPLY transaction_data into; without mmap() the direct path
// never receives a reply (the driver reports no mapped buffers for the
// process). 1 MiB is plenty for SurfaceFlinger --latency dumps.
static void*                  g_binderMmap  = MAP_FAILED;
static constexpr size_t      kBinderMmapSize = (1u << 20);
static uint32_t               g_sfHandle     = 0;
static BinderState            g_state        = BinderState::UNINITIALIZED;
static int                    g_sdkLevel     = 0;
static std::mutex             g_mu;
static FrameStats             g_cached;
static int64_t                g_lastRefreshMs = 0;
static int                    g_failCount    = 0;
// P2-3: backoff for FALLBACK_NEEDED. Once a transient failure lands us in
// FALLBACK_NEEDED, we do NOT immediately retry on the next refresh() tick
// (that would hammer a possibly-hung SurfaceFlinger). Instead we wait at least
// kFallbackRetryBackoffMs before re-attempting; a single jitter can no longer
// pin the process permanently onto the slow dumpsys fallback.
static int64_t                g_lastFailMs  = 0;
static constexpr int64_t      kFallbackRetryBackoffMs = 30000;

// ---------------------------------------------------------------------------
// Write a String16 (UTF-16 in native byte order) into a Parcel data buffer.
// Format: int32 charLen + char16_t[charLen] + char16_t(0)
// Returns total bytes written, or 0 on overflow (bufLen too small).
// P2-6: caller must pass bufLen so a future longer string cannot run off the
// end of a fixed-size stack parcel.
// ---------------------------------------------------------------------------
// LED-056 (R4-012): writeString16 encodes the service name as UTF-16LE on
// little-endian hosts (ARM/x86), which matches Android Binder Parcel's native
// byte order — there is no protocol-level BE requirement. SurfaceFlinger's
// service name is pure ASCII ("SurfaceFlinger", "--latency"), so the low-byte
// first / high-byte zero encoding below is correct. We explicitly reject
// non-ASCII (high-bit set) bytes instead of silently emitting mojibake, so a
// future service name with non-ASCII characters fails loudly rather than
// sending garbage to SF.
// ---------------------------------------------------------------------------
static size_t writeString16(uint8_t* buf, size_t bufLen, const char* utf8) {
    size_t len = strlen(utf8);
    size_t needed = sizeof(int32_t) + len * 2 + 2;  // len(32) + chars + NUL
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
        buf[off++] = 0;  // high byte = 0 for BMP ASCII
    }
    // Null terminator (UTF-16)
    buf[off++] = 0;
    buf[off++] = 0;
    return off;
}

// ---------------------------------------------------------------------------
// Parse BR_* commands from the binder read buffer.
//
// readBuf: caller-provided read buffer
// readBufSize: bytes actually consumed by the kernel in this batch
// outReply: receives the transaction_data if BR_REPLY/BR_TRANSACTION found
//
// REPLY -> a synchronous reply (or an incoming transaction) was delivered;
//          outReply is filled. The transaction is COMPLETE.
// MORE  -> the batch only carried BR_NOOP / BR_TRANSACTION_COMPLETE / BR_OK.
//          For a still-pending synchronous transaction this is NOT the reply:
//          BR_TRANSACTION_COMPLETE is just the driver's "your BC_TRANSACTION
//          was handed to the target" ack, and BR_OK means "no more commands
//          queued right now". Caller must keep waiting for BR_REPLY.
// ERROR -> BR_DEAD_REPLY / BR_FAILED_REPLY / BR_ERROR / malformed buffer.
// ---------------------------------------------------------------------------
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
            // Transaction sent; reply may come in a later read. NOT terminal.
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
            // "No more commands queued right now" — on a pending synchronous
            // transaction the BR_REPLY has NOT arrived yet.
            return ReadBatch::MORE;
        default:
            LOG_WARN("FRAME_BINDER: unknown BR cmd=0x%08x", cmd);
            return ReadBatch::ERROR;
        }
    }
    return ReadBatch::MORE;  // batch drained without a terminal command
}

// ---------------------------------------------------------------------------
// Send a synchronous BC_TRANSACTION and wait for BR_REPLY.
// txn: fully filled binder_transaction_data (handle, code, data sizes).
// outReply: receives the reply transaction_data on success.
// Returns 0 on success, -1 on failure.
// ---------------------------------------------------------------------------
// Wait for the Binder fd to become readable (reply available) with a
// hard timeout. Returns 0 on readable, -1 on error, -2 on timeout.
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

// ---------------------------------------------------------------------------
// Pure decision: what did this BINDER_WRITE_READ ioctl mean for the write we
// are trying to deliver? Extracted so the host unit test can drive the exact
// EAGAIN state machine without a /dev/binder.
//
//   TXN_SENT      - ioctl succeeded, OR EAGAIN/EWOULDBLOCK with the driver
//                   having consumed the WHOLE write buffer (write_consumed ==
//                   write_size). The transaction IS on the wire: EAGAIN came
//                   from the read side (no BR_REPLY queued yet). NEVER resend.
//   WRITE_PENDING - EAGAIN with the write NOT fully consumed yet (extremely
//                   rare on 4.14). Advance write_buffer by write_consumed and
//                   retry. binder has no POLLOUT, so no poll-wait is valid.
//   FATAL         - any other ioctl error (EPERM/EIO/ENOMEM/...).
// ---------------------------------------------------------------------------
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
    // Build the write command: BC_TRANSACTION + transaction_data
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

    // ------------------------------------------------------------------
    // Phase 1: hand the BC_TRANSACTION to the driver.
    //
    // REAL-DEVICE ROOT CAUSE (rel11, chopin / MTK 1100 / kernel 4.14.186,
    // binder protocol v8): binder_ioctl_write_read() processes the write
    // buffer FIRST, then calls binder_thread_read(). On an O_NONBLOCK fd, when
    // no BR_REPLY has been queued yet the read side returns -EAGAIN — EVEN IF
    // the whole write buffer was already consumed. binder_poll() only ever
    // raises EPOLLIN (when the thread has work); it has NO EPOLLOUT bit.
    //
    // The old code treated every EAGAIN as "write buffer full", then polled
    // POLLOUT — which on a binder fd can never fire — and timed out after 2s,
    // ALSO having re-issued the same BC_TRANSACTION on retry (duplicate sync
    // transaction). Net effect: servicemanager checkService always failed,
    // every ~30s, forever.
    //
    // Correct contract (mirrors IPCThreadState::talkWithDriver):
    //   * bwr.write_consumed == bwr.write_size  => transaction IS on the wire;
    //     NEVER resend it. The EAGAIN came from the read side.
    //   * afterwards we wait for POLLIN (reply queued) and read-only drain.
    // ------------------------------------------------------------------
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
            txnSent = true;   // clean return, or EAGAIN-after-full-consumption
            break;
        }
        // WRITE_PENDING: EAGAIN and the driver did NOT consume the whole command
        // yet. Advance the buffer past consumed bytes and retry. binder never
        // signals POLLOUT, so we cannot poll-wait; the next ioctl is the retry.
        bwr.write_buffer += bwr.write_consumed;
        bwr.write_size   -= bwr.write_consumed;
        if (std::chrono::steady_clock::now() >= deadline) {
            LOG_WARN("FRAME_BINDER: txn write pump deadline exceeded");
            return -1;
        }
        usleep(2000);
    }

    // ------------------------------------------------------------------
    // Phase 2: drain BR_* until BR_REPLY. Poll POLLIN (NOT POLLOUT) while
    // waiting — binder_poll() raises EPOLLIN exactly when the target queued
    // our reply. Read-only drains carry write_size=0 so the transaction can
    // never be re-sent.
    // ------------------------------------------------------------------
    struct binder_transaction_data reply = {};
    while (true) {
        ReadBatch rb = binderParseReadBatch(readBuf, bwr.read_consumed, &reply);
        if (rb == ReadBatch::REPLY) {
            if (outReply) *outReply = reply;
            return 0;
        }
        if (rb == ReadBatch::ERROR) return -1;

        // MORE: no reply in this batch. Wait for reply work with the remaining
        // deadline budget.
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // spurious wakeup
            LOG_WARN("FRAME_BINDER: BINDER_WRITE_READ (reply drain) failed: %s",
                     aatpeErrStr(errno));
            return -1;
        }
        bwr = rbwr;
    }
}

// ---------------------------------------------------------------------------
// Look up "SurfaceFlinger" from servicemanager (handle=0).
// Returns remote binder handle on success, 0 on failure.
// ---------------------------------------------------------------------------
static uint32_t lookupSurfaceFlinger(int fd) {
    // Build parcel data: String16("SurfaceFlinger")
    uint8_t parcel[128];
    memset(parcel, 0, sizeof(parcel));
    size_t parcelLen = writeString16(parcel, sizeof(parcel), kSvcName);
    if (parcelLen == 0) {
        LOG_WARN("FRAME_BINDER: service name too long for parcel");
        return 0;
    }

    struct binder_transaction_data txn = {};
    txn.target.handle  = 0;  // servicemanager
    txn.code           = kSmCheckService;
    txn.flags          = 0;  // synchronous
    txn.data_size      = parcelLen;
    txn.offsets_size   = 0;
    txn.data.ptr.buffer  = reinterpret_cast<binder_uintptr_t>(parcel);
    txn.data.ptr.offsets = 0;

    struct binder_transaction_data reply = {};
    if (sendTransaction(fd, &txn, &reply) != 0) {
        LOG_WARN("FRAME_BINDER: servicemanager checkService failed");
        return 0;
    }

    // Reply should contain a flat_binder_object of type BINDER_TYPE_HANDLE.
    // S-008: validate BOTH the size AND that the reply data buffer pointer is
    // non-null before reinterpret_casting it (a zero-length / error reply can
    // leave data.ptr.buffer == 0, which would dereference NULL below).
    // P3-27: with the P1-1 mmap() the kernel now delivers replies into our
    // page-aligned 1 MiB binder receive buffer, so the flat_binder_object*
    // cast below is naturally 8-byte aligned. Without mmap() the reply buffer
    // pointed into a small stack readBuf and could cause an unaligned trap on
    // strict-alignment kernels.
    if (reply.data_size < sizeof(flat_binder_object) || reply.data.ptr.buffer == 0) {
        LOG_WARN("FRAME_BINDER: SM reply invalid (size=%zu buffer=%llx, expect size>=%zu && buffer!=0)",
                 (size_t)reply.data_size, (unsigned long long)reply.data.ptr.buffer,
                 sizeof(flat_binder_object));
        return 0;
    }
    // Validate the buffer pointer lies within our mmap'd receive region.
    // The kernel delivers reply data into the mmap area; a pointer outside that
    // range would indicate a malformed transaction and must not be dereferenced.
    // GPUTOP-08: guard against integer wrap-around on a hostile/garbage bufPtr
    // (near 2^64): reject if bufPtr + data_size < bufPtr, or if the end exceeds
    // the mmap ceiling. All operations are unsigned so wrap wraps, not signs.
    uintptr_t bufPtr = static_cast<uintptr_t>(reply.data.ptr.buffer);
    uintptr_t mmapStart = reinterpret_cast<uintptr_t>(g_binderMmap);
    uintptr_t bufEnd = bufPtr + reply.data_size;
    uintptr_t mmapEnd = mmapStart + kBinderMmapSize;
    if (g_binderMmap == MAP_FAILED ||
        bufPtr < mmapStart ||
        bufEnd < bufPtr ||            // wrapped around 2^N
        mmapEnd < mmapStart ||        // mmap ceiling itself wrapped (paranoia)
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

// ---------------------------------------------------------------------------
// Read all available data from a non-blocking pipe until EOF or timeout.
// P3-14: hard 256 KiB cap — a runaway SurfaceFlinger dump cannot grow the
// string without bound (OOM on a low-RAM device).
// ---------------------------------------------------------------------------
static constexpr size_t kPipeReadMaxBytes = 256UL * 1024UL;
// GPUTOP-05: hard total wall-clock cap. The idle timeout alone lets a
// slowly-dripping SF (1 byte every 1499ms) loop forever. 5s is plenty for a
// --latency dump (real dumps finish in <100ms).
static constexpr int kPipeReadTotalMs = 5000;
// Read all available data from a non-blocking pipe until EOF or idle timeout.
// The timeout is an *idle* timeout: idleMs resets to 0 on every successful read.
// A separate total wall-clock cap (kPipeReadTotalMs) bounds a slowly-dripping
// source so the daemon is never blocked indefinitely.
static std::string readPipeBlocking(int fd, int timeoutMs) {
    std::string out;
    char buf[4096];
    int idleMs = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (idleMs < timeoutMs) {
        // GPUTOP-05: total wall-clock cap.
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
            break;  // EOF
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(5000);  // 5ms
                idleMs += 5;
            } else {
                break;
            }
        }
    }
    return out;
}

#ifdef AATPE_HOST_TEST
// GPUTOP-05: test seam so host tests can drive readPipeBlocking directly.
std::string readPipeBlockingPublic(int fd, int timeoutMs) {
    return readPipeBlocking(fd, timeoutMs);
}
#endif

// ---------------------------------------------------------------------------
// Parse SurfaceFlinger --latency output into FrameStats.
// AOSP canonical format:
//   line 1  : the refresh/vsync PERIOD in ns (a single number, e.g. 16666666).
//   rows    : one line per frame. Classic = 3 columns
//             (vsync_ns intended_present_ns actual_present_ns). Android 12 (API 31)
//             / some OEM kernels emit only 2 columns
//             (vsync_ns intended_present_ns) — no actual_present.
// C-008/C-009: the vsync period is taken from the FIRST line ONLY, matching
// frame.cpp parseSfLatency. The old code (a) left minIntervalNs=0 for the whole
// 2-column mode (the `continue` ran before any period extraction), and (b) on the
// 3-column rows misread v[2] as the period — but on real devices v[2] is the
// ~1e12 ns actual-present timestamp, which never falls in [5e6,1e8] ns, so the
// period was effectively always 0 -> hz=0 and jank detection disabled.
// ---------------------------------------------------------------------------
static void parseLatencyOutput(const std::string& text, FrameStats& stats) {
    std::vector<long long> frameDurationsUs;
    long long minIntervalNs = 0;
    // P1 round7-rework: track first/last PRESENT timestamp (intendedPresent col) to
    // derive real renderFps = (n-1)/(last-first). The ring holds "one line per
    // submitted frame", NOT 128 fixed vsync slots — a 60fps game on a 120Hz panel
    // must report ~60, not ~120.
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
            // First line = vsync period in ns (single number). Validate against
            // the same [5e6, 2e8] ns band parseSfLatency uses; an out-of-band /
            // missing first line means "unknown period" (jank stays disabled).
            long long period = 0;
            if (sscanf(s.c_str(), "%lld", &period) == 1 &&
                period >= 5000000LL && period <= 200000000LL) {
                minIntervalNs = period;
            }
            continue;   // the period line is NOT a frame row
        }

        long long v[3] = {0, 0, 0};
        int n = sscanf(s.c_str(), "%lld %lld %lld", &v[0], &v[1], &v[2]);
        // LED-045 (R4-001): accept BOTH the 2-column (Android 12) and the 3-column
        // classic row shape. Unknown column counts are skipped. The vsync period is
        // already taken from the first line above — do NOT reuse v[2] here.
        if (n != 2 && n != 3) continue;
        // Skip unused/invalid rows (-32768 markers), aligning with parseSfLatency.
        if (v[0] == -32768 || v[1] == -32768) continue;
        // Sanity gate (fuzz/R5 UB): a malformed/attacker-rolled dump can carry absurd
        // magnitudes or opposite-sign fields, under which (v[1]-v[0]) overflows signed
        // long long (UBSan). Real SF vsync/frame timestamps are positive nanoseconds
        // bounded by uptime, well under 2e12 ns (~23 days). Reject any row outside
        // [1, 2e12] ns BEFORE the subtraction — fail-closed, matching frame.cpp
        // parseSfLatency (frame.cpp:1101-1102). Both operands now in [1,2e12], so the
        // difference is bounded to ±2e12 and cannot overflow.
        const long long kTsMaxNs = 2000000000000LL;   // 2e12 ns
        if (v[0] <= 0 || v[1] <= 0 || v[0] > kTsMaxNs || v[1] > kTsMaxNs) continue;
        // F-05: this value is SUBMISSION LATENCY (intendedPresent - vsyncTime), NOT the
        // inter-present frame duration. It feeds averageFrameTimeUs/p95/p99, which are
        // used ONLY as latency/jank thresholds — never to reverse-compute FPS. The
        // measured render FPS is derived separately from the firstPresentNs/
        // lastPresentNs present-column span (see frame.cpp resolveGameFps), NOT from
        // this latency. Renaming the field to averageLatencyUs is the long-term fix;
        // it is kept under the legacy name to avoid rippling the ABI, with this comment
        // pinning the contract.
        long long latNs = v[1] - v[0];
        if (latNs > 0 && latNs < 200000000LL) {  // 0..200ms sanity
            frameDurationsUs.push_back(latNs / 1000);
            if (firstPresentNs == 0) firstPresentNs = v[1];
            lastPresentNs = v[1];
        }
    }

    if (frameDurationsUs.empty()) return;

    // Sort for percentiles.
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
    // LED-055 (R4-011): currentFrameTimeUs should reflect the MOST RECENT frame
    // (last sample in frameDurationsUs, which is appended in parse order), not
    // the sorted max (p100). The max is already available via p99 + the callers
    // that want worst-case; "current" means the latest observed frame.
    stats.currentFrameTimeUs = (double)frameDurationsUs.back();
    stats.averageFrameTimeUs = avg;
    stats.p90FrameTimeUs = pct(0.90);
    stats.p95FrameTimeUs = pct(0.95);
    stats.p99FrameTimeUs = pct(0.99);
    stats.sampleCount = (int)n;
    stats.vsyncPeriodNs = minIntervalNs;
    stats.firstPresentNs = firstPresentNs;
    stats.lastPresentNs = lastPresentNs;

    // Jank: frames exceeding 2x the minimum vsync period.
    if (minIntervalNs > 0) {
        double thresholdUs = (double)(minIntervalNs * 2) / 1000.0;
        int64_t jank = 0;
        for (auto v : sorted) {
            if ((double)v > thresholdUs) jank++;
        }
        stats.jankCount = jank;
    }
}

// ---------------------------------------------------------------------------
// Issue DUMP_TRANSACTION on the SurfaceFlinger handle with the given argv.
// Uses binder_fd_object for the pipe write-end (correct for kernel >= 4.14).
// WS-C2: argv is now a vector so callers can request either:
//   ["--latency"]                 -> general SF latency
//   ["--latency", "<layerName>"]  -> a specific game SurfaceView layer's latency
// ---------------------------------------------------------------------------
static bool fetchLatency(int fd, uint32_t sfHandle,
                         const std::vector<std::string>& argv, FrameStats& out) {
    // Create pipe for dump output.
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) {
        LOG_WARN("FRAME_BINDER: pipe2 failed: %s", aatpeErrStr(errno));
        return false;
    }

    // Build parcel data for DUMP_TRANSACTION.
    // Parcel layout (data buffer):
    //   offset 0:  binder_fd_object (24 bytes on 64-bit) - BINDER_TYPE_FD
    //   offset 24: int32 count = argv.size() (number of String16 args)
    //   offset 28+: String16(argv[0]) [String16(argv[1]) ...]
    // Offsets array: [0] = 0 (offset of fd object in data buffer)
    // T3 (P3): request buffer bumped 512 -> 8192. Layer names can be long
    // (SurfaceView[com.pkg/com.pkg.Activity]@0(BLAST)#132833); 512B risked
    // truncating the String16 layer-name arg.
    uint8_t dataBuf[8192];
    memset(dataBuf, 0, sizeof(dataBuf));

    // 1. binder_fd_object for the pipe write FD.
    struct binder_fd_object fdObj = {};
    fdObj.hdr.type = BINDER_TYPE_FD;
    fdObj.pad_flags = 0;
    fdObj.fd = pipefd[1];
    fdObj.cookie = 0;
    memcpy(dataBuf, &fdObj, sizeof(fdObj));

    // 2. String16Vector: count=N, then one String16 per argv entry.
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

    // Offsets array: one entry at data buffer offset 0.
    binder_size_t offsets[1] = { 0 };

    struct binder_transaction_data txn = {};
    txn.target.handle  = sfHandle;
    txn.code           = kDumpTransaction;
    txn.flags          = 0;  // synchronous
    txn.data_size      = off;
    txn.offsets_size   = sizeof(offsets);
    txn.data.ptr.buffer  = reinterpret_cast<binder_uintptr_t>(dataBuf);
    txn.data.ptr.offsets = reinterpret_cast<binder_uintptr_t>(offsets);

    // Send transaction. Reply data is not needed (dump writes to pipe FD).
    struct binder_transaction_data reply = {};
    int rc = sendTransaction(fd, &txn, &reply);

    // Close our write-end; read-end remains for reading.
    close(pipefd[1]);

    if (rc != 0) {
        LOG_WARN("FRAME_BINDER: DUMP_TRANSACTION to SurfaceFlinger failed");
        close(pipefd[0]);
        return false;
    }

    // Read dump output from pipe.
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

// ---------------------------------------------------------------------------
// Detect Android SDK level at init time.
// ---------------------------------------------------------------------------
static void detectSdkLevel() {
    g_sdkLevel = aatpeAndroidSdkInt();
    if (g_sdkLevel > 0) {
        LOG_INFO("FRAME_BINDER: Android SDK level = %d", g_sdkLevel);
    } else {
        LOG_WARN("FRAME_BINDER: could not detect Android SDK level, assuming generic");
    }
}

// ---------------------------------------------------------------------------
// Public API implementation
// ---------------------------------------------------------------------------

bool init() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_state == BinderState::READY) return true;
    if (g_state == BinderState::DEAD) return false;

    // LED-047 (R4-003): FALLBACK_NEEDED leaves g_binderFd >= 0 and
    // g_binderMmap != MAP_FAILED (refresh() failure path closes only on the
    // >=kMaxFailures DEAD transition). If init() is re-entered in that state,
    // the open()/mmap() below would overwrite the globals and leak the old fd
    // and 1 MiB mapping. Tear down any partial state before re-acquiring.
    if (g_binderFd >= 0) {
        close(g_binderFd);
        g_binderFd = -1;
    }
    if (g_binderMmap != MAP_FAILED) {
        munmap(g_binderMmap, kBinderMmapSize);
        g_binderMmap = MAP_FAILED;
    }

    detectSdkLevel();

    // O_NONBLOCK: all BINDER_WRITE_READ ioctls are non-blocking. Combined
    // with poll() in sendTransaction(), this guarantees the daemon main loop
    // can never be blocked by a slow SurfaceFlinger/servicemanager.
    g_binderFd = open(kBinderDev, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (g_binderFd < 0) {
        LOG_WARN("FRAME_BINDER: cannot open %s: %s", kBinderDev, aatpeErrStr(errno));
        g_state = BinderState::FALLBACK_NEEDED;
        return false;
    }

    // Negotiate protocol version.
    struct binder_version bv = {};
    if (ioctl(g_binderFd, BINDER_VERSION, &bv) < 0) {
        LOG_WARN("FRAME_BINDER: BINDER_VERSION ioctl failed: %s", aatpeErrStr(errno));
        close(g_binderFd);
        g_binderFd = -1;
        g_state = BinderState::FALLBACK_NEEDED;
        return false;
    }
    LOG_INFO("FRAME_BINDER: binder protocol version = %d", bv.protocol_version);

    // P1-1: mmap the user-space receive buffer. The Binder driver requires a
    // mapped region to deliver BR_REPLY transaction_data into; without this
    // the direct path sends BC_TRANSACTION but never receives a reply (the
    // kernel sees no mapped buffers for the process). MAP_PRIVATE | PROT_READ
    // is what libbinder uses for its receive buffer. On failure we cannot
    // make the direct path work — close the fd and fall back.
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

    // Set max threads (single-threaded client).
    uint32_t maxThreads = 1;
    if (ioctl(g_binderFd, BINDER_SET_MAX_THREADS, &maxThreads) < 0) {
        LOG_WARN("FRAME_BINDER: BINDER_SET_MAX_THREADS failed: %s", aatpeErrStr(errno));
    }

    // Enter looper state.
    uint32_t enterCmd = BC_ENTER_LOOPER;
    {
        struct binder_write_read bwr = {};
        bwr.write_size   = sizeof(enterCmd);
        bwr.write_buffer = reinterpret_cast<binder_uintptr_t>(&enterCmd);
        int ec = ioctl(g_binderFd, BINDER_WRITE_READ, &bwr);
        if (ec < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            bwr.write_consumed == sizeof(enterCmd)) {
            // Same contract as sendTransaction: EAGAIN on a non-blocking fd can
            // come purely from the read side (BR_OK / no pending work) while the
            // BC_ENTER_LOOPER command was already consumed. Treat as success.
            ec = 0;
        }
        if (ec < 0) {
            LOG_WARN("FRAME_BINDER: BC_ENTER_LOOPER failed: %s", aatpeErrStr(errno));
            // mmap succeeded above; release it on this failure path.
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

    // Look up SurfaceFlinger from servicemanager.
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
    // P3-15: the Binder IO (sendTransaction + pipe read, up to ~2s) MUST NOT
    // run while holding g_mu — getStats()/isAlive()/getState() would block on
    // a slow SurfaceFlinger. We take the lock only to snapshot the fd/handle/
    // state, release it, do the IO, then re-take the lock to publish.
    //
    // LED-046 (R4-002): the fd snapshot below is protected by dup()-ing it under
    // the lock. Without the dup, shutdown()/failure-close() could close() the
    // original fd while IO is in flight; the fd number could be reused by another
    // open() and our ioctl/read would target the wrong file. The dup'd fd has an
    // independent lifecycle and is closed when we return.
    int fdDup = -1;
    uint32_t sfHandle;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto now = std::chrono::steady_clock::now();
        int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count();

        // P2-3: if we are in FALLBACK_NEEDED, do not retry until the backoff
        // window has elapsed. A single transient glitch no longer pins the
        // path permanently onto dumpsys; we re-attempt after 30s.
        if (g_state == BinderState::FALLBACK_NEEDED) {
            if (g_lastFailMs > 0 &&
                (nowMs - g_lastFailMs) < kFallbackRetryBackoffMs) {
                // Still inside backoff window: report cached data (if any).
                return g_cached.valid;
            }
            // Backoff window elapsed: promote to READY attempt. We still have
            // the fd/handle; reset fail count and try again.
            g_state = BinderState::READY;
            g_failCount = 0;
            LOG_INFO("FRAME_BINDER: backoff elapsed, retrying direct path");
        }

        if (g_state != BinderState::READY || g_binderFd < 0) return false;

        // Cache TTL check (under lock, cheap).
        if (g_lastRefreshMs > 0 && (nowMs - g_lastRefreshMs) < kCacheTtlMs) {
            return g_cached.valid;
        }

        // LED-046: dup the fd so our IO uses an independent descriptor that
        // shutdown()/failure-close cannot race with.
        fdDup = fcntl(g_binderFd, F_DUPFD_CLOEXEC, 0);
        if (fdDup < 0) {
            LOG_WARN("FRAME_BINDER: F_DUPFD_CLOEXEC failed: %s", aatpeErrStr(errno));
            return false;
        }
        sfHandle = g_sfHandle;
    }

    // ---- IO here, lock NOT held (using fdDup, not g_binderFd) ----
    FrameStats stats = {};
    bool ok = fetchLatency(fdDup, sfHandle, {kLatencyArg}, stats);

    // LED-057 (R4-013): re-stamp the timestamp at publish time (after IO), so a
    // slow ~2s IO doesn't make the 500ms TTL window look expired when it isn't.
    auto nowAfter = std::chrono::steady_clock::now();
    int64_t nowAfterMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              nowAfter.time_since_epoch()).count();

    // ---- Publish under lock ----
    std::lock_guard<std::mutex> lk(g_mu);
    close(fdDup);
    // If shutdown() ran while we were doing IO, the fd is closed / state reset.
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

// ---------------------------------------------------------------------------
// WS-C2: one-shot per-layer latency fetch. Does NOT use the 500ms general
// cache (the target layer changes per game / frame window). Dups the fd under
// the lock, runs IO lock-free, then closes the dup. On any failure returns
// false and the caller falls back to aatpeExec("dumpsys SurfaceFlinger --latency
// \"<layerName>\"").
// ---------------------------------------------------------------------------
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

} // namespace frame_binder
} // namespace aatpe
