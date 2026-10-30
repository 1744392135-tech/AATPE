#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <string>
#include <vector>
#include "aatpe_ml.h"

using namespace aatpe_ml;

static int g_pass = 0, g_fail = 0;

static std::string makeTempPath(const char* prefix) {
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/%s_%d_XXXXXX", prefix, (int)getpid());
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    return std::string(tmpl);
}
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static void backdateMtime(const char* path, time_t secondsAgo) {
    struct stat st;
    stat(path, &st);
    struct timeval tv[2];
    tv[0].tv_sec = st.st_atime;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = st.st_mtime - secondsAgo;
    tv[1].tv_usec = 0;
    utimes(path, tv);
}

int main() {
    printf("=== F-001: stale (>7d) model decays global fusion alpha ===\n");
    {
        LearningModel m; m.init(1); m.setEnabled(true);
        ThreadFeatures f = LearningModel::extractFeatures(
            0, 0, 50, 0, -20, false, 0, 512, true, 0);
        for (int i = 0; i < 700; i++) m.update(0, f, 0.8f);
        float alphaTrained = m.getAlpha();
        printf("  trained total_samples=%d alpha=%.4f\n",
               m.getTotalSamples(), (double)alphaTrained);
        CHECK(alphaTrained > 0.25f, "precondition: trained global alpha near max (>=0.25)");

        std::string p = makeTempPath("aatpe_f001");
        m.save(p.c_str());

        LearningModel mFresh; mFresh.init(1); mFresh.setEnabled(true);
        CHECK(mFresh.load(p.c_str()) == true, "F-001: fresh load ok");
        float alphaFresh = mFresh.getAlpha();
        printf("  fresh-load alpha=%.4f (total_samples=%d)\n",
               (double)alphaFresh, mFresh.getTotalSamples());
        CHECK(alphaFresh > 0.25f, "F-001: non-stale load keeps global alpha saturated");

        backdateMtime(p.c_str(), 30 * 24 * 3600);

        LearningModel mStale; mStale.init(1); mStale.setEnabled(true);
        CHECK(mStale.load(p.c_str()) == true, "F-001: stale load ok");
        float alphaStale = mStale.getAlpha();
        printf("  stale-load alpha=%.4f (total_samples=%d)\n",
               (double)alphaStale, mStale.getTotalSamples());
        CHECK(alphaStale < alphaFresh * 0.8f,
              "F-001: stale global alpha decayed (< 80% of fresh)");
        CHECK(alphaStale > 0.0f, "F-001: stale alpha not zero (partial decay, not reset)");

        int poolSamplesStale = mStale.getPoolModel(0).sample_count;
        printf("  stale pool[0].sample_count=%d (was 700)\n", poolSamplesStale);
        CHECK(poolSamplesStale < 700, "F-001: per-pool sample_count decayed too");

        unlink(p.c_str());
    }

    printf("=== F-003: v1 load clears m_anomalyLatched diagnostic latch ===\n");
    {
        std::string p = makeTempPath("aatpe_f003");
        FILE* fp = fopen(p.c_str(), "wb");
        char magic[8] = {'A','A','T','P','E','M','L','1'};
        fwrite(magic, 1, 8, fp);
        int num_cpus = 2;
        fwrite(&num_cpus, sizeof(int), 1, fp);
        int   ts = 200;
        float alpha = 0.2f, max_alpha = 0.3f, init_lr = 0.01f;
        float lr_decay = 0.999f, wclamp = 10.0f;
        int   cold = 50;
        fwrite(&ts, sizeof(int), 1, fp);
        fwrite(&alpha, sizeof(float), 1, fp);
        fwrite(&max_alpha, sizeof(float), 1, fp);
        fwrite(&init_lr, sizeof(float), 1, fp);
        fwrite(&lr_decay, sizeof(float), 1, fp);
        fwrite(&wclamp, sizeof(float), 1, fp);
        fwrite(&cold, sizeof(int), 1, fp);
        for (int c = 0; c < num_cpus; c++) {
            float w[11]; for (int i=0;i<11;i++) w[i] = 0.0f;
            fwrite(w, sizeof(float), 11, fp);
            float lr = init_lr;
            int sc = 100;
            fwrite(&lr, sizeof(float), 1, fp);
            fwrite(&sc, sizeof(int), 1, fp);
        }
        fclose(fp);

        LearningModel m; m.init(3); m.setEnabled(true);
        bool loaded = m.load(p.c_str());
        CHECK(loaded, "F-003: v1 crafted model loads ok");
        CHECK(m.isAnomalyLatched() == false,
              "F-003: v1 load leaves m_anomalyLatched=false (not latched)");
        CHECK(m.getAlpha() >= 0.0f, "F-003: v1 load produces a valid alpha");

        unlink(p.c_str());
    }

    printf("\n========================================\n");
    printf("F v4.3.4 ML results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}
