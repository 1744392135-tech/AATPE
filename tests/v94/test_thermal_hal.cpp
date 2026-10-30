#include <cstdio>
#include <string>
#include <cassert>

bool atomicWriteFile(const char*, const std::string&) { return true; }

struct ThermalHalInfo { int level; int skinC; int headroom; };
extern ThermalHalInfo parseThermalServiceDump(const std::string& dump);

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
                              else { g_fail++; printf("  [FAIL] %s\n", msg); } } while(0)

int main() {
    printf("=== Thermal HAL parser fixture tests ===\n");

    std::string f1 =
        "Temperature{mValue=32.1, mType=SKIN, mName=skin, mStatus=0, mThrottlingEnabled=false}\n"
        "Temperature{mValue=55.0, mType=CPU, mName=cpu, mStatus=0}\n"
        "Thresholds: NONE LIGHT MODERATE SEVERE CRITICAL EMERGENCY SHUTDOWN\n";
    ThermalHalInfo r1 = parseThermalServiceDump(f1);
    CHECK(r1.level == 0, "F1 NONE level=0 (threshold table noise ignored)");
    CHECK(r1.skinC == 32, "F1 skin=32C");

    std::string f2 =
        "Temperature{mValue=65.0, mType=SKIN, mName=skin, mStatus=3}\n"
        "Temperature{mValue=85.0, mType=CPU, mName=cpu, mStatus=3}\n"
        "Temperature{mValue=0.4, mType=HEADROOM, mName=headroom, mStatus=0}\n";
    ThermalHalInfo r2 = parseThermalServiceDump(f2);
    CHECK(r2.level == 3, "F2 SEVERE level=3");
    CHECK(r2.headroom == 40, "F2 headroom=40%");

    ThermalHalInfo r3 = parseThermalServiceDump("");
    CHECK(r3.level == -1, "F3 empty -> -1 unavailable");

    printf("\nThermal HAL: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
