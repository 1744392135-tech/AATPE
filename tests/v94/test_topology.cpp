#include <cstdio>
#include <set>
#include <string>
#include "aatped.h"
#include "aatped_topology.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static bool has(const std::set<int>& s, int v) { return s.count(v) != 0; }

static bool poolEq(const std::vector<int>& pool, std::initializer_list<int> want) {
    std::vector<int> got = pool, w(want);
    std::sort(got.begin(), got.end());
    std::sort(w.begin(), w.end());
    return got == w;
}

int main() {
    printf("=== Topology: parseAffinityMask ranges ===\n");
    std::set<int> r = parseAffinityMask("0-3,7");
    CHECK(has(r, 0) && has(r, 3) && has(r, 7), "range 0-3 plus single 7 parsed");
    CHECK(!has(r, 4), "cpu 4 not in 0-3,7");

    printf("=== Topology: parseAffinityMask single list ===\n");
    std::set<int> s = parseAffinityMask("1,3,5");
    CHECK(s.size() == 3 && has(s, 5), "comma list 1,3,5");

    printf("=== Topology: parseAffinityMask hex bitmask ===\n");
    std::set<int> h = parseAffinityMask("0f");
    CHECK(has(h, 0) && has(h, 3) && !has(h, 4), "G6-014: 2-char '0f' bitmask -> cpus 0..3");
    std::set<int> ff = parseAffinityMask("ff");
    CHECK(has(ff, 0) && has(ff, 3), "G6-014: 'ff' single-byte mask resolves to cpus 0..3");
    std::set<int> hx = parseAffinityMask("0000000f");
    CHECK(has(hx, 0) && has(hx, 3) && !has(hx, 4), "8-char hex 0x0f -> cpus 0..3");

    printf("=== Topology: empty / garbage -> empty set ===\n");
    CHECK(parseAffinityMask("").empty(), "empty string -> empty");
    CHECK(parseAffinityMask("  ").empty(), "whitespace -> empty");
    CHECK(parseAffinityMask("xyz!").empty(), "non-hex garbage -> empty");

    printf("=== Topology: getThreadAffinityCpus(self) non-empty ===\n");
    cpu_set_t me = getThreadAffinityCpus(0);
    me = getThreadAffinityCpus((int)syscall(SYS_gettid));
    CHECK(CPU_COUNT(&me) > 0, "self thread has at least one allowed cpu");

    printf("=== Topology: readThreadCurrentCpu missing proc -> -1 ===\n");
    CHECK(readThreadCurrentCpu(999999, 999999) == -1, "nonexistent proc/task -> -1");

    printf("=== Topology: detectCpuTopology graceful on host ===\n");
    detectCpuTopology();
    CHECK(g_allCpus.size() > 0, "detectCpuTopology populates g_allCpus via affinity fallback");

    auto runCluster = [](const std::vector<int>& cpus,
                         const std::vector<int>& caps,
                         const std::vector<int>& freqs) {
        g_allCpus = cpus;
        g_littlePool.clear(); g_midPool.clear(); g_bigPool.clear();
        buildGenericClusters(caps, freqs);
    };

    printf("=== Topology B1: Dimensity 1100 (1+3+4) -> LITTLE4/BIG3/PRIME1 ===\n");
    {
        std::vector<int> cpus = {0,1,2,3,4,5,6,7};
        std::vector<int> caps = {241,241,241,241, 917,917,917, 1024};
        std::vector<int> freq= {200000,200000,200000,200000, 200000,200000,200000, 260000};
        runCluster(cpus, caps, freq);
        CHECK(clusterCount() == 3, "D1100 -> 3 clusters");
        CHECK(tierOfCpu(7) == CpuTier::PRIME, "D1100 cpu7 = PRIME");
        CHECK(tierOfCpu(4) == CpuTier::BIG,   "D1100 cpu4 = BIG");
        CHECK(tierOfCpu(0) == CpuTier::LITTLE,"D1100 cpu0 = LITTLE");
        std::vector<int> pp = primePool();
        CHECK(pp.size()==1 && pp[0]==7, "primePool = {7}");
    }

    printf("=== Topology B1: 8 homogeneous -> 1 cluster, no PRIME ===\n");
    {
        std::vector<int> cpus = {0,1,2,3,4,5,6,7};
        std::vector<int> caps = {200,200,200,200,200,200,200,200};
        std::vector<int> freq= {180000,180000,180000,180000,180000,180000,180000,180000};
        runCluster(cpus, caps, freq);
        CHECK(clusterCount() == 1, "8同构 -> 1 cluster");
        CHECK(tierOfCpu(0) != CpuTier::PRIME, "8同构 -> no PRIME");
        CHECK(primePool().empty(), "8同构 primePool empty");
    }

    printf("=== Topology B1: 4+4 dual cluster -> LITTLE/BIG, no PRIME ===\n");
    {
        std::vector<int> cpus = {0,1,2,3,4,5,6,7};
        std::vector<int> caps = {300,300,300,300, 800,800,800,800};
        std::vector<int> freq= {180000,180000,180000,180000, 280000,280000,280000,280000};
        runCluster(cpus, caps, freq);
        CHECK(clusterCount() == 2, "4+4 -> 2 clusters");
        CHECK(tierOfCpu(4) == CpuTier::BIG, "4+4 cpu4 = BIG");
        CHECK(tierOfCpu(0) == CpuTier::LITTLE, "4+4 cpu0 = LITTLE");
        CHECK(primePool().empty(), "4+4 -> no PRIME");
    }

    printf("=== Topology B1: 1+4+3 (3 clusters, top=PRIME) ===\n");
    {
        std::vector<int> cpus = {0,1,2,3,4,5,6,7};
        std::vector<int> caps = {200,200,200, 700,700,700,700, 1024};
        std::vector<int> freq= {180000,180000,180000, 240000,240000,240000,240000, 300000};
        runCluster(cpus, caps, freq);
        CHECK(clusterCount() == 3, "1+4+3 -> 3 clusters");
        CHECK(tierOfCpu(7) == CpuTier::PRIME, "1+4+3 cpu7 = PRIME");
        CHECK(tierOfCpu(3) == CpuTier::BIG, "1+4+3 cpu3 = BIG");
    }

    printf("=== Topology B1: capacity missing -> freq clustering still works ===\n");
    {
        std::vector<int> cpus = {0,1,2,3,4,5,6,7};
        std::vector<int> caps = {380,380,380,380,380,380,380,380};
        std::vector<int> freq= {180000,180000,180000,180000, 240000,240000,240000, 300000};
        runCluster(cpus, caps, freq);
        CHECK(clusterCount() >= 2, "no-cap -> freq clustering produces >=2 clusters");
    }

    printf("=== Topology B1: clustersStateText format ===\n");
    {
        std::string txt = clustersStateText();
        CHECK(txt.find("LITTLE,") != std::string::npos, "state text has LITTLE line");
        CHECK(txt.find(":0,") != std::string::npos || txt.find(":0") != std::string::npos, "state text lists cpulist");
        CHECK(txt.find("-") != std::string::npos, "state text has freq range");
    }

    printf("=== Topology B1: PRIME_POOL manual override ===\n");
    {
        g_cpuCount = 8;
        g_littlePool = {0,1,2,3}; g_midPool = {}; g_bigPool = {4,5,6};
        g_clusters.clear();
        CpuCluster lc; lc.cpus={0,1,2,3}; lc.tier=CpuTier::LITTLE;
        CpuCluster bc; bc.cpus={4,5,6}; bc.tier=CpuTier::BIG;
        g_clusters = {lc, bc};
        applyManualPrimeOverride("7");
        CHECK(tierOfCpu(7)==CpuTier::PRIME, "PRIME_POOL=7 -> cpu7 PRIME");
        CHECK(std::find(g_bigPool.begin(),g_bigPool.end(),7)==g_bigPool.end(), "cpu7 removed from BIG");
        CHECK(primePool().size()==1 && primePool()[0]==7, "primePool={7}");
    }
    printf("=== Topology B1: PRIME_POOL illegal cpu rejected ===\n");
    {
        g_cpuCount = 8;
        g_littlePool={0,1,2,3}; g_midPool={}; g_bigPool={4,5,6,7};
        g_clusters.clear();
        CpuCluster lc; lc.cpus={0,1,2,3}; lc.tier=CpuTier::LITTLE;
        CpuCluster bc; bc.cpus={4,5,6,7}; bc.tier=CpuTier::BIG;
        g_clusters = {lc, bc};
        applyManualPrimeOverride("9");
        CHECK(primePool().empty(), "illegal cpu9 -> no PRIME override");
    }
    printf("=== Topology B1: PRIME_POOL overlapping BIG rejected ===\n");
    {
        g_cpuCount = 8;
        g_littlePool={0,1,2,3}; g_midPool={}; g_bigPool={4,5,6,7};
        g_clusters.clear();
        CpuCluster lc; lc.cpus={0,1,2,3}; lc.tier=CpuTier::LITTLE;
        CpuCluster bc; bc.cpus={4,5,6,7}; bc.tier=CpuTier::BIG;
        g_clusters = {lc, bc};
        applyManualPrimeOverride("7");
        CHECK(tierOfCpu(7)==CpuTier::BIG, "overlap cpu7 stays BIG, not PRIME");
    }
    printf("=== Topology R2: PRIME_POOL dedup (0-2,1) ===\n");
    {
        g_cpuCount = 8;
        g_littlePool={0,1,2,3}; g_midPool={}; g_bigPool={4,5,6,7};
        g_clusters.clear();
        CpuCluster lc; lc.cpus={0,1,2,3}; lc.tier=CpuTier::LITTLE;
        CpuCluster bc; bc.cpus={4,5,6,7}; bc.tier=CpuTier::BIG;
        g_clusters = {lc, bc};
        applyManualPrimeOverride("0-2,1");
        CHECK(primePool().size()==3, "dedup: 0-2,1 -> 3 cpus (no double count)");
    }

    printf("=== Topology R2: SoC matrix ===\n");
    runCluster({0,1,2,3,4,5,6,7},
               {300,300,300,300, 800,800,800,800},
               {180000,180000,180000,180000, 280000,280000,280000,280000});
    CHECK(clusterCount()==2, "SD845 4+4 -> 2 clusters");
    CHECK(tierOfCpu(4)==CpuTier::BIG && primePool().empty(), "SD845 -> no PRIME");

    runCluster({0,1,2,3,4,5,6,7},
               {200,200,200,200,200,200,200,200},
               {180000,180000,180000,180000,180000,180000,180000,180000});
    CHECK(clusterCount()==1 && primePool().empty(), "SD835 8同构 -> 1 cluster, no PRIME");

    runCluster({0,1,2,3,4,5,6,7},
               {200,200,200,200,200,200, 750,750},
               {200000,200000,200000,200000,200000,200000, 240000,240000});
    CHECK(clusterCount()==2, "Dim900 2+6 -> 2 clusters");
    CHECK(tierOfCpu(6)==CpuTier::PRIME && !primePool().empty(), "Dim900 -> 2 fast cores PRIME (F1 fail-open)");

    printf("=== W2-X F1: 2+6 / 1+4 ratio-lead PRIME, fail-open ===\n");
    {
        runCluster({0,1,2,3,4,5,6,7},
                   {840,840,840,840,840,840, 1024,1024},
                   {260000,260000,260000,260000,260000,260000, 340000,340000});
        CHECK(tierOfCpu(6)==CpuTier::PRIME && !primePool().empty(),
              "8-Elite 2+6 -> 2 fast cores PRIME (primePool non-empty)");
    }
    {
        runCluster({0,1,2,3,4},
                   {241,241,241,241, 1024},
                   {200000,200000,200000,200000, 220000});
        CHECK(tierOfCpu(4)==CpuTier::PRIME && !primePool().empty(),
              "1+4 -> superbig PRIME via capacity lead (F1)");
    }
    {
        runCluster({0,1,2,3,4,5,6,7},
                   {380,380,380,380,380,380,380,380},
                   {180000,180000,180000,180000, 240000,240000,240000, 280000});
        CHECK(tierOfCpu(7)==CpuTier::PRIME, "capacity-denied freq-only -> cpu7 PRIME");
    }
    {
        runCluster({0,1,2,3,4,5,6,7},
                   {0,0,0,0,0,0,0,0},
                   {0,0,0,0,0,0,0,0});
        CHECK(clusterCount()==1 && primePool().empty(),
              "capacity+freq denied -> 1 cluster fail-open, no PRIME");
    }

    runCluster({0,1,2,3,4,5,6,7},
               {241,241,241,241, 895,895,895, 1024},
               {180000,180000,180000,180000, 240000,240000,240000, 284000});
    CHECK(clusterCount()==3, "SD888 1+3+4 -> 3 clusters");
    CHECK(tierOfCpu(7)==CpuTier::PRIME && tierOfCpu(4)==CpuTier::BIG, "SD888 -> cpu7 PRIME, cpu4 BIG");

    runCluster({0,1,2,3,4,5,6,7},
               {241,241,241,241, 895,895,895, 1024},
               {180000,180000,180000,180000, 240000,240000,240000, 300000});
    CHECK(tierOfCpu(7)==CpuTier::PRIME, "SD870 -> cpu7 PRIME (freq ratio 300/240=1.25)");

    runCluster({0,1,2,3,4,5,6,7},
               {200,200,200,200, 825,825,825, 1024},
               {180000,180000,180000,180000, 250000,250000,250000, 260000});
    CHECK(clusterCount()==3, "Exynos2100 1+3+4 -> 3 clusters");
    CHECK(tierOfCpu(7)==CpuTier::PRIME, "Exynos2100 -> cpu7 PRIME (cap ratio 1024/825=1.24)");

    runCluster({0,1,2,3,4,5,6,7},
               {250,250,250,250, 700,700,700,700},
               {190000,190000,190000,190000, 200000,200000,200000,200000});
    CHECK(clusterCount()==2 && primePool().empty(), "TV 4+4 -> 2 clusters, no PRIME");

    runCluster({0,1,2,3,4,5,6,7},
               {200,200,200,200, 400, 800,800, 1024},
               {180000,180000,180000,180000, 210000, 250000,250000, 300000});
    CHECK(clusterCount()==4, "1+2+1+4 -> 4 clusters");
    CHECK(tierOfCpu(7)==CpuTier::PRIME, "4-cluster -> cpu7 PRIME");

    runCluster({0,1,2,3,4,5,6,7},
               {380,380,380,380,380,380,380,380},
               {180000,180000,180000,180000, 240000,240000,240000, 300000});
    CHECK(clusterCount()>=2, "no-cap -> freq split >=2");

    runCluster({0,1,2,3,4,5,6,7},
               {200,200,200,200, 800,800,800,800},
               {0,0,0,0,0,0,0,0});
    CHECK(clusterCount()==2, "no-freq -> cap split ==2");

    runCluster({0,1,2,3,4,5,6,7},
               {380,380,380,380,380,380,380,380},
               {0,0,0,0,0,0,0,0});
    CHECK(clusterCount()==1, "no-cap,no-freq -> 1 cluster (safe)");

    printf("=== Topology R5: partial capacity failure -> layer invalid, no scramble ===\n");
    {
        runCluster({0,1,2,3,4,5,6,7},
               {0,0,0,0,0,0,0,0},
               {200000,200000,200000,200000,200000,200000,200000, 260000});
        CHECK(clusterCount() <= 2, "R5: partial cap fail -> <=2 clean clusters (no scramble)");
        CHECK(tierOfCpu(7)==CpuTier::PRIME || tierOfCpu(7)==CpuTier::BIG, "R5: top core 7 is lead tier");
    }

    printf("=== Topology R5: backoff retry timeline ===\n");
    {
        markTopologyReady();
        CHECK(topologyRetryDue(0)==false, "ready -> no retry");
    }

    printf("=== Topology R6: policy-first clustering ===\n");
    {
        std::map<int,int> pol;
        for (int c=0;c<4;c++) pol[c]=0;
        for (int c=4;c<7;c++) pol[c]=4;
        pol[7]=7;
        topologyInjectPolicyForTest(pol);
        runCluster({0,1,2,3,4,5,6,7},
               {241,241,241,241, 917,917,917, 1024},
               {200000,200000,200000,200000, 200000,200000,200000, 260000});
        CHECK(tierOfCpu(7)==CpuTier::PRIME, "R6 D1100 policy7 -> cpu7 PRIME");
        CHECK(tierOfCpu(4)==CpuTier::BIG,   "R6 D1100 cpu4 BIG");
        CHECK(tierOfCpu(0)==CpuTier::LITTLE,"R6 D1100 cpu0 LITTLE");
        CHECK(primePool().size()==1 && primePool()[0]==7, "R6 D1100 primePool={7}");
    }
    {
        std::map<int,int> pol;
        for (int c=0;c<6;c++) pol[c]=0;
        for (int c=6;c<8;c++) pol[c]=6;
        topologyInjectPolicyForTest(pol);
        runCluster({0,1,2,3,4,5,6,7},
               {800,800,800,800,800,800, 1024,1024},
               {240000,240000,240000,240000,240000,240000, 320000,320000});
        CHECK(clusterCount()==2, "R6 8-Elite 2+6 -> 2 clusters");
        CHECK(tierOfCpu(6)==CpuTier::PRIME, "R6 8-Elite cpu6 PRIME (2-core lead)");
        CHECK(tierOfCpu(0)==CpuTier::BIG, "R6 8-Elite cpu0 = BIG (N1: 2+N no-LITTLE)");
        CHECK(poolEq(g_littlePool, {}), "R6 8-Elite g_littlePool EMPTY (no real LITTLE)");
        CHECK(poolEq(g_bigPool, {0,1,2,3,4,5}), "R6 8-Elite g_bigPool = 6 performance cores");
        CHECK(g_midPool.empty(), "R6 8-Elite g_midPool empty (2-tier box)");
        CHECK(poolEq(primePool(), {6,7}), "R6 8-Elite primePool = {6,7}");
    }
    {
        topologyInjectPolicyForTest({});
        runCluster({0,1,2,3,4,5,6,7},
               {241,241,241,241, 917,917,917, 1024},
               {200000,200000,200000,200000, 200000,200000,200000, 260000});
        CHECK(clusterCount()>=2, "R6 no-policy fallback still splits >=2");
    }

    printf("=== Topology R7: real D1100 data (identical max freq, policy7) ===\n");
    {
        std::map<int,int> pol;
        for (int c=0;c<4;c++) pol[c]=0;
        for (int c=4;c<7;c++) pol[c]=4;
        pol[7]=7;
        topologyInjectPolicyForTest(pol);
        runCluster({0,1,2,3,4,5,6,7},
               {241,241,241,241, 917,917,917, 1024},
               {2000000,2000000,2000000,2000000, 2600000,2600000,2600000, 2600000});
        CHECK(tierOfCpu(7)==CpuTier::PRIME, "R7 D1100 real data: cpu7 PRIME");
        CHECK(tierOfCpu(4)==CpuTier::BIG,   "R7 D1100 cpu4 BIG");
        CHECK(tierOfCpu(0)==CpuTier::LITTLE,"R7 D1100 cpu0 LITTLE");
        std::string txt = clustersStateText();
        bool fmtOk = true;
        size_t pos = 0;
        while (pos < txt.size()) {
            size_t nl = txt.find('\n', pos);
            if (nl == std::string::npos) nl = txt.size();
            std::string line = txt.substr(pos, nl - pos);
            size_t c1 = line.find(',');
            size_t c2 = line.find(',', c1+1);
            size_t dash = line.find('-', c2+1);
            size_t colon = line.find(':', c2+1);
            if (c1==std::string::npos || c2==std::string::npos ||
                dash==std::string::npos || colon==std::string::npos ||
                dash < c2 || colon < dash) { fmtOk = false; break; }
            pos = nl + 1;
        }
        CHECK(fmtOk, "R7 clustersStateText every line has tier,cap,min-max:cpulist");
    }
    topologyInjectPolicyForTest({});

    printf("=== Topology R7d: clusters publishable guard ===\n");
    {
        CHECK(clustersTextPublishable("LITTLE,241,2000000-2000000:0,1,2,3\nPRIME,1024,2600000-2600000:7"),
              "R7d: well-formed 2-line text publishable");
        CHECK(!clustersTextPublishable("PRIME,1024,2600000:7"),
              "R7d: 1-line missing min-max NOT publishable (keeps old file)");
        CHECK(clustersTextPublishable("LITTLE,241,2000000-2000000:0,1,2,3"),
              "R7d: single cluster (835) IS publishable");
        CHECK(!clustersTextPublishable(""), "R7d: empty not publishable");
    }

    printf("=== Topology R10: 8-form hermetic matrix ===\n");
    {
        std::map<int,int> d1100;
        for(int c=0;c<4;c++) d1100[c]=0;
        for(int c=4;c<7;c++) d1100[c]=4;
        d1100[7]=7;
        topologyInjectPolicyForTest(d1100);
        runCluster({0,1,2,3,4,5,6,7},{241,241,241,241,917,917,917,1024},
               {2000000,2000000,2000000,2000000,2600000,2600000,2600000,2600000});
        CHECK(tierOfCpu(7)==CpuTier::PRIME, "R10 D1100 cpu7 PRIME");
    }
    {
        std::map<int,int> p;
        for(int c=0;c<4;c++) p[c]=0;
        for(int c=4;c<8;c++) p[c]=4;
        topologyInjectPolicyForTest(p);
        runCluster({0,1,2,3,4,5,6,7},{900,900,900,900,900,900,900,900},
               {2800000,2800000,2800000,2800000,2800000,2800000,2800000,2800000});
        CHECK(clusterCount()==2, "R10 9300 4+4 -> 2 clusters");
        CHECK(tierOfCpu(7)!=CpuTier::PRIME, "R10 9300 same cap/freq -> no PRIME");
    }
    {
        topologyInjectPolicyForTest({});
        runCluster({0,1,2,3},{400,400,400,400},{2000000,2000000,2000000,2000000});
        CHECK(clusterCount()==1, "R10 835 k=1");
        CHECK(clustersTextPublishable(clustersStateText()), "R10 835 clusters published");
    }
    {
        std::map<int,int> p;
        for(int c=0;c<6;c++) p[c]=0;
        for(int c=6;c<8;c++) p[c]=6;
        topologyInjectPolicyForTest(p);
        runCluster({0,1,2,3,4,5,6,7},{800,800,800,800,800,800,1024,1024},
               {2400000,2400000,2400000,2400000,2400000,2400000,3200000,3200000});
        CHECK(tierOfCpu(6)==CpuTier::PRIME, "R10 8Elite cpu6 PRIME");
    }
    topologyInjectPolicyForTest({});

    printf("=== Topology N1 (audit5 U-01): 2+6 no-LITTLE pool routing + controls ===\n");
    {
        std::map<int,int> pol;
        for (int c=0;c<6;c++) pol[c]=0;
        for (int c=6;c<8;c++) pol[c]=6;
        topologyInjectPolicyForTest(pol);
        runCluster({0,1,2,3,4,5,6,7},
                   {800,800,800,800,800,800, 1024,1024},
                   {240000,240000,240000,240000,240000,240000, 320000,320000});
        CHECK(clusterCount()==2, "N1 2+6 -> 2 clusters");
        CHECK(tierOfCpu(0)==CpuTier::BIG,    "N1 cpu0 = BIG (performance, not LITTLE)");
        CHECK(tierOfCpu(6)==CpuTier::PRIME,  "N1 cpu6 = PRIME");
        CHECK(poolEq(g_littlePool, {}),      "N1 g_littlePool EMPTY (no real LITTLE)");
        CHECK(poolEq(g_bigPool, {0,1,2,3,4,5}), "N1 g_bigPool = exactly the 6 performance cores");
        CHECK(g_midPool.empty(),            "N1 g_midPool empty");
        CHECK(poolEq(primePool(), {6,7}),   "N1 PRIME cores in primePool = {6,7}");
    }
    {
        topologyInjectPolicyForTest({});
        runCluster({0,1,2,3,4,5,6,7},
                   {840,840,840,840,840,840, 1024,1024},
                   {260000,260000,260000,260000,260000,260000, 340000,340000});
        CHECK(tierOfCpu(6)==CpuTier::PRIME, "N1-nopolicy cpu6 = PRIME (F1 ratio lead)");
        CHECK(tierOfCpu(0)==CpuTier::BIG,  "N1-nopolicy cpu0 = BIG (not LITTLE)");
        CHECK(poolEq(g_littlePool, {}),     "N1-nopolicy g_littlePool EMPTY");
        CHECK(poolEq(g_bigPool, {0,1,2,3,4,5}), "N1-nopolicy g_bigPool = 6 performance cores");
        CHECK(poolEq(primePool(), {6,7}),  "N1-nopolicy primePool = {6,7}");
    }
    {
        topologyInjectPolicyForTest({});
        runCluster({0,1,2,3,4,5,6,7},
                   {300,300,300,300, 800,800,800,800},
                   {180000,180000,180000,180000, 280000,280000,280000,280000});
        CHECK(clusterCount()==2,            "N1-control 4+4 -> 2 clusters");
        CHECK(tierOfCpu(0)==CpuTier::LITTLE,"N1-control cpu0 = LITTLE (unchanged)");
        CHECK(tierOfCpu(4)==CpuTier::BIG,   "N1-control cpu4 = BIG (unchanged)");
        CHECK(poolEq(g_littlePool, {0,1,2,3}), "N1-control g_littlePool = {0..3}");
        CHECK(poolEq(g_bigPool, {4,5,6,7}),    "N1-control g_bigPool = {4..7}");
        CHECK(primePool().empty(),          "N1-control no PRIME");
    }
    {
        std::map<int,int> pol;
        for(int c=0;c<4;c++) pol[c]=0;
        for(int c=4;c<7;c++) pol[c]=4;
        pol[7]=7;
        topologyInjectPolicyForTest(pol);
        runCluster({0,1,2,3,4,5,6,7},
                   {241,241,241,241, 917,917,917, 1024},
                   {2000000,2000000,2000000,2000000, 2600000,2600000,2600000,2600000});
        CHECK(poolEq(g_littlePool, {0,1,2,3}), "N1-control D1100 g_littlePool = {0..3}");
        CHECK(poolEq(g_bigPool, {4,5,6}),     "N1-control D1100 g_bigPool = {4..6}");
        CHECK(poolEq(primePool(), {7}),       "N1-control D1100 primePool = {7}");
        CHECK(g_midPool.empty(),             "N1-control D1100 g_midPool empty");
    }
    topologyInjectPolicyForTest({});

    printf("\n========================================\n"
           "Topology results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
