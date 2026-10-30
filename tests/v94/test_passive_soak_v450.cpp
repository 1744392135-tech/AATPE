#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <dirent.h>
#include <string>
#include "aatpe_ml.h"
#include "aatped_types.h"

using namespace aatpe_ml;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static ThreadFeatures makeFeeds(float runUs) {
    ThreadFeatures f;
    f.avg_run_us = runUs;
    f.nr_switches = 10.0f;
    f.cpu_usage = 0.4f;
    f.priority = 0.5f;
    f.nice = 0.0f;
    f.is_game = 1.0f;
    f.thermal = 0.2f;
    f.capacity = 0.7f;
    f.current_cpu_match = 1.0f;
    f.migration_count = 0.0f;
    f.cross_run_cap = 0.28f;
    f.cross_run_usage = 0.16f;
    f.cross_cap_thermal = 0.14f;
    f.cross_run_thermal = 0.08f;
    return f;
}

static long readRssKb() {
    FILE* fp = fopen("/proc/self/status", "r");
    if (!fp) return -1;
    long rss = -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "VmRSS: %ld kB", &rss) == 1) break;
    }
    fclose(fp);
    return rss;
}

static long countFds() {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    long n = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        n++;
    }
    closedir(d);
    return n;
}

static int countPending(LearningModel& m) {
    int n = 0;
    PendingDecision pd;
    while (m.getPendingDecision(n, pd)) n++;
    return n;
}

int main() {
    printf("=== PASSIVE-mode shadow boundedness soak (compressed fast-tick) ===\n");
    LearningModel m;
    m.setEnabled(true);
    m.setShadowSamplingEnabled(true);

    long long colBase = m.getShadowSamplesCollected();
    long long dropBase = m.getShadowSamplesDropped();
    int totalSamplesBefore = m.getTotalSamples();
    long rss0 = readRssKb();
    long fd0 = countFds();
    printf("baseline: collected=%lld dropped=%lld totalSamples=%d rss_kb=%ld fds=%ld\n",
           colBase, dropBase, totalSamplesBefore, rss0, fd0);
    printf("\n--- sample table: checkpoint, pushed, ring_size, collected, dropped, pending, rss_kb, fds ---\n");

    const long N = 200000;
    const long STEP = 5000;
    size_t maxRingSeen = 0;
    long long pushed = 0;

    for (long i = 0; i < N; i++) {
        m.recordShadowOutcome((int)(i % 3), makeFeeds(1000.0f + (float)(i % 512)),
                              0.5f + 0.01f * (float)(i % 10));
        pushed++;
        if ((i % 1000) == 0) {
            int tid = 900000 + (int)(i % 4000);
            m.recordPendingDecision(tid, 2, 2,
                                     makeFeeds(1500.0f), 1000ULL, 50ULL, 40);
            m.markPendingShadow(tid);
        }
        if ((i % STEP) == (STEP - 1)) {
            size_t ring = m.getShadowRingSize();
            long long col = m.getShadowSamplesCollected();
            long long drp = m.getShadowSamplesDropped();
            int pend = countPending(m);
            long rss = readRssKb();
            long fd = countFds();
            if (ring > maxRingSeen) maxRingSeen = ring;
            printf("  cp pushed=%6lld ring=%3zu collected=%lld dropped=%lld pending=%d rss_kb=%ld fds=%ld\n",
                   pushed, ring, col - colBase, drp - dropBase, pend, rss, fd);
        }
    }

    size_t ringAfter = m.getShadowRingSize();
    long long colAfter = m.getShadowSamplesCollected();
    for (long i = 0; i < 20000; i++) {
        m.recordShadowOutcome(0, makeFeeds(999.0f), 0.7f);
    }
    size_t ringAfter2 = m.getShadowRingSize();
    long long colAfter2 = m.getShadowSamplesCollected();

    long long dropBeforeC = m.getShadowSamplesDropped();
    size_t ringBeforeC = m.getShadowRingSize();
    long long colBeforeC = m.getShadowSamplesCollected();
    for (int i = 0; i < 1000; i++) {
        m.recordShadowOutcome(0, makeFeeds(1000.0f), -1.0f);
    }

    int pendBeforeD = countPending(m);
    int pendPeakD = 0;
    for (int i = 0; i < 15000; i++) {
        int tid = 5000000 + i;
        m.recordPendingDecision(tid, 1, 1,
                                makeFeeds(1200.0f), 1000ULL, 50ULL, 40);
        m.markPendingShadow(tid);
        if ((i % 2000) == 1999) {
            int p = countPending(m);
            if (p > pendPeakD) pendPeakD = p;
            printf("  pending-stress after %5d tids: pending=%d\n", i + 1, p);
        }
    }
    int pendAfterD = countPending(m);
    if (pendAfterD > pendPeakD) pendPeakD = pendAfterD;
    printf("  pending-stress: before=%d  after=%d  peak=%d\n", pendBeforeD, pendAfterD, pendPeakD);

    long rss1 = readRssKb();
    long fd1 = countFds();
    printf("\n--- end-of-soak measurements ---\n");
    printf("  rss start=%ld kB  end=%ld kB  delta=%ld kB\n", rss0, rss1, rss1 - rss0);
    printf("  fds start=%ld  end=%ld  delta=%ld\n", fd0, fd1, fd1 - fd0);

    printf("\n=== assertions ===\n");
    CHECK(maxRingSeen <= 512, "ring size never exceeded 512 across 220k pushes");
    CHECK(ringAfter == 512,  "ring sealed at exactly 512 after warmup (oldest evicted)");
    CHECK(ringAfter2 == 512, "ring still 512 after a further 20k pushes (no growth)");
    CHECK(colAfter2 > colAfter, "collected counter keeps climbing after ring capped");
    CHECK(m.getShadowSamplesDropped() - dropBeforeC == 1000, "invalid sentinels all counted as dropped");
    CHECK(m.getShadowRingSize() == ringBeforeC, "ring unchanged after invalid-sentinel burst");
    CHECK(m.getShadowSamplesCollected() == colBeforeC, "collected unchanged by invalid sentinels");

    CHECK(pendPeakD <= 1024, "pending decision count bounded <=1024 after 15k fresh tids");
    CHECK(pendAfterD == 1024, "pending sealed at exactly 1024 (hard top, oldest evicted)");

    CHECK(m.getTotalSamples() == totalSamplesBefore, "passive: formal total_samples never moved (no update())");
    CHECK(m.getAlpha() == 0.0f, "passive: alpha stays 0 (shadow never matures control)");
    CHECK(!g_mlDriftDetected.load(), "passive: no drift latch from shadow outcomes");

    long rssDelta = rss1 - rss0;
    CHECK(rssDelta < 4096, "RSS did not grow >4MB across the soak (flat / bounded)");
    CHECK(fd1 - fd0 <= 2, "fd count flat across soak (no fd leak)");

    printf("\n=== passive soak v450: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
