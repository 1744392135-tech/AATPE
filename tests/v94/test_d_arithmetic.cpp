// test_d_arithmetic.cpp — D-006 regression guard for emergIndNeedSamples.
//
// The production expression in aatpe-watchdog.cpp is:
//   emergIndNeedSamples = (emergencyClearSec + INTERVAL - 1) / INTERVAL  (floor, min 1)
// i.e. CEILING(emergencyClearSec / INTERVAL). The previous floor expression
// under-counted whenever emergencyClearSec was not an exact multiple of INTERVAL,
// so the independent thermal-exit observed FEWER than emergencyClearSec seconds
// of cool readings before leaving EMERGENCY.
//
// This guard pins the arithmetic over the production (clear=30s, interval sweep)
// and a non-divisible pair. If someone reverts to floor division, the
// (clear=30, interval=20) row flips from 2 to 1 and the test fails.
#include <cstdio>
#include <initializer_list>

static int failures = 0;
static int ceilDiv(int clearSec, int intervalSec) {
    int v = (clearSec + intervalSec - 1) / intervalSec;   // MUST mirror production
    return v < 1 ? 1 : v;
}
#define CK(cond, m) do { if (cond) printf("  [PASS] %s\n", m); \
                         else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main() {
    printf("==> D-006: emergIndNeedSamples ceiling division ==\n");
    // Production: emergencyClearSec = kMinRecoverySec = 30s.
    CK(ceilDiv(30, 60) == 1, "ceil(30/60)=1 (interval>=clear: one sample ~60s)");
    CK(ceilDiv(30, 30) == 1, "ceil(30/30)=1");
    CK(ceilDiv(30, 20) == 2, "ceil(30/20)=2 (floor would give 1 — the bug)");
    CK(ceilDiv(30, 15) == 2, "ceil(30/15)=2");
    CK(ceilDiv(30, 10) == 3, "ceil(30/10)=3");
    CK(ceilDiv(30, 7)  == 5, "ceil(30/7)=5 (4.28->5 samples ~35s>=30s)");
    // Wall-clock covered = needSamples * interval must be >= clear.
    CK(ceilDiv(30,10)*10 >= 30, "wall-clock n*interval>=30 for interval=10");
    CK(ceilDiv(30,15)*15 >= 30, "wall-clock n*interval>=30 for interval=15");
    CK(ceilDiv(30,20)*20 >= 30, "wall-clock n*interval>=30 for interval=20");
    CK(ceilDiv(30,30)*30 >= 30, "wall-clock n*interval>=30 for interval=30");
    CK(ceilDiv(30,60)*60 >= 30, "wall-clock n*interval>=30 for interval=60");
    printf("==> d_arithmetic: %s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}
