// AATPE - A/B testing statistics sub-header
#ifndef AATPED_ABTEST_H
#define AATPED_ABTEST_H

// A/B testing framework
#include <mutex>
struct ABTestStats {
    long long decisionsA = 0;
    long long decisionsB = 0;
    float avgFrameTimeA = 0;
    float avgFrameTimeB = 0;
    float varianceA = 0;
    float varianceB = 0;
    int sampleCountA = 0;
    int sampleCountB = 0;
    int jankCountA = 0;
    int jankCountB = 0;
    int migrationsA = 0;
    int migrationsB = 0;
    int winner = 0; // 0=none, 1=A, 2=B
    bool active = false;
    // Welford online-variance accumulators (M2). Previously varianceA/B
    // were never written by recordFrameTime(), so Welch's SE was always 0 and
    // determineWinner() could never reach significance. These hold the running
    // sum-of-squared-deviations; varianceA/B is derived from them per-sample.
    double m2A = 0.0;
    double m2B = 0.0;
    // ML-04: experiment wall-clock start (steady monotonic ms), stamped on the
    // first recorded sample. determineWinner() gates on elapsed >= 10 min so a
    // burst of 100 frames inside one second of game-load cannot prematurely
    // crown a winner. 0 = "no sample recorded yet".
    long long startMonotonicMs = 0;
};

extern ABTestStats g_abStats;
extern std::mutex g_abStatsMutex;
int abTestAssignThread(int tid);
// G6-017: stable, restart-stable bucketing by Android uid (preferred).
int abTestAssignByUid(int uid);
void abTestRecordFrameTime(int group, float frameTime);
void abTestRecordJank(int group);
void abTestRecordMigration(int group);
int abTestDetermineWinner(void);

// ML-04: host-test seam — inject a fake monotonic clock (ms) so the 10-minute
// minimum-experiment-duration gate can be exercised hermetically. Release
// builds do not define AATPE_HOST_TEST, so this symbol never reaches the daemon.
#ifdef AATPE_HOST_TEST
void abTestSetClockForTest(long long (*nowMsFn)());
#endif

// LED-100: snapshot live AB averages + p-value (under g_abStatsMutex) for the
// diag/state.json globals g_abGroupAAvg/g_abGroupBAvg/g_abPValue.
void abTestDiagSnapshot(float& avgA, float& avgB, double& pValue);

#endif // AATPED_ABTEST_H
