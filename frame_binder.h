// frame_binder.h - Binder direct SurfaceFlinger timestats frame rate capture
// Raw /dev/binder ioctl to connect to ISurfaceComposer and fetch
// frame latency statistics without forking dumpsys.
// Android 10 (API 29) through Android 14 (API 34).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace aatpe {
namespace frame_binder {

// Frame statistics snapshot from SurfaceFlinger --latency.
struct FrameStats {
    bool valid = false;
    double currentFrameTimeUs = -1;
    double averageFrameTimeUs = -1;
    double p90FrameTimeUs = -1;
    double p95FrameTimeUs = -1;
    double p99FrameTimeUs = -1;
    int64_t jankCount = 0;
    int sampleCount = 0;
    int64_t vsyncPeriodNs = 0;
    int64_t firstPresentNs = 0;   // P1 round7-rework: real present span for renderFps
    int64_t lastPresentNs = 0;
    bool sampleCountTruncated = false;   // response buffer likely cut the tail rows
};

// Connection state machine for diagnostics.
enum class BinderState {
    UNINITIALIZED,   // init() not yet called
    READY,           // connected, working
    DEAD,            // fatal error, do not retry
    FALLBACK_NEEDED  // transient failure, caller should fallback
};

// Open /dev/binder, negotiate protocol, enter looper, resolve SurfaceFlinger.
// Returns true on success; subsequent refresh()/getStats() will work.
bool init();

// Close /dev/binder, release all resources. Idempotent.
void shutdown();

// Force-refresh cached stats from SurfaceFlinger via Binder DUMP_TRANSACTION.
// Internal cache TTL is 500ms; calls within TTL return cached data.
// Returns true if fresh or cached data is available.
bool refresh();

// Return the last cached FrameStats. Does NOT trigger a Binder call.
FrameStats getStats();

// Check whether the Binder path is currently alive.
bool isAlive();

// Get current connection state (for diagnostics/logging).
BinderState getState();

// Get the detected Android SDK level (29..34+).
int getAndroidSdkLevel();

// WS-C2: one-shot DUMP_TRANSACTION for a *specific* layer's latency.
// Issues ["--latency", "<layerName>"] to SurfaceFlinger (no TTL cache, since
// the target layer changes per game). On success fills `out` and returns true.
// On any failure returns false; the caller falls back to `dumpsys SurfaceFlinger
// --latency "<layerName>"` via aatpeExec. Does NOT touch the cached g_cached
// general-latency stats used by refresh()/getStats().
bool refreshLayerLatency(const std::string& layerName, FrameStats& out);

#ifdef AATPE_HOST_TEST
// GPUTOP-05: host-test seam for readPipeBlocking total-cap behaviour.
std::string readPipeBlockingPublic(int fd, int timeoutMs);
#endif

} // namespace frame_binder
} // namespace aatpe
