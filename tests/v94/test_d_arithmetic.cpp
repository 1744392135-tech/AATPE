#include <cstdio>
#include <initializer_list>

static int failures = 0;
static int ceilDiv(int clearSec, int intervalSec) {
    int v = (clearSec + intervalSec - 1) / intervalSec;
    return v < 1 ? 1 : v;
}
#define CK(cond, m) do { if (cond) printf("  [PASS] %s\n", m); \
                         else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main() {
    printf("==> D-006: emergIndNeedSamples ceiling division ==\n");
    CK(ceilDiv(30, 60) == 1, "ceil(30/60)=1 (interval>=clear: one sample ~60s)");
    CK(ceilDiv(30, 30) == 1, "ceil(30/30)=1");
    CK(ceilDiv(30, 20) == 2, "ceil(30/20)=2 (floor would give 1 — the bug)");
    CK(ceilDiv(30, 15) == 2, "ceil(30/15)=2");
    CK(ceilDiv(30, 10) == 3, "ceil(30/10)=3");
    CK(ceilDiv(30, 7)  == 5, "ceil(30/7)=5 (4.28->5 samples ~35s>=30s)");
    CK(ceilDiv(30,10)*10 >= 30, "wall-clock n*interval>=30 for interval=10");
    CK(ceilDiv(30,15)*15 >= 30, "wall-clock n*interval>=30 for interval=15");
    CK(ceilDiv(30,20)*20 >= 30, "wall-clock n*interval>=30 for interval=20");
    CK(ceilDiv(30,30)*30 >= 30, "wall-clock n*interval>=30 for interval=30");
    CK(ceilDiv(30,60)*60 >= 30, "wall-clock n*interval>=30 for interval=60");
    printf("==> d_arithmetic: %s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}
