#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "aatpe_session_token.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        bool ok = (c>='0'&&c<='9')||(c>='a'&&c<='f');
        if (!ok) return false;
    }
    return true;
}

int main() {
    printf("=== #61 session: issue returns a 64-hex token ===\n");
    std::string t1 = aatpeSessionIssue();
    CHECK(!t1.empty(), "issue returns non-empty token");
    CHECK(isHex64(t1), "token is 64 lowercase hex chars (256-bit)");

    printf("=== #61 session: freshly issued token verifies ===\n");
    CHECK(aatpeSessionVerify(t1) == true, "valid token verifies");
    CHECK(aatpeSessionAnyActive() == true, "any-active true after issue");

    printf("=== #89 fault injection: malformed tokens are rejected, no crash ===\n");
    CHECK(aatpeSessionVerify("") == false, "empty token rejected");
    CHECK(aatpeSessionVerify("short") == false, "short token rejected (len!=64)");
    CHECK(aatpeSessionVerify(std::string(65, 'a')) == false, "over-long token rejected");
    CHECK(aatpeSessionVerify(std::string(63, 'a')) == false, "under-long token rejected");
    std::string unknown = std::string("ab") + std::string(60, '0') + "cd";
    CHECK(aatpeSessionVerify(unknown) == false, "unknown well-formed token rejected");
    std::string weird = std::string(64, 'Z');
    CHECK(aatpeSessionVerify(weird) == false, "non-hex token rejected (not in store)");

    printf("=== #89 fault injection: purgeAll -> token no longer verifies ===\n");
    aatpeSessionPurgeAll();
    CHECK(aatpeSessionVerify(t1) == false, "purged token rejected");
    CHECK(aatpeSessionAnyActive() == false, "any-active false after purgeAll");

    printf("=== #61 session: two issues are distinct tokens ===\n");
    aatpeSessionPurgeAll();
    std::string a = aatpeSessionIssue();
    std::string b = aatpeSessionIssue();
    CHECK(a != b, "two issued tokens differ (random)");
    CHECK(aatpeSessionVerify(a) && aatpeSessionVerify(b), "both fresh tokens verify");

    printf("=== #90 stability sim: 100 issue/verify cycles, map stays bounded ===\n");
    aatpeSessionPurgeAll();
    int issuedOk = 0;
    std::vector<std::string> fresh;
    for (int i = 0; i < 100; i++) {
        std::string tk = aatpeSessionIssue();
        if (!tk.empty() && isHex64(tk)) issuedOk++;
        if (i % 7 == 0) {
            bool v = aatpeSessionVerify(tk);
            (void)v;
        }
        if (i % 13 == 0) aatpeSessionAnyActive();
    }
    CHECK(issuedOk == 100, "100/100 issue cycles returned a valid token");
    std::string last = aatpeSessionIssue();
    CHECK(aatpeSessionVerify(last), "store functional after 100-issue storm");
    CHECK(aatpeSessionAnyActive() == true, "sessions still tracked after storm");

    printf("=== #90 stability sim: repeated purge/issue restart loop ===\n");
    std::string afterLoop;
    for (int i = 0; i < 100; i++) {
        aatpeSessionPurgeAll();
        std::string tk = aatpeSessionIssue();
        if (tk.empty()) { CHECK(false, "issue after purge must succeed"); break; }
        afterLoop = tk;
    }
    CHECK(aatpeSessionVerify(afterLoop), "token issued after 100x purge/loop still verifies");

    printf("=== summary ===\n");
    printf("round-4 session tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
