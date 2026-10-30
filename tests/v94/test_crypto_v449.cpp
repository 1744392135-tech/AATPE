#include <cstdio>
#include <string>
#include "aatpe_crypto.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static std::string hex64(char fill) {
    return std::string(64, fill);
}

int main() {
    using namespace aatpe_crypto;
    printf("=== v4.4.9 FX4a: constantTimeHexEqual fixed-width correctness ===\n");

    std::string a = hex64('a');
    std::string b = hex64('a');
    CHECK(constantTimeHexEqual(a, b) == true, "two identical 64-char hex -> true");

    std::string c = hex64('a'); c[63] = 'b';
    CHECK(constantTimeHexEqual(a, c) == false, "one-char diff (last byte) -> false");

    std::string d = hex64('a'); d[0] = 'b';
    CHECK(constantTimeHexEqual(a, d) == false, "one-char diff (first byte) -> false");

    CHECK(constantTimeHexEqual(std::string("abc"), std::string("abcd")) == false,
          "length mismatch (3 vs 4) -> false");
    CHECK(constantTimeHexEqual(std::string(""), std::string("")) == true,
          "two empty strings -> true");
    CHECK(constantTimeHexEqual(std::string(""), std::string("x")) == false,
          "empty vs non-empty -> false");
    CHECK(constantTimeHexEqual(std::string("deadbeef"), std::string("deadbeef")) == true,
          "equal short (<64) strings -> true");
    CHECK(constantTimeHexEqual(std::string("deadbeef"), std::string("deadbee0")) == false,
          "diff short (<64) strings -> false");

    printf("=== v449 crypto: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
