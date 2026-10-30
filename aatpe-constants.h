
#ifndef AATPE_CONSTANTS_H
#define AATPE_CONSTANTS_H

#include <cstdlib>
#include <cerrno>
#include <climits>
#include <string>

namespace aatpe {

inline int parseIntSafe(const char* s, int def = 0) {
    if (!s || !*s) return def;
    char* end = nullptr;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE || v < INT_MIN || v > INT_MAX) return def;
    return (int)v;
}
inline int parseIntSafe(const std::string& s, int def = 0) {
    return parseIntSafe(s.c_str(), def);
}

namespace latency {
    constexpr int kScoreUi          = 200;
    constexpr int kScoreRender      = 180;
    constexpr int kScoreGame        = 150;
    constexpr int kScoreThroughput  = 50;
    constexpr int kScoreSystem      = 0;
    constexpr int kScoreBackground  = 0;
}

namespace thermal {
    constexpr int kHighTempThreshold   = 70;
    constexpr int kPenaltyPerDegree    = 3;
    constexpr int kLowTempThreshold    = 50;
    constexpr int kBoostPerDegree      = 2;
    constexpr int kThrottleFreqPct     = 80;
    constexpr int kEmergencyHysteresis = 10;
}

namespace topology {
    constexpr int kLittleMaxRatio  = 70;
    constexpr int kMidMaxRatio     = 90;
    constexpr int kMaxCapacityRef  = 1024;
    constexpr int kDefaultCapacity = 380;
    constexpr int kMidFallbackMinLittle = 4;
    constexpr int kHexBitWidth     = 64;

    constexpr double kGapCapRatio   = 1.20;
    constexpr double kGapFreqRatio  = 1.15;
    constexpr double kPrimeMinFreqRatio = 1.15;
    constexpr double kPrimeMinCapRatio  = 1.20;
}

namespace thread_classify {
    constexpr int kThroughputAvgRunUs  = 5000;
    constexpr int kUiAvgRunUs          = 500;
    constexpr int kUiMinSwitches       = 100;
    constexpr int kGameAvgRunUs        = 2000;
    constexpr int kDefaultPriority     = 50;
}

namespace cache_score {
    constexpr int kAffinityHit = 5;
}

namespace power {
    constexpr int kBatteryBgScore        = 30;
    constexpr int kBatteryThroughputScore = -20;
    constexpr int kBatteryOtherScore      = -10;
    constexpr int kgamingInteractiveScore = 20;
    constexpr int kgamingBgScore          = -30;
    constexpr int kgamingOtherScore       = 0;
    constexpr int kThermalBgScore         = 40;
    constexpr int kThermalOtherScore      = -15;
}

namespace frame_score {
    constexpr int kCriticalBoost = 100;
}

namespace pool_score {

    constexpr int kUiPrime      = 48;
    constexpr int kUiBig        = 38;
    constexpr int kUiMid        = 32;
    constexpr int kUiLittle     = -20;
    constexpr int kRenderPrime  = 55;
    constexpr int kRenderBig   = 44;
    constexpr int kRenderMid   = 35;
    constexpr int kRenderLittle = -30;
    constexpr int kGamePrime   = 65;
    constexpr int kGameBig     = 55;
    constexpr int kGameMid     = 38;
    constexpr int kGameLittle  = -40;
    constexpr int kThroughputPrime   = 35;
    constexpr int kThroughputBig    = 28;
    constexpr int kThroughputMid    = 22;
    constexpr int kThroughputLittle = 5;
    constexpr int kBackgroundPrime  = -60;
    constexpr int kBackgroundLittle = 40;
    constexpr int kBackgroundMid    = -10;
    constexpr int kBackgroundBig    = -50;
}

namespace migration {
    constexpr int kCrossPoolPenalty = 7;
    constexpr int kSamePoolPenalty  = 3;
}

namespace spillover {
    constexpr float kUtilThresholdPassive = 85.0f;
    constexpr float kUtilThresholdAssist  = 75.0f;
    constexpr float kUtilThresholdActive  = 65.0f;
    constexpr float kPrimeHeadroom        = 55.0f;
    constexpr float kClusterPressureActive  = 75.0f;
    constexpr float kClusterPressureAssist = 80.0f;
    constexpr float kClusterPressurePassive = 85.0f;
    constexpr int kTopNPassive = 1;
    constexpr int kTopNAssist   = 2;
    constexpr int kTopNActive  = 3;
    constexpr long long kDwellMsPassive = 150;
    constexpr long long kDwellMsAssist  = 100;
    constexpr long long kDwellMsActive  = 60;
    constexpr int kStableCycles = 2;
    constexpr long long kInputBoostWindowMs = 80;
}

namespace contention {
    constexpr int kHighUtilThreshold   = 80;
    constexpr int kHighUtilPenalty     = -200;
    constexpr int kMidUtilThreshold    = 60;
    constexpr int kMidUtilPenalty      = -80;
    constexpr int kLowUtilThreshold    = 40;
    constexpr int kLowUtilPenalty      = -20;
    constexpr int kIdleBoost           = 40;
}

