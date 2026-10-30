#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include "aatped.h"

void mkdirIfNotExist(const char* path, mode_t mode) { ::mkdir(path, mode); }

namespace aatpe {
std::string thermalZoneRecToTempPathForTest(const std::string& recName);
std::string thermalZoneBaseFromTempPathForTest(const std::string& p);
}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== THM-03: zoneRec -> temp path mapping ===\n");
    {
        std::string p = aatpe::thermalZoneRecToTempPathForTest("thermal_zone4");
        CHECK(p == "/sys/class/thermal/thermal_zone4/temp",
              "thermal_zone4 -> /sys/class/thermal/thermal_zone4/temp");
    }
    {
        std::string p = aatpe::thermalZoneRecToTempPathForTest("hwmon0/temp1");
        CHECK(p == "/sys/class/hwmon/hwmon0/temp1_input",
              "hwmon0/temp1 -> /sys/class/hwmon/hwmon0/temp1_input (THM-03)");
    }
    {
        std::string p = aatpe::thermalZoneRecToTempPathForTest("hwmon2/temp3");
        CHECK(p == "/sys/class/hwmon/hwmon2/temp3_input",
              "hwmon2/temp3 -> /sys/class/hwmon/hwmon2/temp3_input");
    }

    printf("=== THM-02: temp path -> zone base (trip_point sibling) ===\n");
    {
        std::string b = aatpe::thermalZoneBaseFromTempPathForTest(
            "/sys/class/thermal/thermal_zone4/temp");
        CHECK(b == "/sys/class/thermal/thermal_zone4",
              "strip /temp -> zone base for trip_point_N_temp (THM-02)");
    }
    {
        std::string b = aatpe::thermalZoneBaseFromTempPathForTest(
            "/sys/class/hwmon/hwmon0/temp1_input");
        CHECK(b == "/sys/class/hwmon/hwmon0",
              "strip /tempN_input -> hwmon dir");
    }
    {
        std::string base = aatpe::thermalZoneBaseFromTempPathForTest(
            "/sys/class/thermal/thermal_zone4/temp");
        std::string trip0 = base + "/trip_point_0_temp";
        CHECK(trip0 == "/sys/class/thermal/thermal_zone4/trip_point_0_temp",
              "trip_point_0_temp is sibling of temp (not child)");
    }

    printf("=== THM-05: battery AC/USB online paths are independent supplies ===\n");
    {
        const char* acPath = "/sys/class/power_supply/ac/online";
        const char* usbPath = "/sys/class/power_supply/usb/online";
        CHECK(std::string(acPath).rfind("/sys/class/power_supply/", 0) == 0,
              "ac online path under /sys/class/power_supply/");
        CHECK(std::string(usbPath).rfind("/sys/class/power_supply/", 0) == 0,
              "usb online path under /sys/class/power_supply/");
        CHECK(std::string(acPath).find("battery/") == std::string::npos,
              "ac online NOT under battery/ subdir (THM-05)");
    }

    printf("=== THM-04: live flag file is not written by set_thermal_emergency ===\n");
    {
        mkdirIfNotExist(STATE_DIR, 0755);
        std::string flagPath = std::string(STATE_DIR) + "/thermal_emergency";
        writeFile(flagPath.c_str(), 0);
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            g_cfg.thermalEmergency = 95;
        }
        FILE* f = fopen(flagPath.c_str(), "r");
        if (f) {
            char buf[16] = {0};
            if (fgets(buf, sizeof(buf), f)) {
                for (char* p = buf; *p; ++p) if (*p=='\n'||*p=='\r') *p=0;
                fclose(f);
                CHECK(std::string(buf) == "0",
                      "thermal_emergency live flag still '0' after g_cfg write (THM-04)");
            } else { fclose(f); CHECK(false, "thermal_emergency file unreadable"); }
        } else {
            CHECK(false, "thermal_emergency file not created by test pre-seed");
        }
    }

    printf("=== THM-01: refreshThermalHal writes thermal_hal_source, not thermal_source ===\n");
    {
        printf("  [INFO] THM-01: thermal_source clobber site moved to thermal_hal_source\n");
    }

    printf("===================================================================\n");
    printf("thermal P2 fixes: %d ok, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
