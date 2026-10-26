// test_state_ttl_d.cpp — D-002 regression: unified ttlHours<=0 semantics.
//
// Before the fix: stateTtlIsExpired(ttlHours<=0)==false (never expire) but
// stateTtlSweep() fell back to a hardcoded 86400s (24h) window for history /
// score-cache cleanup — while stale state files were never unlinked. The three
// subsystems disagreed.
//
// After the fix: ttlHours<=0 DISABLES all age-based sweeps uniformly
// (history, score cache, state files all retained); ttlHours>0 uses
// ttlHours*3600 everywhere. The liveness GC of g_threadSettle for dead threads
// still runs regardless.
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

    // --- stateTtlIsExpired boundary contract --------------------------------
    // ttlHours <= 0 must ALWAYS report "not expired".
    CK(stateTtlIsExpired(now - 10*86400, now, 0)  == false, "isExpired: ttlHours=0, 10d-old file NOT expired");
    CK(stateTtlIsExpired(now - 10*86400, now, -5) == false, "isExpired: ttlHours=-5, 10d-old file NOT expired");
    // ttlHours > 0 uses ttlHours*3600.
    CK(stateTtlIsExpired(now - 86400 - 1, now, 24) == true,  "isExpired: ttlHours=24h, 24h+1s-old IS expired");
    CK(stateTtlIsExpired(now - 3600,      now, 24) == false, "isExpired: ttlHours=24h, 1h-old NOT expired");
    CK(stateTtlIsExpired(now - 2*3600,    now, 1)  == true,  "isExpired: ttlHours=1h, 2h-old IS expired");

    // --- stateTtlSweep: history age cleanup respects the disabled flag ------
    // Seed one history entry whose lastUpdate is 10 days in the past.
    g_history.clear();
    HistoryEntry e; e.lastUpdate = now - 10*86400; e.samples = 1;
    g_history[4242] = e;

    // ttlHours <= 0 => age-based history sweep DISABLED -> entry retained.
    stateTtlSweep(now, 0);
    CK(g_history.find(4242) != g_history.end(),
       "sweep: ttlHours=0 keeps 10-day-old history entry (no 86400 fallback)");

    // ttlHours = 24 => age-based sweep ENABLED -> entry evicted.
    stateTtlSweep(now, 24);
    CK(g_history.find(4242) == g_history.end(),
       "sweep: ttlHours=24 evicts 10-day-old history entry");

    // ttlHours <= 0 with a FRESH entry is also retained (sanity).
    g_history.clear();
    HistoryEntry e2; e2.lastUpdate = now - 10; e2.samples = 1;
    g_history[4343] = e2;
    stateTtlSweep(now, 0);
    CK(g_history.find(4343) != g_history.end(), "sweep: ttlHours=0 keeps fresh entry");

    printf("==> state_ttl_d: %s\n", failures ? "FAILURES" : "ALL PASS");
    return failures ? 1 : 0;
}
