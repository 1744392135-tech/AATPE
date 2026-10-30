#ifndef AATPED_ABTEST_H
#define AATPED_ABTEST_H

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
    int winner = 0;
    bool active = false;
    double m2A = 0.0;
    double m2B = 0.0;
    long long startMonotonicMs = 0;
};

extern ABTestStats g_abStats;
extern std::mutex g_abStatsMutex;
int abTestAssignThread(int tid);
int abTestAssignByUid(int uid);
void abTestRecordFrameTime(int group, float frameTime);
void abTestRecordJank(int group);
void abTestRecordMigration(int group);
int abTestDetermineWinner(void);

#ifdef AATPE_HOST_TEST
void abTestSetClockForTest(long long (*nowMsFn)());
#endif

void abTestDiagSnapshot(float& avgA, float& avgB, double& pValue);

#endif
