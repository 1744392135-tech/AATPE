#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <iterator>
#include <sstream>
#include "aatped.h"
#include "aatpe_diag.h"
std::string jsonEscape(const std::string& s);
const char* roleDisplayName(ThreadRole role);
using namespace aatpe;
static int g_pass=0,g_fail=0;
#define CHECK(c,m) do{ if(c){g_pass++;printf("  [PASS] %s\n",m);} else {g_fail++;printf("  [FAIL] %s\n",m);} }while(0)
int main(){
    static char tmp[]="/tmp/aatpe_webui_XXXXXX";
    char* d=mkdtemp(tmp);
    if(d){ extern const char* WEBUI_DIR; WEBUI_DIR=d; }

    printf("=== webui_json: jsonEscape ===\n");
    CHECK(jsonEscape("ab")=="ab", "plain passes through");
    CHECK(jsonEscape("a\"b")=="a\\\"b", "double-quote escaped");
    CHECK(jsonEscape("a\\b")=="a\\\\b", "backslash escaped");
    CHECK(jsonEscape("a\nb")=="a\\nb", "newline escaped");
    CHECK(jsonEscape("a\tb")=="a\\tb", "tab escaped");
    printf("=== webui_json: roleDisplayName ===\n");
    CHECK(std::string(roleDisplayName(ROLE_S0_SYSTEM))=="系统线程", "ROLE_S0 -> 系统线程");
    CHECK(std::string(roleDisplayName(ROLE_S1_UI))=="UI线程", "ROLE_S1 -> UI线程");
    CHECK(std::string(roleDisplayName(ROLE_S2_RENDER))=="渲染线程", "ROLE_S2 -> 渲染线程");
    CHECK(std::string(roleDisplayName(ROLE_S3_GAME))=="游戏线程", "ROLE_S3 -> 游戏线程");
    CHECK(std::string(roleDisplayName(ROLE_S4_THROUGHPUT))=="吞吐线程", "ROLE_S4 -> 吞吐线程");
    CHECK(std::string(roleDisplayName(ROLE_S5_BACKGROUND))=="后台线程", "ROLE_S5 -> 后台线程");
    CHECK(std::string(roleDisplayName((ThreadRole)99))=="未知", "unknown role -> 未知");
    printf("=== webui_json: recordWebuiDecision bounded ring ===\n");
    g_decisionRing.clear();
    for (int i=0;i<30;i++) recordWebuiDecision(1000+i,"t"+std::to_string(i),"游戏线程",0,2,80,90,"migrate");
    CHECK(g_decisionRing.size()<=21, "ring bounded (<=21 after 30 records)");
    CHECK(g_decisionRing.size()==20, "ring stabilizes at 20 entries");
    CHECK(g_decisionRing.front().tid==1010, "oldest evicted, first kept = 1010");
    CHECK(g_decisionRing.back().tid==1029, "latest record kept = 1029");
    printf("=== webui_json: writeDecisionsJson serialization ===\n");
    writeDecisionsJson();
    std::string path = std::string(WEBUI_DIR)+"/decisions.json";
    std::ifstream f(path);
    CHECK(f.good(), "decisions.json written to WEBUI_DIR");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content.find("\"decisions\":[")!=std::string::npos, "json has decisions array");
    CHECK(content.find("游戏线程")!=std::string::npos, "json contains role display");
    CHECK(content.find("\"tid\":1029")!=std::string::npos, "json contains latest tid");
    size_t p1 = content.find("\"tid\":1029"), p2 = content.find("\"tid\":1010");
    CHECK(p1 != std::string::npos && p2 != std::string::npos && p1 < p2, "newest decision serialized first (reversed ring)");
    printf("  [INFO] decisions.json (%zu bytes)\n", content.size());

    printf("=== ROUND-001: jsonEscape robustness on malformed input ===\n");
    {
        std::string bad1; bad1.push_back('\xE2'); bad1.push_back('\x80');
        std::string out1 = jsonEscape(bad1);
        CHECK(!out1.empty(), "truncated UTF-8 E2 80 does not crash");
        std::string u2028; u2028.push_back('\xE2'); u2028.push_back('\x80'); u2028.push_back('\xA8');
        std::string out2 = jsonEscape(u2028);
        CHECK(out2.find("\\u2028") != std::string::npos, "U+2028 escaped");
        std::string u2029; u2029.push_back('\xE2'); u2029.push_back('\x80'); u2029.push_back('\xA9');
        std::string out3 = jsonEscape(u2029);
        CHECK(out3.find("\\u2029") != std::string::npos, "U+2029 escaped");
        std::string ctrl; ctrl.push_back('\x01');
        std::string out4 = jsonEscape(ctrl);
        CHECK(out4.find("\\u0001") != std::string::npos, "0x01 escaped");
        std::string big(10000, 'a');
        std::string out5 = jsonEscape(big);
        CHECK(out5.size() >= 10000, "long string round-trips");
        std::string nul; nul.push_back('a'); nul.push_back('\0'); nul.push_back('b');
        std::string out6 = jsonEscape(nul);
        CHECK(out6.size() >= 3, "embedded NUL does not truncate");
    }

    printf("\n=== R007e: writeModelJson includes PRIME (cpu7) in model.json ===\n");
    {
        extern std::vector<int> g_littlePool, g_midPool, g_bigPool;
        extern std::vector<CpuCluster> g_clusters;
        g_littlePool = {0,1,2,3};
        g_midPool    = {};
        g_bigPool    = {4,5,6};
        g_clusters.clear();
        CpuCluster lc; lc.cpus={0,1,2,3}; lc.tier=CpuTier::LITTLE; g_clusters.push_back(lc);
        CpuCluster mc; mc.cpus={4,5,6}; mc.tier=CpuTier::MID;     g_clusters.push_back(mc);
        CpuCluster pc; pc.cpus={7};    pc.tier=CpuTier::PRIME;    g_clusters.push_back(pc);

        g_mlModel.init(4);
        g_mlModel.setEnabled(true);
        {
            std::vector<int> asg(8, -1);
            for (int c = 0; c <= 3; c++) asg[c] = 0;
            for (int c = 4; c <= 6; c++) asg[c] = 2;
            asg[7] = 3;
            g_mlModel.setCpuPoolAssignment(asg);
        }
        std::vector<float> w3(aatpe_ml::ML_WEIGHT_COUNT, 0.0f);
        w3[0] = 0.1234f;
        g_mlModel.setWeights(3, w3);
        writeModelJson();

        std::string path = std::string(WEBUI_DIR)+"/model.json";
        std::ifstream f(path);
        CHECK(f.good(), "model.json written");
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(content.find("\"name\":\"PRIME\"")!=std::string::npos, "pools array contains name=PRIME");
        CHECK(content.find("\"pool\":3")!=std::string::npos, "pools array contains pool slot 3");
        CHECK(content.find("\"cpu\":7")!=std::string::npos, "per-CPU cpus[] contains cpu7");
        CHECK(content.find("\"pool_count\":4")!=std::string::npos, "pool_count=4");
        CHECK(content.find("\"present\":true")!=std::string::npos, "#97: model.json emits present:true for real clusters");
        CHECK(content.find("\"present\":false")!=std::string::npos, "#97: model.json emits present:false for empty MID slot");
        {
            size_t p1 = content.find("\"pool\":1,\"name\":\"MID\"");
            CHECK(p1 != std::string::npos, "#97: pool:1 MID object present");
            if (p1 != std::string::npos) {
                size_t openBrace = content.rfind('{', p1);
                size_t closeBrace = content.find('}', p1);
                std::string slot1 = content.substr(openBrace, closeBrace - openBrace + 1);
                CHECK(slot1.find("\"present\":false") != std::string::npos,
                      "#97: MID slot1 present=false (empty cluster, not 'learning')");
                CHECK(slot1.find("\"present\":true") == std::string::npos,
                      "#97: MID slot1 is not accidentally marked present");
            }
        }
        printf("  [INFO] model.json (%zu bytes)\n", content.size());
    }

    printf("\n=== v4.2 Batch C: writeSystemJson v42 observability keys ===\n");
    {
        static char sTmp[]="/tmp/aatpe_webui_v42_XXXXXX";
        char* sd=mkdtemp(sTmp);
        if(sd){ extern const char* STATE_DIR; STATE_DIR=sd; }
        writeSystemJson();
        std::ifstream f0(std::string(WEBUI_DIR)+"/system.json");
        CHECK(f0.good(), "system.json written (empty STATE_DIR)");
        std::string c0((std::istreambuf_iterator<char>(f0)), std::istreambuf_iterator<char>());
        CHECK(c0.find("\"v42\":{")!=std::string::npos, "system.json has v42 block");
        CHECK(c0.find("\"cold_start_rejected\":0")!=std::string::npos, "cold_start_rejected defaults 0 when missing");
        CHECK(c0.find("\"ml_weight_degraded_count\":0")!=std::string::npos, "ml_weight_degraded_count defaults 0 when missing");
        CHECK(c0.find("\"sf_thread_count\":0")!=std::string::npos, "sf_thread_count defaults 0 when missing");
        CHECK(c0.find("\"schedtune_boost_current\":0")!=std::string::npos, "schedtune_boost_current defaults 0 when missing");
        CHECK(c0.find("\"autocal_ratio\":0.000")!=std::string::npos, "autocal_ratio defaults 0.000 when missing");
        CHECK(c0.find("\"thermal_headroom_pct\":0")!=std::string::npos, "thermal_headroom_pct defaults 0 when missing");

        auto w=[&](const char* name,const std::string& v){
            std::ofstream o(std::string(sd)+"/"+name); o<<v; o.close();
        };
        w("cold_start_rejected","5");
        w("ml_weight_degraded_count","3");
        w("sf_thread_count","7");
        w("schedtune_boost_current","60");
        w("autocal_ratio","870");
        w("thermal_headroom_pct","42");
        writeSystemJson();
        std::ifstream f1(std::string(WEBUI_DIR)+"/system.json");
        std::string c1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
        CHECK(c1.find("\"cold_start_rejected\":5")!=std::string::npos, "cold_start_rejected reads planted value 5");
        CHECK(c1.find("\"ml_weight_degraded_count\":3")!=std::string::npos, "ml_weight_degraded_count reads 3");
        CHECK(c1.find("\"sf_thread_count\":7")!=std::string::npos, "sf_thread_count reads 7");
        CHECK(c1.find("\"schedtune_boost_current\":60")!=std::string::npos, "schedtune_boost_current reads 60");
        CHECK(c1.find("\"autocal_ratio\":0.870")!=std::string::npos, "autocal_ratio restored 870 -> 0.870");
        CHECK(c1.find("\"thermal_headroom_pct\":42")!=std::string::npos, "thermal_headroom_pct reads 42");
        printf("  [INFO] system.json (%zu bytes)\n", c1.size());
    }

    printf("\n=== M1/S2: battery-unknown and no-sample MAE emit JSON null ===\n");
    {
        static char sTmp[]="/tmp/aatpe_webui_sentinel_XXXXXX";
        char* sd=mkdtemp(sTmp);
        if(sd){ extern const char* STATE_DIR; STATE_DIR=sd; }
        auto w=[&](const char* name,const std::string& v){
            std::ofstream o(std::string(sd)+"/"+name); o<<v; o.close();
        };
        writeSystemJson();
        std::ifstream f0(std::string(WEBUI_DIR)+"/system.json");
        std::string c0((std::istreambuf_iterator<char>(f0)), std::istreambuf_iterator<char>());
        CHECK(c0.find("\"battery\":null")!=std::string::npos,
              "M1: missing thermal_battery -> thermal.battery is JSON null (not 0)");
        CHECK(c0.find("\"prediction_mae\":null")!=std::string::npos,
              "S2: missing prediction_mae -> prediction_mae is JSON null (not 0.000000)");
        w("thermal_battery","35");
        w("prediction_mae","2.500000");
        writeSystemJson();
        std::ifstream f1(std::string(WEBUI_DIR)+"/system.json");
        std::string c1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
        CHECK(c1.find("\"battery\":35")!=std::string::npos,
              "M1: planted thermal_battery=35 -> thermal.battery is 35");
        CHECK(c1.find("\"prediction_mae\":\"2.500000\"")!=std::string::npos,
              "S2: planted prediction_mae=2.500000 -> quoted value preserved");
        printf("  [INFO] system.json (%zu bytes)\n", c1.size());
    }

    printf("\n=== v4.2.2 P3-1: PSI three-tier (avg10/60/300) in system.json ===\n");
    {
        extern aatpe::diag::SystemPressure g_stubPsi;
        using namespace aatpe::diag;
        g_stubPsi = SystemPressure{};
        g_stubPsi.psiAvailable = true;
        g_stubPsi.psiCpuSomePct = 5.5f;  g_stubPsi.psiCpuSome60Pct = 2.2f; g_stubPsi.psiCpuSome300Pct = 1.1f;
        g_stubPsi.psiCpuTotalUsec = 12345678ULL;
        g_stubPsi.psiIoSomePct = 3.3f;   g_stubPsi.psiIoSome60Pct = 0.0f; g_stubPsi.psiIoSome300Pct = 0.0f;
        g_stubPsi.psiMemSomePct = 0.0f;  g_stubPsi.psiMemSome60Pct = 0.0f; g_stubPsi.psiMemSome300Pct = 0.0f;
        writeSystemJson();
        std::ifstream f(std::string(WEBUI_DIR)+"/system.json");
        std::string c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(c.find("\"psi\":{\"available\":true")!=std::string::npos, "psi.available true");
        CHECK(c.find("\"some10\":5.500000")!=std::string::npos, "psi cpu some10=5.5");
        CHECK(c.find("\"some60\":2.200000")!=std::string::npos, "psi cpu some60=2.2");
        CHECK(c.find("\"some300\":1.100000")!=std::string::npos, "psi cpu some300=1.1");
        CHECK(c.find("\"total_usec\":12345678")!=std::string::npos, "psi cpu total_usec");
        CHECK(c.find("\"io\":{\"some10\":3.300000")!=std::string::npos, "psi io some10=3.3");
        CHECK(c.find("\"mem\":{\"some10\":0.000000")!=std::string::npos, "psi mem some10=0 (missing resource)");
        g_stubPsi = SystemPressure{}; g_stubPsi.psiAvailable = false;
        writeSystemJson();
        std::ifstream f2(std::string(WEBUI_DIR)+"/system.json");
        std::string c2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
        CHECK(c2.find("\"psi\":{\"available\":false")!=std::string::npos, "psi.available false when pressure missing");
        printf("  [INFO] psi block round-trip OK\n");
    }

    printf("\n=== W2: aatpe_buildFrameAppsJson serialization ===\n");
    {
        extern std::string aatpe_buildFrameAppsJson();
        extern std::vector<AatpePerAppFrameStat> g_stubFrameAppStats;
        extern std::string g_stubFrameAppCurrent;

        g_stubFrameAppStats.clear();
        g_stubFrameAppCurrent = "";
        std::string e0 = aatpe_buildFrameAppsJson();
        CHECK(e0 == "{\"current\":\"\",\"source\":\"\",\"reason\":\"\",\"apps\":[]}", "empty -> {\"current\":\"\",\"source\":\"\",\"reason\":\"\",\"apps\":[]}");

        AatpePerAppFrameStat a;
        a.pkg = "com.tencent.tmgp.sgame"; a.fps=59.5; a.ft_avg_ms=16.8; a.ft_p50=16.0;
        a.ft_p95=22.4; a.ft_p99=28.1; a.jank=3; a.dropped=12; a.samples=1024;
        a.active_ms=61234; a.last_update_ms=1700000000123LL;
        AatpePerAppFrameStat b;
        b.pkg = "com.miui.home"; b.fps=60.0; b.ft_avg_ms=16.6; b.ft_p50=16.6;
        b.ft_p95=17.0; b.ft_p99=17.2; b.jank=0; b.dropped=0; b.samples=500;
        b.active_ms=120000; b.last_update_ms=1700000000000LL;
        g_stubFrameAppStats = {a, b};
        g_stubFrameAppCurrent = "com.tencent.tmgp.sgame";
        std::string j = aatpe_buildFrameAppsJson();
        CHECK(j.find("\"current\":\"com.tencent.tmgp.sgame\"") != std::string::npos, "current pkg serialized");
        CHECK(j.find("\"pkg\":\"com.tencent.tmgp.sgame\"") < j.find("\"pkg\":\"com.miui.home\""), "MRU order preserved (sgame before home)");
        for (const char* field : {"\"pkg\"","\"fps\"","\"ft_avg_ms\"","\"ft_p50\"","\"ft_p95\"","\"ft_p99\"",
                                  "\"jank\"","\"dropped\"","\"samples\"","\"active_ms\"","\"last_update_ms\""}) {
            bool found = j.find(field) != std::string::npos;
            if (found) { g_pass++; printf("  [PASS] field present\n"); }
            else { g_fail++; printf("  [FAIL] field missing\n"); }
        }
        CHECK(j.find("\"jank\":3") != std::string::npos, "jank count serialized");
        CHECK(j.find("\"source\":\"") != std::string::npos, "v4.2.8 source field present");
        CHECK(j.find("\"reason\":\"") != std::string::npos, "v4.2.8 reason field present");
        printf("  [INFO] frame_apps json (%zu bytes)\n", j.size());
    }

    printf("\n=== v4.2.9: frame_apps target_fps missing -> null contract ===\n");
    {
        extern std::vector<AatpePerAppFrameStat> g_stubFrameAppStats;
        extern std::string g_stubFrameAppCurrent;
        extern std::string aatpe_buildFrameAppsJson();

        AatpePerAppFrameStat missing;
        missing.pkg = "com.missing.tfps"; missing.fps=0.0;
        AatpePerAppFrameStat known;
        known.pkg = "com.game.tfps"; known.fps=59.9; known.target_fps=90.0;

        g_stubFrameAppStats = {missing, known};
        g_stubFrameAppCurrent = "com.game.tfps";
        std::string j = aatpe_buildFrameAppsJson();

        size_t pMissing = j.find("\"pkg\":\"com.missing.tfps\"");
        size_t pKnown   = j.find("\"pkg\":\"com.game.tfps\"");
        CHECK(pMissing != std::string::npos, "missing-tfps app serialized");
        CHECK(pKnown   != std::string::npos, "known-tfps app serialized");

        size_t tm = j.find("\"target_fps\":", pMissing);
        bool nullOk = (tm != std::string::npos &&
                       j.compare(tm+13, 4, "null") == 0);
        CHECK(nullOk, "missing target_fps serializes as JSON null (not NaN/omitted)");

        size_t tk = j.find("\"target_fps\":", pKnown);
        bool numOk = (tk != std::string::npos &&
                      j.compare(tk+13, std::string("90.000000").size(), "90.000000") == 0);
        CHECK(numOk, "known target_fps serializes as the numeric 90.000000");
        printf("  [INFO] tfps contract json (%zu bytes)\n", j.size());
    }

    printf("\n========================================\nwebui_json results: %d passed, %d failed\n========================================\n", g_pass, g_fail);
    return g_fail==0?0:1;
}
