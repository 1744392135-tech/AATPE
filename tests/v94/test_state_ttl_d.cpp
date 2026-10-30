#include "aatped.h"
#include "aatpe_state_ttl.h"
#include <cstdio>
#include <ctime>

static int failures = 0;
#define CK(c, m) do { if (c) printf("  [PASS] %s\n", m); \
                      else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main() {
    printf("==> D-002: ttlHours<=0 unified semantics ==\n");
    time_t now = 1'000'000;

    CK(stateTtlIsExpired(now - 10*86400, now, 0)  == false, "isExpired: ttlHours=0, 10d-old file NOT expired");
    CK(stateTtlIsExpired(now - 10*86400, now, -5) == false, "isExpired: ttlHours=-5, 10d-old file NOT expired");
    CK(stateTtlIsExpired(now - 86400 - 1, now, 24) == true,  "isExpired: ttlHours=24h, 24h+1s-old IS expired");
    CK(stateTtlIsExpired(now - 3600,      now, 24) == false, "isExpired: ttlHours=24h, 1h-old NOT expired");
    CK(stateTtlIsExpired(now - 2*3600,    now, 1)  == true,  "isExpired: ttlHours=1h, 2h-old IS expired");

    g_history.clear();
    HistoryEntry e; e.lastUpdate = now - 10*86400; e.samples = 1;
    g_history[4242] = e;

    stateTtlSweep(now, 0);
    CK(g_history.find(4242) != g_history.end(),
       "sweep: ttlHours=0 keeps 10-day-old history entry (no 86400 fallback)");

    stateTtlSweep(now, 24);
    CK(g_history.find(4242) == g_history.end(),
       "sweep: ttlHours=24 evicts 10-day-old history entry");

    g_history.clear();
    HistoryEntry e2; e2.lastUpdate = now - 10; e2.samples = 1;
    g_history[4343] = e2;
    stateTtlSweep(now, 0);
    CK(g_history.find(4343) != g_history.end(), "sweep: ttlHours=0 keeps fresh entry");

    printf("==> state_ttl_d: %s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}
