#include "aatped.h"
#include <cmath>
#include <chrono>
#include <mutex>

using namespace aatpe;

std::mutex g_abStatsMutex;
ABTestStats g_abStats;

namespace {
constexpr long long kMinExperimentMs = 600000;
#ifdef AATPE_HOST_TEST
long long (*s_abNowFn)() = nullptr;
#endif
long long abNowMs() {
#ifdef AATPE_HOST_TEST
    if (s_abNowFn) return s_abNowFn();
#endif
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

#ifdef AATPE_HOST_TEST
void abTestSetClockForTest(long long (*nowMsFn)()) { s_abNowFn = nowMsFn; }
#endif

static constexpr unsigned int kAbExperimentSalt = 0xA7E3C9D1u;

static int bucketIdentity(unsigned int id) {
    unsigned int h = id * 2654435761u + kAbExperimentSalt;
    h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16;
    return (int)(h % 2);
}

int abTestAssignByUid(int uid) {
    return bucketIdentity((unsigned int)(uid < 0 ? 0 : uid));
}

int abTestAssignThread(int tid) {
    return bucketIdentity((unsigned int)tid);
}

static double betaIncomplete(double x, double a, double b) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    double lbeta = lgamma(a) + lgamma(b) - lgamma(a + b);
    double front = exp(log(x) * a + log(1.0 - x) * b - lbeta) / a;
    double fpmin = 1e-30;
    double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0, d = 1.0 - qab * x / qap;
    if (fabs(d) < fpmin) d = fpmin;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= 200; m++) {
        int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < fpmin) d = fpmin;
        c = 1.0 + aa / c;
        if (fabs(c) < fpmin) c = fpmin;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < fpmin) d = fpmin;
        c = 1.0 + aa / c;
        if (fabs(c) < fpmin) c = fpmin;
        d = 1.0 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1.0) < 3e-7) break;
    }
    return front * h;
}

static double tDistCDF(double t, double df) {
    if (df <= 0.0) return 0.5;
    double x = df / (df + t * t);
    double ibeta = betaIncomplete(x, df / 2.0, 0.5);
    if (t > 0) return 1.0 - 0.5 * ibeta;
    return 0.5 * ibeta;
}

static double abTTest(const ABTestStats& a, const ABTestStats& b) {
    if (a.sampleCountA < 2 || b.sampleCountB < 2) return 1.0;
    double meanA = a.avgFrameTimeA;
    double meanB = b.avgFrameTimeB;
    double varA = (a.varianceA);
    double varB = (b.varianceB);
    if (varA < 0) varA = 0;
    if (varB < 0) varB = 0;
    double se = sqrt(varA / a.sampleCountA + varB / b.sampleCountB);
    if (se < 1e-9) return 1.0;
    double t = (meanA - meanB) / se;
    double df = (varA / a.sampleCountA + varB / b.sampleCountB) * (varA / a.sampleCountA + varB / b.sampleCountB)
              / ((varA / a.sampleCountA) * (varA / a.sampleCountA) / (a.sampleCountA - 1)
               + (varB / b.sampleCountB) * (varB / b.sampleCountB) / (b.sampleCountB - 1));
    if (df < 1.0) df = 1.0;
    double p = 2.0 * (1.0 - tDistCDF(fabs(t), df));
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    return p;
}

int abTestDetermineWinner(void) {
    std::lock_guard<std::mutex> lk(g_abStatsMutex);
    if (g_abStats.sampleCountA < 100 || g_abStats.sampleCountB < 100) return -1;
    if (g_abStats.startMonotonicMs == 0) return -1;
    long long elapsed = abNowMs() - g_abStats.startMonotonicMs;
    if (elapsed < kMinExperimentMs) return -1;
    double p = abTTest(g_abStats, g_abStats);
    if (p < 0.05) {
        return (g_abStats.avgFrameTimeA < g_abStats.avgFrameTimeB) ? 0 : 1;
    }
    return -1;
}

void abTestRecordFrameTime(int group, float frameTime) {
    std::lock_guard<std::mutex> lk(g_abStatsMutex);
    if (g_abStats.startMonotonicMs == 0) g_abStats.startMonotonicMs = abNowMs();
    if (group == 1) {
        g_abStats.decisionsA++;
        g_abStats.sampleCountA++;
        float oldMean = g_abStats.avgFrameTimeA;
        g_abStats.avgFrameTimeA += (frameTime - oldMean) / g_abStats.decisionsA;
        g_abStats.m2A += (double)(frameTime - oldMean) * (double)(frameTime - g_abStats.avgFrameTimeA);
        g_abStats.varianceA = (g_abStats.decisionsA > 1)
            ? (float)(g_abStats.m2A / (double)(g_abStats.decisionsA - 1)) : 0.0f;
    } else if (group == 2) {
        g_abStats.decisionsB++;
        g_abStats.sampleCountB++;
        float oldMean = g_abStats.avgFrameTimeB;
        g_abStats.avgFrameTimeB += (frameTime - oldMean) / g_abStats.decisionsB;
        g_abStats.m2B += (double)(frameTime - oldMean) * (double)(frameTime - g_abStats.avgFrameTimeB);
        g_abStats.varianceB = (g_abStats.decisionsB > 1)
            ? (float)(g_abStats.m2B / (double)(g_abStats.decisionsB - 1)) : 0.0f;
    }
}
void abTestRecordJank(int group) {
    std::lock_guard<std::mutex> lk(g_abStatsMutex);
    if (group == 1) g_abStats.jankCountA++;
    else if (group == 2) g_abStats.jankCountB++;
}
void abTestRecordMigration(int group) {
    std::lock_guard<std::mutex> lk(g_abStatsMutex);
    if (group == 1) g_abStats.migrationsA++;
    else if (group == 2) g_abStats.migrationsB++;
}

void abTestDiagSnapshot(float& avgA, float& avgB, double& pValue) {
    std::lock_guard<std::mutex> lk(g_abStatsMutex);
    avgA = g_abStats.avgFrameTimeA;
    avgB = g_abStats.avgFrameTimeB;
    pValue = abTTest(g_abStats, g_abStats);
}
