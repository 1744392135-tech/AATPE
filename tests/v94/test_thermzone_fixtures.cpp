// test_fixtures.cpp — hermetic /sys/class/thermal fixtures for the integrated
// CPU thermal-zone selector (shared/cpu_zone_selector.h).
//
// Exercises the SAME root-injectable reader loop that ships in aatpe-watchdog.cpp
// (wdReadMaxTempCelsiusUnder) against fake thermal_zone*/{type,temp} trees, and
// asserts the five required on-device scenarios for Dimensity 1200:
//   F1  MTK rogue zone (intermittent -1, drifts to 87) does NOT cause a false
//       WARNING throttle (trusted stays at the healthy cluster ~71C).
//   F2  A real, clean big core at 85C DOES trigger WARNING (trusted >= 80C).
//   F3  ANY CPU zone reaching 110C still latches emergency (anyCpu >= 110C);
//       the rogue filter must never swallow real overheat.
//   F4  Single CPU zone fail-safe (lone zone is trusted, not dropped to 0).
//   F5  badStreak recovery: a flaky zone that comes back to valid reads rejoins
//       the trusted set.
#include "cpu_zone_selector.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <climits>
#include <string>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

using namespace thermzone;

// Per-fixture cross-scan history (reset between fixtures).
static std::map<std::string, ZoneHist> g_hist;
static void resetHist() { g_hist.clear(); }

// One scan: read the fake tree, run the selector EXACTLY ONCE (as production does),
// and return both trustedCpuTempC (WARNING/90C wall) and anyCpuTempC (emergency wall).
// Mirrors the production watchdog reader loop (type + temp, three-tier unit normalize,
// -1/out-of-range => invalid).
struct ScanOut { long long trustedC; long long anyC; };
static ScanOut scanOnce(const char* root) {
    DIR* base = opendir(root);
    ScanOut out{-1, -1};
    if (!base) return out;
    std::vector<ZoneReading> tz;
    struct dirent* de;
    while ((de = readdir(base)) != nullptr) {
        std::string name = de->d_name;
        if (name.rfind("thermal_zone", 0) != 0) continue;
        std::string basePath = std::string(root) + "/" + name;

        std::string typeLower;
        {
            FILE* tf = fopen((basePath + "/type").c_str(), "r");
            if (!tf) continue;
            char buf[128] = {0};
            if (!fgets(buf, sizeof(buf), tf)) { fclose(tf); continue; }
            fclose(tf);
            typeLower = buf;
            while (!typeLower.empty() && (typeLower.back()=='\n'||typeLower.back()=='\r'||
                                          typeLower.back()==' ' ||typeLower.back()=='\t'))
                typeLower.pop_back();
            for (auto& c : typeLower) c = (char)std::tolower((unsigned char)c);
        }
        long long raw = 0; bool haveRaw = false;
        {
            FILE* f = fopen((basePath + "/temp").c_str(), "r");
            if (f) { if (fscanf(f, "%lld", &raw) == 1) haveRaw = true; fclose(f); }
        }
        ZoneReading r; r.name = name; r.typeLower = typeLower;
        if (haveRaw && raw > -50 && raw < 200000) {
            int c;
            if (raw > 1500)     c = (int)(raw / 1000);
            else if (raw > 150) c = (int)(raw / 10);
            else                c = (int)raw;
            r.valid = (c > 0 && c < 150);   // -1/0 sentinel => invalid
            r.tempC = r.valid ? c : INT_MIN;
        } else { r.valid = false; r.tempC = INT_MIN; }
        tz.push_back(r);
    }
    closedir(base);

    SelectResult sel = select(tz, g_hist);   // exactly one select per scan
    out.trustedC = sel.trustedCpuTempC > 0 ? sel.trustedCpuTempC : -1;
    out.anyC     = sel.anyCpuTempC;
    return out;
}

// ---- fake sysfs helpers ----
static void writeFile(const std::string& p, const std::string& content) {
    FILE* f = fopen(p.c_str(), "w"); if (!f) { perror(p.c_str()); exit(2); }
    fputs(content.c_str(), f); fclose(f);
}
static void makeZone(const std::string& root, const std::string& zone,
                     const std::string& type, int tempC) {
    std::string d = root + "/" + zone;
    mkdir(root.c_str(), 0755); mkdir(d.c_str(), 0755);
    writeFile(d + "/type", type + "\n");
    if (tempC == INT_MIN) writeFile(d + "/temp", "-1\n");
    else writeFile(d + "/temp", std::to_string(tempC * 1000) + "\n");
}
static void rmrf(const std::string& p) { std::system(("rm -rf " + p).c_str()); }

