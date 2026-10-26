#include "cpu_zone_selector.h"
#include <cstdio>
#include <map>
using namespace thermzone;

static int g_pass=0, g_fail=0;
#define CK(c,m) do{ if(c){g_pass++;printf("  [PASS] %s\n",m);} else {g_fail++;printf("  [FAIL] %s\n",m);} }while(0)

int main(){
    printf("=== T1: Dimensity rogue zone4 (real: -1 intermittent, drifts to 87; zone1 stable 71) ===\n");
    {
        std::map<std::string, ZoneHist> h;
        std::vector<std::vector<ZoneReading>> seq = {
            {{"z1","mtktsap",53,true},{"z4","mtktscpu",63,true}},
            {{"z1","mtktsap",54,true},{"z4","mtktscpu",62,true}},
            {{"z1","mtktsap",55,true},{"z4","mtktscpu",INT_MIN,false}}, // -1
            {{"z1","mtktsap",60,true},{"z4","mtktscpu",73,true}},
            {{"z1","mtktsap",65,true},{"z4","mtktscpu",INT_MIN,false}}, // -1 again
            {{"z1","mtktsap",71,true},{"z4","mtktscpu",87,true}},
        };
        int trustedEnd=0, anyEnd=0;
        for (auto& s : seq) { SelectResult r=select(s,h); trustedEnd=r.trustedCpuTempC; anyEnd=r.anyCpuTempC; }
        CK(trustedEnd <= 75, "T1: trusted~71 (rogue z4=87 removed, no false throttle)");
        CK(anyEnd == 87, "T1: anyCpu still sees 87 (emergency wall intact)");
    }

    printf("\n=== T2: real overheat -> emergency must fire ===\n");
    {
        std::map<std::string, ZoneHist> h;
        SelectResult r;
        for (int i=0;i<3;i++) r = select({{"z1","mtktsap",105,true},{"z4","mtktscpu",112,true}}, h);
        CK(r.anyCpuTempC >= kEmergencyWallC, "T2: anyCpu>=110 -> emergency latch");
        CK(r.trustedCpuTempC >= 105, "T2: trusted also high (big core not filtered)");
    }

    printf("\n=== T3: QCOM type prefixes ===\n");
    {
        std::map<std::string, ZoneHist> h;
        SelectResult r = select({{"z0","cpu-0",55,true},{"z1","cpu-1",56,true},
                                  {"z2","cpuss",58,true},{"z3","battery",30,true}}, h);
        CK(r.trustedCpuTempC == 58, "T3: QCOM cpu-0/1/cpuss max=58");
        CK(std::find(r.cpuNames.begin(),r.cpuNames.end(),"z3")==r.cpuNames.end(),"T3: battery excluded");
    }

    printf("\n=== T4: single CPU zone fail-safe ===\n");
    {
        std::map<std::string, ZoneHist> h;
        SelectResult r = select({{"z4","mtktscpu",80,true}}, h);
        CK(r.trustedCpuTempC == 80, "T4: lone CPU zone trusted");
    }

    printf("\n=== T5: big.LITTLE smooth — LITTLE65 / big85 -> trusted>=80 (WARNING, not rogue) ===\n");
    {
        std::map<std::string, ZoneHist> h;
        SelectResult r;
        // smooth load ramp: little 55->65, big 70->85, both clean
        std::vector<std::vector<ZoneReading>> seq = {
            {{"little","cpu-little",55,true},{"big","cpu-big",70,true}},
            {{"little","cpu-little",60,true},{"big","cpu-big",78,true}},
            {{"little","cpu-little",65,true},{"big","cpu-big",85,true}},
        };
        for (auto& s : seq) r = select(s, h);
        CK(r.trustedCpuTempC >= 80, "T5: trusted=85 (big core real heat triggers WARNING)");
        CK(std::find(r.rogueNames.begin(),r.rogueNames.end(),"big")==r.rogueNames.end(),
           "T5: big core 85 NOT judged rogue");
    }

    printf("\n=== T6: multi-zone consensus ramp 60->70->80->90, trusted follows high cluster ===\n");
    {
        std::map<std::string, ZoneHist> h;
        SelectResult r;
        for (int t=60;t<=90;t+=10)
            r = select({{"z0","cpu-0",t,true},{"z1","cpu-1",t+1,true},{"z2","cpuss",t+2,true}}, h);
        CK(r.trustedCpuTempC >= 88, "T6: trusted follows high cluster (~92)");
        CK(r.rogueNames.empty(), "T6: no zone rogue (clean consensus ramp)");
    }

    printf("\n=== T7: two -1s then recovery -> zone returns to trusted set ===\n");
    {
        std::map<std::string, ZoneHist> h;
        std::vector<std::vector<ZoneReading>> seq = {
            {{"z1","mtktsap",55,true},{"z4","mtktscpu",60,true}},
            {{"z1","mtktsap",56,true},{"z4","mtktscpu",INT_MIN,false}}, // -1
            {{"z1","mtktsap",57,true},{"z4","mtktscpu",INT_MIN,false}}, // -1 (badStreak=2 -> rogue)
            {{"z1","mtktsap",58,true},{"z4","mtktscpu",62,true}},      // valid recovery
            {{"z1","mtktsap",59,true},{"z4","mtktscpu",63,true}},      // valid (goodStreak decays badStreak)
            {{"z1","mtktsap",60,true},{"z4","mtktscpu",64,true}},
        };
        bool z4TrustedAtEnd=false;
        for (auto& s : seq) {
            SelectResult r=select(s,h);
            z4TrustedAtEnd = (std::find(r.rogueNames.begin(),r.rogueNames.end(),"z4")==r.rogueNames.end());
        }
        CK(z4TrustedAtEnd, "T7: z4 after two -1s + recovery reads returns to trusted");
    }

    printf("\n=== T8: persistently bad zone stays timeBad (not prematurely cleared) ===\n");
    {
        std::map<std::string, ZoneHist> h;
        SelectResult r;
        for (int i=0;i<6;i++)
            r = select({{"z1","mtktsap",60,true},{"z4","mtktscpu",INT_MIN,false}}, h);
        bool z4Rogue = (std::find(r.rogueNames.begin(),r.rogueNames.end(),"z4")!=r.rogueNames.end());
        CK(z4Rogue, "T8: z4 persistently -1 remains rogue (not washed out)");
        CK(r.trustedCpuTempC == 60, "T8: trusted uses healthy z1=60");
    }

    printf("\n=== T9 (D-005): lone-CPU-zone ramps must NOT pollute spikeStreak ===\n");
    {
        // A single-zone device ramps hard (50 -> 90 -> 95, each scan >25C slew).
        // Before the fix others==0 forced every such slew to be flagged an
        // "unfollowed spike" and spikeStreak climbed. Later a SECOND CPU zone
        // hot-plugs in cool (50C): the polluted z4 then looks timeBad AND
        // divergent from the new median (|95-50|=45 > 12) and gets judged ROGUE —
        // rejecting the REAL hot die. After the fix single-zone scans skip spike
        // detection entirely, so spikeStreak stays 0 and z4 survives.
        std::map<std::string, ZoneHist> h;
        SelectResult a = select({{"z4","mtktscpu",50,true}}, h);
        SelectResult b = select({{"z4","mtktscpu",90,true}}, h);
        SelectResult c = select({{"z4","mtktscpu",95,true}}, h);
        // Now a second CPU zone appears cool at 50C.
        SelectResult r = select({{"z4","mtktscpu",95,true},{"z1","mtktsap",50,true}}, h);
        bool z4Rogue = (std::find(r.rogueNames.begin(),r.rogueNames.end(),"z4")!=r.rogueNames.end());
        CK(!z4Rogue, "T9: z4 NOT rogue after 2nd zone hot-plugs (spikeStreak was not polluted)");
        CK(r.trustedCpuTempC == 95, "T9: trusted still follows the real hot die z4=95");
        (void)a;(void)b;(void)c;
    }

    printf("\nresults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail?1:0;
}
