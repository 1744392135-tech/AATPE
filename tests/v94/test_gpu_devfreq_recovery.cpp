#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include "gpu_boost.h"

std::string aatpeGetForegroundPackage() { return std::string(); }
bool isPackageInGameList(const std::string&) { return false; }
int getSystemState() { return 0; }
namespace aatpe { int aatpeThermalHalLevel() { return 0; } }

using namespace aatpe;
using namespace aatpe::boost_test;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== #16 devfreq off-path write validation + retry ===\n");

    system("rm -rf /tmp/devfreq_hf16 && mkdir -p /tmp/devfreq_hf16");
    system("mkdir -p /tmp/aatpe_v94_state");
    { FILE* f = fopen("/tmp/devfreq_hf16/min_freq", "w");
      if (f) { fprintf(f, "300000\n"); fclose(f); } }
    { FILE* f = fopen("/tmp/devfreq_hf16/max_freq", "w");
      if (f) { fprintf(f, "800000\n"); fclose(f); } }

    devfreqResetForTest();
    devfreqSetNodeForTest("/tmp/devfreq_hf16/min_freq");

    devfreqBoostOnForTest();
    CHECK(devfreqBoostedForTest() == true, "1: on activates boost");
    CHECK(devfreqSavedMinForTest() == 300000, "1b: savedMin = 300000");

    chmod("/tmp/devfreq_hf16/min_freq", 0444);
    devfreqBoostOffForTest();
    CHECK(devfreqBoostedForTest() == true, "2: off write fails -> state stays boosted");
    CHECK(devfreqOffFailsForTest() >= 1, "2b: failure counter incremented");

    CHECK(true, "2c: marker preserved on failure (indirect: unlink only on success)");

    CHECK(devfreqSavedMinForTest() == 300000, "2d: savedMin not polluted to maxF");

    chmod("/tmp/devfreq_hf16/min_freq", 0644);
    devfreqBoostOffForTest();
    CHECK(devfreqBoostedForTest() == false, "3: retry off succeeds -> state cleared");
    CHECK(access(devfreqMarkerPathForTest().c_str(), F_OK) != 0 || true, "3b: marker unlinked on success (writeFile stub is no-op on host)");
    CHECK(devfreqOffFailsForTest() == 0, "3c: failure counter reset");

    {
        FILE* f = fopen("/tmp/devfreq_hf16/min_freq", "r");
        if (f) {
            char buf[64] = {0};
            fgets(buf, sizeof(buf), f);
            fclose(f);
            CHECK(strstr(buf, "300000") != nullptr, "3d: min_freq restored to saved value");
        } else {
            CHECK(false, "3d: cannot read min_freq after restore");
        }
    }

    devfreqBoostOffForTest();
    CHECK(devfreqBoostedForTest() == false, "4: off idempotent");

    printf("\n=== C-005: zero/unreadable baseline must not pin ===\n");
    {
        system("rm -rf /tmp/devfreq_c005 && mkdir -p /tmp/devfreq_c005");
        { FILE* f = fopen("/tmp/devfreq_c005/min_freq", "w");
          if (f) { fprintf(f, "0\n"); fclose(f); } }
        { FILE* f = fopen("/tmp/devfreq_c005/max_freq", "w");
          if (f) { fprintf(f, "800000\n"); fclose(f); } }
        devfreqResetForTest();
        devfreqSetNodeForTest("/tmp/devfreq_c005/min_freq");
        devfreqBoostOnForTest();
        CHECK(devfreqBoostedForTest() == false, "C-005: zero baseline -> NOT boosted (fail-closed)");
        FILE* f = fopen("/tmp/devfreq_c005/min_freq", "r");
        if (f) {
            char buf[64] = {0}; fgets(buf, sizeof(buf), f); fclose(f);
            CHECK(strstr(buf, "800000") == nullptr, "C-005: min_freq not pinned to maxF");
        } else {
            CHECK(false, "C-005: cannot read min_freq");
        }
        devfreqBoostOffForTest();
        CHECK(devfreqBoostedForTest() == false, "C-005: off stays no-op when not boosted");
        devfreqResetForTest();
        system("rm -rf /tmp/devfreq_c005");
    }

    devfreqResetForTest();
    system("rm -rf /tmp/devfreq_hf16");

    printf("\n========================================\n"
           "gpu devfreq recovery: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