namespace capacity {
    constexpr int kBackgroundInverseRef = 1024;
}

namespace refresh {
    constexpr int kMinRate       = 30;
    constexpr int kMaxRate       = 240;
    constexpr int kDefaultRate   = 60;
    constexpr int kCurrentCacheTtl = 3;
    constexpr int kPeakCacheTtl    = 10;
    constexpr int kOneSecondNs     = 1000000000;
    constexpr int kOneSecondUs     = 1000000;
    constexpr int kMinVsyncPeriodNs = 1000000;
    constexpr int kMaxVsyncPeriodNs = 33333333;
    constexpr float kFrameCriticalMultiplier = 1.5f;
}

namespace sf_latency {
    constexpr int kMinUs   = 1000;
    constexpr int kMaxUs   = 100000;
    constexpr int kCacheTtl = 3;
    constexpr int kMaxMultiplier = 2;
}

namespace layer {
    constexpr int kCacheTtl = 2;
}

namespace foreground {
    constexpr int kCacheTtl = 3;
}

namespace cpu_cache {
    constexpr int kCacheTtlSec = 1;
}

namespace daemon {
    constexpr int kDecisionCleanupIntervalSec = 60;
    constexpr int kDecisionFileMaxAgeSec       = 300;
    constexpr int kHistoryDecayLoopCount       = 120;
    constexpr int kMinSleepMs                  = 20;
    constexpr int kHistoryScoreMinThreshold    = 5;
    constexpr int kHistoryEntryMaxAgeSec       = 3600;
    constexpr size_t kIdleTickMaxTrackedThreads = 10;
    constexpr int kIdleTickPollTimeoutMs       = 5000;
}

namespace logging {
    constexpr long long kMaxLogSizeBytes = 8LL * 1024LL * 1024LL;
    constexpr int kMaxLevel = 4;
    constexpr int kDefaultLevel = 2;
}

namespace role_names {
    constexpr int kRoleCount = 6;
    constexpr const char* kNames[kRoleCount] = {
        "S0_SYSTEM",
        "S1_UI",
        "S2_RENDER",
        "S3_GAME",
        "S4_THROUGHPUT",
        "S5_BACKGROUND"
    };
    constexpr const char* kUnknown = "S0_UNKNOWN";

    inline const char* getName(int role) {
        if (role >= 0 && role < kRoleCount) return kNames[role];
        return kUnknown;
    }
}

namespace cli_display {
    constexpr int kThreadsTruncate = 50;
    constexpr int kTopTruncate     = 30;
    constexpr int kBenchmarkMaxThreads = 100;
    constexpr int kLogChunkSize    = 4096;
    constexpr int kThermalCrit     = 90;
    constexpr int kThermalHot      = 80;
    constexpr int kThermalWarn     = 60;
    constexpr int kTempMilliScale  = 1000;
}

namespace probe {
    constexpr int kMaxAllowedFailures = 5;
    constexpr int kFullCompatMaxWarn  = 2;
    constexpr int kBasicCompatMaxFail  = 3;
}

namespace watchdog {
    constexpr int kDefaultIntervalSec    = 3;
    constexpr int kMinIntervalSec        = 1;
    constexpr int kDefaultMaxRestarts    = 5;
    constexpr int kDefaultRestartWindow  = 60;
    constexpr int kDefaultMemWarnKb      = 65536;
    constexpr int kDefaultMigrationLimit = 120;
    constexpr int kDefaultLoadLimit      = 95;
    constexpr int kRestartWaitSec        = 1;
    constexpr int kHeartbeatTimeoutSec   = 30;
    constexpr int kPassiveCooldownSec    = 300;
    constexpr int kConsecutiveThreshold  = 3;
    constexpr int kTempPassiveDurationSec = 10;
    constexpr int kRapidDeathThresholdSec = 10;
    constexpr int kMaxRapidCrashesBeforeDegrade = 2;
    constexpr int kDegradedCooldownSec = 120;
    constexpr int kRecoveryStableSec = 30;

    constexpr int kBootGraceSec = 120;
    constexpr int kBootGraceHeartbeatMultiplier = 3;

    inline bool heartbeatIsStale(long hbAgeSec, int timeoutSec) { return hbAgeSec >= timeoutSec; }
    inline bool shouldLatchPassive(int restartCount, int maxRestarts) { return restartCount > maxRestarts; }
    inline bool inPassiveCooldown(long passiveAgeSec, int cooldownSec) { return passiveAgeSec < cooldownSec; }
    inline bool memoryOverWarn(long memKb, int warnKb) { return memKb > warnKb; }
    inline bool migrationRateOver(long migPerSec, int limitPerSec) { return migPerSec > limitPerSec; }