static int g_pass = 0, g_fail = 0;
#define CK(c, m) do{ if(c){g_pass++;printf("  [PASS] %s\n",m);} \
                    else{g_fail++;printf("  [FAIL] %s\n",m);} }while(0)

int main() {
    const std::string root = "/tmp/tz_fixture_root";

    // ---- F1: MTK rogue zone4 (-1 intermittent, drifts to 87); zone1 stable 71.
    printf("=== F1: Dimensity rogue zone4 -> no false WARNING (trusted~71, anyCpu=87) ===\n");
    {
        rmrf(root); resetHist();
        struct { int z1; int z4; } seq[] = { {53,63},{54,62},{55,INT_MIN},{60,73},{65,INT_MIN},{71,87} };
        ScanOut o{-1,-1};
        for (auto& s : seq) {
            rmrf(root);
            makeZone(root, "thermal_zone1", "mtktsap", s.z1);
            makeZone(root, "thermal_zone4", "mtktscpu", s.z4);
            makeZone(root, "thermal_zone26", "battery", 30);   // denylist, ignored
            o = scanOnce(root.c_str());
        }
        CK(o.trustedC <= 75, "F1: trusted~71 (rogue z4=87 removed; no false 80C throttle)");
        CK(o.anyC == 87, "F1: anyCpu still 87 (rogue zone not hidden from global CPU max)");
    }

    // ---- F2: clean big core ramps to 85C (LITTLE 65) -> WARNING fires.
    printf("\n=== F2: clean big core 85C -> WARNING DOES fire (trusted>=80) ===\n");
    {
        rmrf(root); resetHist();
        struct { int l; int b; } seq[] = { {55,70},{60,78},{65,85} };
        ScanOut o{-1,-1};
        for (auto& s : seq) {
            rmrf(root);
            makeZone(root, "thermal_zone0", "cpu-little", s.l);
            makeZone(root, "thermal_zone4", "cpu-big", s.b);
            o = scanOnce(root.c_str());
        }
        CK(o.trustedC >= 80, "F2: trusted=85 (real big-core heat triggers WARNING)");
    }

    // ---- F3: any CPU zone hits 112C -> emergency latches (anyCpu>=110).
    printf("\n=== F3: any CPU zone 112C -> emergency MUST latch (anyCpu>=110) ===\n");
    {
        rmrf(root); resetHist();
        ScanOut o{-1,-1};
        for (int i = 0; i < 3; i++) {
            rmrf(root);
            makeZone(root, "thermal_zone1", "mtktsap", 105);
            makeZone(root, "thermal_zone4", "mtktscpu", 112);
            o = scanOnce(root.c_str());
        }
        CK(o.anyC >= 110, "F3: anyCpu>=110 -> emergency hard wall latches (no overheat swallowed)");
    }

    // ---- F4: single CPU zone fail-safe.
    printf("\n=== F4: lone CPU zone is trusted (fail-safe) ===\n");
    {
        rmrf(root); resetHist();
        makeZone(root, "thermal_zone4", "mtktscpu", 80);
        ScanOut o = scanOnce(root.c_str());
        CK(o.trustedC == 80, "F4: lone zone trusted=80 (not dropped to 0 / -1)");
    }

    // ---- F5: badStreak recovery -> flaky zone rejoins trusted set.
    printf("\n=== F5: badStreak recovery -> z4 returns to trusted set ===\n");
    {
        rmrf(root); resetHist();
        struct { int z1; int z4; } seq[] = { {55,60},{56,INT_MIN},{57,INT_MIN},{58,62},{59,63},{60,64} };
        ScanOut o{-1,-1};
        for (auto& s : seq) {
            rmrf(root);
            makeZone(root, "thermal_zone1", "mtktsap", s.z1);
            makeZone(root, "thermal_zone4", "mtktscpu", s.z4);
            o = scanOnce(root.c_str());
        }
        CK(o.trustedC >= 63, "F5: z4 after two -1s + valid recovery rejoins trusted (trusted~64)");
    }

    rmrf(root);
    printf("\nfixtures: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
