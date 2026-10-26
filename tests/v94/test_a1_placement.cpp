// test_a1_placement.cpp — v4.2 Batch A1: uclampMinForTier mode=3 (SF tier) and the
// default-off bit-identical contract. Links the real placement.o graph.
//
// Contract:
//   * mode=3 (SF tier) -> floor = uclampSfPct(20%)*1024/100*ratio, evaluated BEFORE
//     the PRIME early-return, so an SF thread routed to PRIME is NOT yanked to
//     1024. With ratio=0.87: 20*1024/100=204, (int)(204*0.87)=177.
//   * modes 0/1/2 -> UNCHANGED (222/622/712; PRIME -> 1024) = v4.1.1 bit-identical.
#include <cstdio>

// placement.cpp now calls the schedtune bridge (gpu_boost.o). For this narrow
// uclamp test we deliberately do NOT link the whole gpu_boost dependency tree;
// the no-op aatpeBoostSet/Clear link stubs are provided by placement_deps.cpp
// (same recipe as the existing placement/v343_placement targets), so the link
// resolves without pulling in gpu_boost.o. (The bridge itself is covered by
// test_a2_boost.)

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== A1: uclampMinForTier mode=3 (SF tier) ===\n");
    // Same mangled-symbol binding as tests/v94/test_placement.cpp (signature
    // unchanged: (int,float,int) -> ...Eifi).
    extern int uclampMinForTier_prod(int targetTier, float bigCapacityRatio, int mode)
        asm("_ZN15aatpe_spillover5aatpe9placement16uclampMinForTierEifi");

    const float ratio = 0.87f;

    // --- mode=3 SF tier, across ALL tiers including PRIME ---
    int sf_big    = uclampMinForTier_prod(1, ratio, 3);
    int sf_prime  = uclampMinForTier_prod(3, ratio, 3);   // would be 1024 for mode!=3
    printf("  SF: big=%d prime=%d (expect ~177 both)\n", sf_big, sf_prime);
    CHECK(sf_big == 177, "mode=3 BIG -> 177 (20%%*1024*0.87)");
    CHECK(sf_prime == 177, "mode=3 PRIME -> 177 (PRIME does NOT override SF floor)");

    // --- default-off: modes 0/1/2 unchanged (v4.1.1 values) ---
    CHECK(uclampMinForTier_prod(1, ratio, 0) == 222, "mode=0 BIG unchanged = 222");
    CHECK(uclampMinForTier_prod(1, ratio, 1) == 622, "mode=1 BIG unchanged = 622");
    CHECK(uclampMinForTier_prod(1, ratio, 2) == 712, "mode=2 BIG unchanged = 712");
    CHECK(uclampMinForTier_prod(3, ratio, 0) == 1024, "mode=0 PRIME unchanged = 1024");
    CHECK(uclampMinForTier_prod(3, ratio, 2) == 1024, "mode=2 PRIME unchanged = 1024");

    // SF floor must be well BELOW the active floor (the whole point: compositor
    // threads do not grab the PRIME-level floor).
    CHECK(sf_big < 222, "SF floor (177) < passive floor (222)");

    printf("\n=== RESULT: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
