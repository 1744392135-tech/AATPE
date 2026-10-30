#include <cstdio>
#include <fstream>
#include <string>
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

static const char* kNode = "/dev/stune/top-app/schedtune.boost";

static int readNode() {
    std::ifstream f(kNode);
    int v = -1;
    if (f) f >> v;
    return v;
}

int main() {
    printf("=== A2: 'placement' producer max aggregation ===\n");
    resetForTest();
    aatpeBoostSet("placement", 50);
    CHECK(readNode() == 50, "placement=50 -> node 50");

    aatpeBoostSet("touch", 80);
    CHECK(readNode() == 80, "touch=80 max -> node 80");
    aatpeBoostClear("touch");
    CHECK(readNode() == 50, "clear touch -> node back to placement=50");

    aatpeBoostSet("gpu", 60);
    CHECK(readNode() == 60, "gpu=60 max -> node 60");
    aatpeBoostClear("placement");
    CHECK(readNode() == 60, "clear placement -> gpu=60 remains");
    aatpeBoostClear("gpu");
    CHECK(readNode() == 0, "clear gpu -> node 0");

    resetForTest();
    aatpeBoostSet("touch", 80);
    CHECK(readNode() == 80, "default-off: touch-only path untouched");
    aatpeBoostClear("touch");
    CHECK(readNode() == 0, "default-off: clear touch -> 0");

    printf("\n=== RESULT: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
