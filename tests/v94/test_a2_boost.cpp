// test_a2_boost.cpp — v4.2 Batch A2: the "placement" producer joins the schedtune
// single-writer bridge and participates in max() aggregation with touch/gpu.
// Links gpu_boost.cpp with -DAATPE_GPU_BOOST_TEST. Run under a mount namespace
// (see build_a.sh) where /dev/stune/top-app/schedtune.boost is a real writable
// file, so the bridge's real ::access probe adopts it and we can read it back.
#include <cstdio>
#include <fstream>
#include <string>
#include "gpu_boost.h"

std::string aatpeGetForegroundPackage() { return std::string(); }
bool isPackageInGameList(const std::string&) { return false; }
int getSystemState() { return 0; }  // NORMAL
namespace aatpe { int aatpeThermalHalLevel() { return 0; } }

using namespace aatpe;
using namespace aatpe::boost_test;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static const char* kNode = "/dev/stune/top-app/schedtune.boost";  // first candidate

static int readNode() {
    std::ifstream f(kNode);
    int v = -1;
    if (f) f >> v;
    return v;
}

int main() {
    printf("=== A2: 'placement' producer max aggregation ===\n");
    resetForTest();                       // clear node + next-probe throttle
    aatpeBoostSet("placement", 50);       // probes real access -> adopts kNode
    CHECK(readNode() == 50, "placement=50 -> node 50");

    aatpeBoostSet("touch", 80);           // max(80,50)=80
    CHECK(readNode() == 80, "touch=80 max -> node 80");
    aatpeBoostClear("touch");             // back to placement=50
    CHECK(readNode() == 50, "clear touch -> node back to placement=50");

    aatpeBoostSet("gpu", 60);             // max(50,60)=60
    CHECK(readNode() == 60, "gpu=60 max -> node 60");
    aatpeBoostClear("placement");         // gpu=60 remains (placement alone released)
    CHECK(readNode() == 60, "clear placement -> gpu=60 remains");
    aatpeBoostClear("gpu");                // all gone -> 0
    CHECK(readNode() == 0, "clear gpu -> node 0");

    // default-off: a fresh touch-only path is unchanged by the existence of the
    // "placement" producer (it is absent when we never Set it).
    resetForTest();
    aatpeBoostSet("touch", 80);
    CHECK(readNode() == 80, "default-off: touch-only path untouched");
    aatpeBoostClear("touch");
    CHECK(readNode() == 0, "default-off: clear touch -> 0");

    printf("\n=== RESULT: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
