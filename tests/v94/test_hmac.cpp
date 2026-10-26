// test_hmac.cpp - v94 hermetic unit tests for aatped_hmac.cpp
// Links aatped_hmac.cpp + log_stub (see build.sh recipe).
// Covers: initHmacKey (generate-from-/dev/urandom on host), computeCommandHmac
// determinism, verifyCommandHmac (correct/wrong/empty signature, constant-time),
// isHmacReadonly fail-closed default.
#include <cstdio>
#include <cstring>
#include <string>
#include "aatped.h"
#include "aatped_hmac.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== Hmac: isHmacReadonly default (before init) ===\n");
    // P2-1 (crash-loop hardening): g_hmacReadonly now defaults FAIL-CLOSED. Until
    // initHmacKey() runs and either loads an existing key or generates+persists a
    // fresh one, the daemon stays read-only.
    CHECK(isHmacReadonly() == true, "readonly TRUE before init (fail-closed default)");

    printf("=== Hmac: initHmacKey generates a key on host ===\n");
    initHmacKey();
    CHECK(strlen(g_hmacKey) >= 64, "generated key is hex (>=64 chars)");
    CHECK(g_hmacActive == g_hmacRequired, "g_hmacActive mirrors required flag");
    CHECK(std::string(g_hmacKeyStatus) == "generated" ||
          std::string(g_hmacKeyStatus) == "loaded", "key status generated/loaded");
    // P2-1: a successful load/generate MUST clear the fail-closed latch so normal
    // authenticated writes are allowed (first-install enrollment not blocked).
    CHECK(isHmacReadonly() == false, "readonly FALSE after successful key establish");

#ifdef AATPE_HOST_TEST
    printf("=== Hmac: urandom/generate failure stays fail-closed (readonly==true) ===\n");
    {
        extern bool g_forceHmacGenFail;
        g_forceHmacGenFail = true;
        initHmacKey();  // load (no persisted key on host) -> generate fails
        CHECK(isHmacReadonly() == true, "generate failure -> readonly stays TRUE (fail-closed)");
        g_forceHmacGenFail = false;
        // Recover a working key so the rest of the suite still signs/verifies.
        initHmacKey();
        CHECK(isHmacReadonly() == false, "after seam cleared, init succeeds -> readonly FALSE");
    }
#endif

    printf("=== Hmac: computeCommandHmac deterministic, 64 hex chars ===\n");
    std::string s1 = computeCommandHmac("status");
    std::string s2 = computeCommandHmac("status");
    CHECK(s1 == s2, "same command -> same signature");
    CHECK(s1.size() == 64, "signature is 64 hex chars (256-bit)");
    std::string s3 = computeCommandHmac("threads");
    CHECK(s3 != s1, "different command -> different signature");

    printf("=== Hmac: verifyCommandHmac correct/wrong ===\n");
    CHECK(verifyCommandHmac("status", s1.c_str()) == true, "matching signature verifies");
    CHECK(verifyCommandHmac("status", "deadbeef") == false, "wrong signature rejected");
    CHECK(verifyCommandHmac("other", s1.c_str()) == false, "sig from another command rejected");

    printf("=== Hmac: empty signature ALWAYS rejected (fail-closed, G3-003) ===\n");
    // P3-18: an empty/missing signature must NEVER verify, regardless of the
    // hmacRequired config flag. The old test asserted "not required -> empty
    // accepted", which contradicted the fail-closed code.
    bool emptyOk = verifyCommandHmac("status", "");
    CHECK(emptyOk == false, "empty signature always rejected (fail-closed)");
    bool nullOk = verifyCommandHmac("status", nullptr);
    CHECK(nullOk == false, "null signature rejected");
    // Over-long signature (captured 64-hex sig + trailing garbage) must be rejected.
    std::string overlong = s1 + "deadbeef";
    CHECK(verifyCommandHmac("status", overlong.c_str()) == false,
          "over-long signature (trailing garbage) rejected");

    printf("=== Hmac: SEC-03 defensive NULL checks (no crash) ===\n");
    // computeCommandHmac(NULL) must return empty string, not SIGSEGV.
    std::string nullSig = computeCommandHmac(nullptr);
    CHECK(nullSig.empty(), "computeCommandHmac(NULL) returns empty (no crash)");
    // verifyCommandHmac(NULL command, any sig) must return false, not SIGSEGV.
    bool nullCmd = verifyCommandHmac(nullptr, s1.c_str());
    CHECK(nullCmd == false, "verifyCommandHmac(NULL cmd, valid sig) returns false (no crash)");

    printf("=== Hmac: checkHmacKeyRotation keeps key usable ===\n");
    checkHmacKeyRotation();
    // After the rotation check the key must still be operational: a fresh
    // sign+verify round-trip must succeed (rotation is a no-op when the key is
    // still inside its validity window, which it is on a fresh /dev/urandom key).
    std::string rt = computeCommandHmac("post-rotation");
    CHECK(verifyCommandHmac("post-rotation", rt.c_str()) == true,
          "sign+verify round-trip still works after rotation check");

    printf("=== Hmac: read-only latch force/clear (ROUND-003 symmetric unlock) ===\n");
    CHECK(isHmacReadonly() == false, "readonly is false after healthy init");
    // Force readonly: privileged/write path is gated by isHmacReadonly() in webui.
    hmacForceReadonly("test");
    CHECK(isHmacReadonly() == true, "hmacForceReadonly -> readonly latched ON");
    // Idempotent re-force must not error.
    hmacForceReadonly("test-again");
    CHECK(isHmacReadonly() == true, "re-force keeps readonly ON");
    // Symmetric clear (integrity recovery hook) restores command processing.
    hmacClearReadonly("restored");
    CHECK(isHmacReadonly() == false, "hmacClearReadonly -> readonly released");
    // After release a fresh sign+verify round-trip still works.
    std::string after = computeCommandHmac("after-unlock");
    CHECK(verifyCommandHmac("after-unlock", after.c_str()) == true,
          "sign+verify round-trip works after readonly release");

    printf("\n========================================\n"
           "Hmac results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