    inline bool inBootGrace(long bootNowSec, int graceSec) { return bootNowSec < graceSec; }
    inline int effectiveHeartbeatTimeout(int baseTimeoutSec, bool inBootGrace) {
        return inBootGrace ? baseTimeoutSec * kBootGraceHeartbeatMultiplier : baseTimeoutSec;
    }
    inline bool countRapidCrash(bool inBootGrace) { return !inBootGrace; }
    inline bool shouldEscalateDegraded(bool inBootGrace, int rapidCount,
                                       int threshold, bool alreadyDegraded) {
        return !inBootGrace && !alreadyDegraded && rapidCount >= threshold;
    }
    inline bool degradedSelfHeal(bool healthyNow, long healthyStreakSec, int stableSec) {
        return healthyNow && healthyStreakSec >= stableSec;
    }
    inline bool highLoadShouldDegrade(bool jankAbnormal, long long dwellMs,
                                      long long dwellThresholdMs) {
        return jankAbnormal || dwellMs >= dwellThresholdMs;
    }
    inline bool shouldAdoptExistingDaemon(bool daemonAlive, bool heartbeatFresh,
                                          bool weStartedDaemon) {
        return daemonAlive && heartbeatFresh && !weStartedDaemon;
    }
}

namespace file_perm {
    constexpr int kDirMode  = 0700;
    constexpr int kFileMode = 0600;
}

namespace benchmark {
    constexpr int kMaxOps = 1000;
}

namespace paths {
    constexpr const char* kRunDir      = "/dev/aatpe";
    constexpr const char* kStateDir    = "/dev/aatpe/state";
    constexpr const char* kConfDir     = "/system/etc/aatpe";
    constexpr const char* kDecisionDir = "/dev/aatpe/decisions";
    constexpr const char* kLogFile     = "/dev/aatpe/aatpe.log";
    constexpr const char* kPidFile     = "/dev/aatpe/daemon.pid";
}

namespace decision_interval {
    constexpr int kHighLoadMs    = 50;
    constexpr int kMediumLoadMs  = 100;
    constexpr int kLowLoadMs     = 500;
    constexpr int kJankForcedMs  = 50;
}

namespace load_threshold {
    constexpr float kHighLoadPercent   = 80.0f;
    constexpr float kMediumLoadPercent = 40.0f;
}

namespace hmac {
    constexpr int kKeySizeBytes     = 128;
    constexpr int kMinKeyLenChars   = 64;
    constexpr int kRotateIntervalSec = 7 * 24 * 3600;
}

namespace thermal_const {
    constexpr int kCpuMaxThresholdC    = 120;
    constexpr int kSkinMaxThresholdC   = 45;
    constexpr int kBatteryMaxThresholdC = 50;
}

namespace log_const {
    constexpr long long kMaxLogSizeBytes = 8LL * 1024LL * 1024LL;
    constexpr int kLevelError = 0;
    constexpr int kLevelWarn  = 1;
    constexpr int kLevelInfo  = 2;
    constexpr int kLevelDebug = 3;
    constexpr int kLevelTrace = 4;
    constexpr int kDebugSamplePercent = 1;
}

namespace thermal_runtime {

    constexpr int kMaxTempJumpC = 45;
    constexpr int kMaxTempJumpGameC = 50;
    constexpr unsigned long long kCacheTtlMs = 1000;
    constexpr int kSpikeTriggerDeltaC = 2;
    constexpr int kEmergencyBootGraceSec = 20;
    constexpr int kEmergencyDebounceReads = 3;
    constexpr int kThermalProfileRestoreHysteresis = 5;

    constexpr int kHysteresisDeltaC = 4;
    constexpr int kMinHoldSec = 10;
    constexpr int kMinRecoverySec = 30;
}
namespace frame_latency {
    constexpr int kJankMaxMultiplierNum = 3;
    constexpr int kJankMaxMultiplierDen = 2;
    constexpr int kSfLatCacheTtlSec = 1;
    constexpr int kJankBoostConsecutive = 2;
}

namespace hmac_const {
    constexpr int kTimestampWindowSec = 30;
    constexpr int kTimestampSkewSec = 5;
    constexpr int kKeyLengthHex = 64;
    constexpr int kDaemonTokenLength = 64;
}

namespace hybrid_const {
    constexpr float kFineTuneWeight = 0.20f;
    constexpr float kPredictNormOffset = 1.0f;
    constexpr int kFeatureCount = 15;
    constexpr float kAvgRunUsScale = 50000.0f;
    constexpr float kNrSwitchesScale = 1000.0f;
    constexpr float kSwitchesPerSecScale = 300.0f;
}

namespace ebpf_ring_const {
    constexpr int kDefaultPageCount = 32;
    constexpr int kDefaultPageSize = 4096;
}

namespace fallback {
    constexpr long long kUeventActivateAfterMs = 5000;
    constexpr long long kUeventPollIntervalMs  = 500;

    constexpr int kTopologySysconfRetryEveryNLoops = 1;

}
}

#endif
