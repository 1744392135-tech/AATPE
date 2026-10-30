#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace aatpe {
namespace frame_binder {

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
    int64_t firstPresentNs = 0;
    int64_t lastPresentNs = 0;
    bool sampleCountTruncated = false;
};

enum class BinderState {
    UNINITIALIZED,
    READY,
    DEAD,
    FALLBACK_NEEDED
};

bool init();

void shutdown();

bool refresh();

FrameStats getStats();

bool isAlive();

BinderState getState();

int getAndroidSdkLevel();

bool refreshLayerLatency(const std::string& layerName, FrameStats& out);

#ifdef AATPE_HOST_TEST
std::string readPipeBlockingPublic(int fd, int timeoutMs);
#endif

}
}
