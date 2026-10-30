#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include "aatpe_ml.h"
#include "aatped_types.h"

using namespace aatpe_ml;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s  (line %d)\n", m, __LINE__); } \
} while(0)

static std::string makeTempPath(const char* prefix) {
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/%s_%d_XXXXXX", prefix, (int)getpid());
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    return std::string(tmpl);
}

int main() {
    printf("=== #97: empty MID pool is NOT in the maturity denominator ===\n");
    {
        LearningModel m;
        m.init(4);
        m.setEnabled(true);

        ThreadFeatures f = LearningModel::extractFeatures(0, 0, 50, 0, -20, false, 0, 512, true, 0);
        for (int i = 0; i < 120; i++) {
            m.update(0, f, 0.7f);
            m.update(2, f, 0.6f);
            m.update(3, f, 0.8f);
        }

        std::vector<int> asg(8, -1);
        for (int c = 0; c <= 3; c++) asg[c] = 0;
        for (int c = 4; c <= 6; c++) asg[c] = 2;
        asg[7] = 3;
        m.setCpuPoolAssignment(asg);

        CHECK(m.isPoolPresent(0) == true, "#97: LITTLE slot0 present");
        CHECK(m.isPoolPresent(1) == false, "#97: MID slot1 ABSENT (no mapped cpu)");
        CHECK(m.isPoolPresent(2) == true, "#97: BIG slot2 present");
        CHECK(m.isPoolPresent(3) == true, "#97: PRIME slot3 present");

        std::string p = makeTempPath("aatpe_ml97");
        CHECK(m.save(p.c_str()) == true, "#97: save trained model ok");
        LearningModel m2;
        m2.init(4);
        m2.setEnabled(true);
        CHECK(m2.load(p.c_str()) == true, "#97: reload trained model ok");
        m2.setCpuPoolAssignment(asg);

        CHECK(m2.isMature() == true,
              "#97: mature despite empty MID pool (empty pool not in denominator)");

        std::vector<int> asgFull = asg;
        asgFull[5] = 1;
        m2.setCpuPoolAssignment(asgFull);
        CHECK(m2.isMature() == false,
              "#97: a real but untrained cluster STILL blocks maturity (gate intact)");

        unlink(p.c_str());
    }

    printf("=== #97: no assignment synced -> all slots present (safe default) ===\n");
    {
        LearningModel m;
        m.init(4);
        m.setEnabled(true);
        CHECK(m.isPoolPresent(0) == true, "#97: unsynced slot0 present by default");
        CHECK(m.isPoolPresent(1) == true, "#97: unsynced slot1 present by default");
    }

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
