#include <cstdio>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== A1: uclampMinForTier mode=3 (SF tier) ===\n");
    extern int uclampMinForTier_prod(int targetTier, float bigCapacityRatio, int mode)
        asm("_ZN15aatpe_spillover5aatpe9placement16uclampMinForTierEifi");

    const float ratio = 0.87f;

    int sf_big    = uclampMinForTier_prod(1, ratio, 3);
    int sf_prime  = uclampMinForTier_prod(3, ratio, 3);
    printf("  SF: big=%d prime=%d (expect ~177 both)\n", sf_big, sf_prime);
    CHECK(sf_big == 177, "mode=3 BIG -> 177 (20%%*1024*0.87)");
    CHECK(sf_prime == 177, "mode=3 PRIME -> 177 (PRIME does NOT override SF floor)");

    CHECK(uclampMinForTier_prod(1, ratio, 0) == 222, "mode=0 BIG unchanged = 222");
    CHECK(uclampMinForTier_prod(1, ratio, 1) == 622, "mode=1 BIG unchanged = 622");
    CHECK(uclampMinForTier_prod(1, ratio, 2) == 712, "mode=2 BIG unchanged = 712");
    CHECK(uclampMinForTier_prod(3, ratio, 0) == 1024, "mode=0 PRIME unchanged = 1024");
    CHECK(uclampMinForTier_prod(3, ratio, 2) == 1024, "mode=2 PRIME unchanged = 1024");

    CHECK(sf_big < 222, "SF floor (177) < passive floor (222)");

    printf("\n=== RESULT: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
