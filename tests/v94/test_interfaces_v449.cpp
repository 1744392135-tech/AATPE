#include <cstdio>
#include <cstring>
#include "aatpe_interfaces.h"

bool aatpeThermalZoneLooksLikeCpuForTest(const char* type);

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while (0)

int main() {
    printf("=== F04: thermal zone classifier ===\n");
    CHECK(aatpeThermalZoneLooksLikeCpuForTest("cpu-0-0-usr"),  "cpu zone is CPU");
    CHECK(aatpeThermalZoneLooksLikeCpuForTest("tsens_tz_sensor0"), "tsens zone is CPU");
    CHECK(aatpeThermalZoneLooksLikeCpuForTest("soc"),         "soc zone is CPU");
    CHECK(aatpeThermalZoneLooksLikeCpuForTest("apc-tsens"),   "apc zone is CPU");
    CHECK(!aatpeThermalZoneLooksLikeCpuForTest("battery"),     "battery zone is NOT CPU");
    CHECK(!aatpeThermalZoneLooksLikeCpuForTest("battery_cell"),"battery_cell zone is NOT CPU");
    CHECK(!aatpeThermalZoneLooksLikeCpuForTest("skin"),       "skin zone is NOT CPU");
    CHECK(!aatpeThermalZoneLooksLikeCpuForTest("skin-1"),     "skin-1 zone is NOT CPU");
    CHECK(!aatpeThermalZoneLooksLikeCpuForTest("charger"),    "charger zone is NOT CPU");
    CHECK(!aatpeThermalZoneLooksLikeCpuForTest("gpu"),        "gpu zone is NOT CPU");
    CHECK(!aatpeThermalZoneLooksLikeCpuForTest("usb"),        "usb zone is NOT CPU");
    CHECK(!aatpeThermalZoneLooksLikeCpuForTest(""),           "empty type is NOT CPU");
    CHECK(!aatpeThermalZoneLooksLikeCpuForTest(nullptr),      "null type is NOT CPU");

    printf("=== F04: DefaultCpuMonitor on missing sysfs returns unknown ===\n");
    DefaultCpuMonitor m;
    int t = m.getTemperature();
    CHECK(t == 40, "missing-sysfs getTemperature() returns unknown sentinel 40");

    printf("=== RESULT: pass=%d fail=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
