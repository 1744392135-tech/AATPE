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

    aatpeSessionPurgeAll();
    CHECK(aatpeSessionAnyActive() == false, "no sessions initially active");

    std::string junk(64, '0');
    CHECK(aatpeSessionVerify(junk) == false, "verify against empty store -> false (all slots unused)");
    CHECK(aatpeSessionVerify("") == false, "empty token -> false");
    CHECK(aatpeSessionVerify(std::string("short")) == false, "short token -> false");

    std::string tok = aatpeSessionIssue();
    CHECK(tok.size() == 64, "issued token is 64 hex chars");
    CHECK(aatpeSessionVerify(tok) == true, "issued token verifies true");
    CHECK(aatpeSessionAnyActive() == true, "session now marked active");

    std::string wrong = tok; wrong[0] = (wrong[0] == 'a') ? 'b' : 'a';
    CHECK(aatpeSessionVerify(wrong) == false, "one-byte-wrong token -> false");

    std::string tok2 = aatpeSessionIssue();
    CHECK(aatpeSessionVerify(tok) == true, "first token still valid after second issue");
    CHECK(aatpeSessionVerify(tok2) == true, "second token verifies true");

    aatpeSessionPurgeAll();
    CHECK(aatpeSessionVerify(tok) == false, "after purgeAll, first token -> false");
    CHECK(aatpeSessionVerify(tok2) == false, "after purgeAll, second token -> false");
    CHECK(aatpeSessionAnyActive() == false, "no sessions active after purgeAll");

    printf("=== SEC-04: fail-closed when CLOCK_MONOTONIC fails ===\n");
    std::string good = aatpeSessionIssue();
    CHECK(good.size() == 64, "[pre] healthy issue mints a 64-hex token");
    CHECK(aatpeSessionVerify(good) == true, "[pre] healthy token verifies");
    CHECK(aatpeSessionAnyActive() == true, "[pre] healthy session shows active");

    aatpeSessionSetClockFailForTest(true);

    std::string bad = aatpeSessionIssue();
    CHECK(bad.empty(), "clock-fail: issue() returns empty (refuses to mint)");

    CHECK(aatpeSessionVerify(good) == false,
          "clock-fail: verify(healthy token) -> false (fail-closed, no wall-clock fallback)");

    CHECK(aatpeSessionAnyActive() == false,
          "clock-fail: anyActive() -> false (badge degrades, not stale-true)");

    aatpeSessionSetClockFailForTest(false);
    aatpeSessionPurgeAll();
    std::string recovered = aatpeSessionIssue();
    CHECK(recovered.size() == 64, "clock-recovery: issue mints again");
    CHECK(aatpeSessionVerify(recovered) == true, "clock-recovery: recovered token verifies");
    aatpeSessionPurgeAll();

    printf("=== v449 session: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
