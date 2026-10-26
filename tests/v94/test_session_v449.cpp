// test_session_v449.cpp - v4.4.9 R3-F06: session verify must keep working after
// unused slots were made to participate in the 64-byte constant-time XOR. The
// fix removed `if(!s.used) continue;` (which leaked slot-state timing); the
// functional contract (issue -> verify true; wrong/empty/purged -> false) must
// be unchanged and the scan over all 8 slots must not crash.
#include <cstdio>
#include <string>
#include "aatpe_session_token.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== v4.4.9 FX4a: session token verify (unused-slot constant-time scan) ===\n");

    // Start clean.
    aatpeSessionPurgeAll();
    CHECK(aatpeSessionAnyActive() == false, "no sessions initially active");

    // No session: a well-formed 64-hex random token must verify false, and the
    // scan over all-unused slots must not crash (this is the F-06 path).
    std::string junk(64, '0');
    CHECK(aatpeSessionVerify(junk) == false, "verify against empty store -> false (all slots unused)");
    CHECK(aatpeSessionVerify("") == false, "empty token -> false");
    CHECK(aatpeSessionVerify(std::string("short")) == false, "short token -> false");

    // Issue a real token.
    std::string tok = aatpeSessionIssue();
    CHECK(tok.size() == 64, "issued token is 64 hex chars");
    CHECK(aatpeSessionVerify(tok) == true, "issued token verifies true");
    CHECK(aatpeSessionAnyActive() == true, "session now marked active");

    // A token differing in one byte must verify false (constant-time XOR fold).
    std::string wrong = tok; wrong[0] = (wrong[0] == 'a') ? 'b' : 'a';
    CHECK(aatpeSessionVerify(wrong) == false, "one-byte-wrong token -> false");

    // Another issue leaves the first valid (multiple live slots exercise the
    // all-slots scan with a mix of used/unused).
    std::string tok2 = aatpeSessionIssue();
    CHECK(aatpeSessionVerify(tok) == true, "first token still valid after second issue");
    CHECK(aatpeSessionVerify(tok2) == true, "second token verifies true");

    // Purge everything -> both must now fail.
    aatpeSessionPurgeAll();
    CHECK(aatpeSessionVerify(tok) == false, "after purgeAll, first token -> false");
    CHECK(aatpeSessionVerify(tok2) == false, "after purgeAll, second token -> false");
    CHECK(aatpeSessionAnyActive() == false, "no sessions active after purgeAll");

    printf("=== SEC-04: fail-closed when CLOCK_MONOTONIC fails ===\n");
    // Seed one healthy session first (clock working).
    std::string good = aatpeSessionIssue();
    CHECK(good.size() == 64, "[pre] healthy issue mints a 64-hex token");
    CHECK(aatpeSessionVerify(good) == true, "[pre] healthy token verifies");
    CHECK(aatpeSessionAnyActive() == true, "[pre] healthy session shows active");

    // Force the monotonic clock read to fail (e.g. hostile seccomp / broken vDSO).
    aatpeSessionSetClockFailForTest(true);

    // issue() must refuse to mint a token (cannot stamp TTL) -> returns "".
    std::string bad = aatpeSessionIssue();
    CHECK(bad.empty(), "clock-fail: issue() returns empty (refuses to mint)");

    // verify() must reject even the previously-healthy token (fail-closed: no
    // trustworthy clock => no enforceable TTL => do NOT authenticate).
    CHECK(aatpeSessionVerify(good) == false,
          "clock-fail: verify(healthy token) -> false (fail-closed, no wall-clock fallback)");

    // anyActive() must report no active sessions (WebUI badge flips to logged-out).
    CHECK(aatpeSessionAnyActive() == false,
          "clock-fail: anyActive() -> false (badge degrades, not stale-true)");

    // Recovery: clock comes back, the next issue works and the slot table is
    // still usable. This proves the fail-closed path did not wedge state.
    aatpeSessionSetClockFailForTest(false);
    aatpeSessionPurgeAll();
    std::string recovered = aatpeSessionIssue();
    CHECK(recovered.size() == 64, "clock-recovery: issue mints again");
    CHECK(aatpeSessionVerify(recovered) == true, "clock-recovery: recovered token verifies");
    aatpeSessionPurgeAll();

    printf("=== v449 session: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
