// test_integrity.cpp - v94 hermetic unit tests for integrity.cpp.
// Links integrity.cpp + integrity_deps + log_stub (see build.sh recipe).
// Exercises the self-contained FIPS 180-4 SHA-256, hex helpers, constant-time
// compare, and the blockchain-style chained manifest (order/tamper sensitivity).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "integrity.h"

using namespace aatpe::integrity;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== Integrity: SHA-256 known vectors (FIPS 180-4) ===\n");
    // SHA-256("")
    {
        uint8_t out[32];
        sha256OneShot(reinterpret_cast<const uint8_t*>(""), 0, out);
        std::string h = toHexStr(out);
        CHECK(h == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "SHA-256(\"\") matches FIPS vector");
    }
    // SHA-256("abc")
    {
        const char* abc = "abc";
        uint8_t out[32];
        sha256OneShot(reinterpret_cast<const uint8_t*>(abc), 3, out);
        std::string h = toHexStr(out);
        CHECK(h == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA-256(\"abc\") matches FIPS vector");
    }

    printf("=== Integrity: toHexStr shape ===\n");
    {
        uint8_t out[32] = {0};
        sha256OneShot(reinterpret_cast<const uint8_t*>("x"), 1, out);
        std::string h = toHexStr(out);
        CHECK(h.size() == 64, "hex digest is 64 chars");
        bool lower = true;
        for (char c : h) if (c >= 'A' && c <= 'F') lower = false;
        CHECK(lower, "hex digest is lowercase");
    }

    printf("=== Integrity: hexEqual (constant-time compare) ===\n");
    CHECK(hexEqual("abc123", "abc123"), "equal strings compare equal");
    CHECK(!hexEqual("abc123", "abc124"), "differing last char -> not equal");
    CHECK(!hexEqual("abc", "abc123"), "different length -> not equal");
    CHECK(hexEqual("", ""), "empty strings compare equal");

    printf("=== Integrity: chained manifest determinism ===\n");
    // Build two known digests explicitly.
    uint8_t d1[32], d2[32];
    sha256OneShot(reinterpret_cast<const uint8_t*>("file-one"), 8, d1);
    sha256OneShot(reinterpret_cast<const uint8_t*>("file-two"), 8, d2);
    FileEntry f1; f1.relPath="a"; f1.hexDigest = toHexStr(d1);
    FileEntry f2; f2.relPath="b"; f2.hexDigest = toHexStr(d2);
    std::vector<FileEntry> chain = { f1, f2 };
    std::string m1 = computeChainedManifest(chain);
    std::string m2 = computeChainedManifest(chain);
    CHECK(m1 == m2, "same input -> identical chained manifest");
    CHECK(m1.size() == 64, "manifest digest is 64 hex chars");

    printf("=== Integrity: tamper detection (one file changed) ===\n");
    FileEntry f2tampered = f2;
    uint8_t d2t[32];
    sha256OneShot(reinterpret_cast<const uint8_t*>("file-two-TAMPERED"), 16, d2t);
    f2tampered.hexDigest = toHexStr(d2t);
    std::vector<FileEntry> chainTampered = { f1, f2tampered };
    std::string mTampered = computeChainedManifest(chainTampered);
    CHECK(mTampered != m1, "changing one file's digest flips the chained manifest");

    printf("=== Integrity: order sensitivity ===\n");
    std::vector<FileEntry> chainReordered = { f2, f1 };
    std::string mReordered = computeChainedManifest(chainReordered);
    CHECK(mReordered != m1, "entry order is load-bearing (chain differs)");

    printf("=== Integrity: empty chain is deterministic ===\n");
    std::string mEmpty = computeChainedManifest({});
    CHECK(mEmpty == std::string(64, '0'), "empty chain -> all-zero cv hex");

    // ------------------------------------------------------------------
    // G3-004 / G3-001: boot-gate & capability-revocation negative tests.
    // On the host, resolveModuleRoot() cannot find /data/adb/modules/aatpe, so
    // verifyModuleIntegrity() takes the root-unresolved FAILED path. We use that
    // deterministic host behavior to prove the gate semantics end to end.
    // ------------------------------------------------------------------
    printf("=== G3-001: privilegedOpsAllowed() is fail-closed by default ===\n");
    CHECK(privilegedOpsAllowed() == false,
          "before any verify, privileged ops are DENIED (Unknown != Ok)");
    CHECK(verdict() == IntegrityVerdict::Unknown || verdict() == IntegrityVerdict::Failed,
          "verdict starts non-OK");

    printf("=== G3-001: fail-hook fires on a FAILED verify ===\n");
    static int g_hookCalls = 0;
    setIntegrityFailHook([](const char* /*reason*/) { g_hookCalls++; });
    int before = g_hookCalls;
    bool v = runBootGate();
    CHECK(v == false, "boot gate FAILS when module root cannot be resolved (host)");
    CHECK(g_hookCalls > before, "fail-hook fired on FAILED verify (capability revoked)");
    CHECK(privilegedOpsAllowed() == false, "after FAILED, privileged ops still denied");

    printf("=== G3-001: FAILED must NOT auto-bootstrap a manifest ===\n");
    // The host cannot write /data/adb/aatpe; regardless, verify must not return
    // OK and must not create a baseline. We re-run and assert it stays FAILED.
    bool v2 = runBootGate();
    CHECK(v2 == false, "repeated verify still FAILED (no auto-rebuild to OK)");

    printf("=== G3-006: malformed (short) digest does not OOB-crash the chain ===\n");
    {
        FileEntry bad; bad.relPath = "x"; bad.hexDigest = std::string("abc"); // short!
        std::string m = computeChainedManifest({ bad });
        CHECK(m.size() == 64, "short digest handled gracefully (no OOB), still 64-hex out");
    }

    printf("=== G3-005: hexEqual is constant-time on length mismatch ===\n");
    CHECK(!hexEqual("abc", "abc123"), "length-mismatch still rejects");
    CHECK(hexEqual("abcd1234", "abcd1234"), "equal still accepts");

    // ------------------------------------------------------------------
    // ROUND-002: critical-list correctness + generate-time missing refusal.
    // ------------------------------------------------------------------
    printf("=== ROUND-002: critical list excludes install-time-only/phantom paths ===\n");
    {
        auto rels = enumerateCriticalRelPaths();
        bool hasCustomize = false, hasProbe = false;
        for (const auto& r : rels) {
            if (r == "customize.sh") hasCustomize = true;
            if (r == "system/bin/aatpe-temp-probe.sh") hasProbe = true;
        }
        CHECK(!hasCustomize, "customize.sh NOT in critical list (install-time only)");
        CHECK(!hasProbe, "aatpe-temp-probe.sh NOT in critical list (phantom, never shipped)");
        CHECK(!rels.empty(), "critical list is non-empty");
    }

    printf("=== ROUND-002: generate refuses to anchor when a critical file is missing ===\n");
    {
        namespace fs = std::filesystem;
        fs::path root = fs::temp_directory_path() / ("aatpe_g3_r2_" + std::to_string((long)::getpid()));
        fs::remove_all(root);
        // Materialize every critical rel path as a regular file.
        for (const auto& rel : enumerateCriticalRelPaths()) {
            fs::path p = root / rel;
            fs::create_directories(p.parent_path());
            std::ofstream(p) << "content:" << rel;
        }
        std::string missing;
        bool complete = criticalSetComplete(root.string(), missing);
        CHECK(complete, "complete temp tree -> criticalSetComplete()==true");

        // Remove one critical file; the generate guard must refuse (no sentinel bake).
        fs::remove(root / "system/bin/aatped");
        bool completeAfter = criticalSetComplete(root.string(), missing);
        CHECK(completeAfter == false, "after removing a critical file -> set incomplete");
        CHECK(missing == "system/bin/aatped", "first missing entry reported correctly");
        bool gen = generateAndStoreManifestAt(root.string());
        CHECK(gen == false, "generateAndStoreManifestAt REFUSES with a missing critical file");
        fs::remove_all(root);
    }

    printf("=== ROUND-002 addendum: explicit module-root routing (no auto-probe drift) ===\n");
    {
        namespace fs = std::filesystem;
        // Build two complete trees: "old" (v3.4.6 payload) and "new" (v3.4.7).
        auto makeTree = [](const fs::path& base, const std::string& tag) {
            fs::remove_all(base);
            for (const auto& rel : enumerateCriticalRelPaths()) {
                fs::path p = base / rel;
                fs::create_directories(p.parent_path());
                std::ofstream(p) << tag << ":" << rel;  // content differs per tree
            }
        };
        fs::path base = fs::temp_directory_path() / ("aatpe_g3_r2b_" + std::to_string((long)::getpid()));
        fs::path oldTree = base / "old", newTree = base / "new";
        makeTree(oldTree, "OLDv346");
        makeTree(newTree, "NEWv347");

        // Explicit root = newTree wins and is validated.
        setModuleRootOverride(newTree.string());
        CHECK(resolveModuleRoot() == newTree.string(), "explicit root=newTree honored");
        std::string miss;
        CHECK(criticalSetComplete(newTree.string(), miss) == true, "newTree complete");

        // old vs new payloads produce different chains (the upgrade swap would
        // mismatch unless generate anchored the NEW tree).
        auto oldEntries = enumerateCriticalFiles(oldTree.string());
        auto newEntries = enumerateCriticalFiles(newTree.string());
        std::string oldChain = computeChainedManifest(oldEntries);
        std::string newChain = computeChainedManifest(newEntries);
        CHECK(oldChain != newChain, "old vs new payload -> different chained manifests");

        // Invalid explicit root (no system/bin) -> resolveModuleRoot() returns ""
        // and must NOT silently fall back to auto-probing another tree.
        setModuleRootOverride("/nonexistent/aatpe_xyz");
        CHECK(resolveModuleRoot() == "", "invalid explicit root -> empty (no fallback)");

        setModuleRootOverride(""); // clear -> auto-probe behavior restored
        fs::remove_all(base);
    }

    // ==================================================================
    // ROUND-003: transient/debounce/symmetric-unlock state machine.
    // ==================================================================
    printf("=== ROUND-003: transient ignored, N=3 latches, symmetric unlock ===\n");
    {
        namespace fs = std::filesystem;
        fs::path root = fs::temp_directory_path() / ("aatpe_g3_r3_" + std::to_string((long)::getpid()));
        fs::remove_all(root);
        // Build a complete tree; baseline content "base".
        for (const auto& rel : enumerateCriticalRelPaths()) {
            fs::path p = root / rel;
            fs::create_directories(p.parent_path());
            std::ofstream(p) << "base:" << rel;
        }
        std::string goodManifest = computeChainedManifest(enumerateCriticalFiles(root.string()));

        static int failHits, restoreHits;
        failHits = restoreHits = 0;
        setIntegrityFailHook([](const char*) { failHits++; });
        setIntegrityRestoreHook([](const char*) { restoreHits++; });

        // Healthy baseline verify -> OK. This may itself fire the restore hook if a
        // previous (host) test left the verdict Failed; that's expected. Capture the
        // baseline AFTER this settling verify so all further assertions are deltas.
        CHECK(verifyAgainstRootAndManifest(root.string(), goodManifest) == true,
              "R3: healthy tree verifies OK");
        int fBase = failHits, rBase = restoreHits;

        // (a) Inject a transient read error on a present file: verdict must not
        // flip, no hook fired.
        integrityTestInjectTransient((root / "system/bin/aatped").string());
        bool t1 = verifyAgainstRootAndManifest(root.string(), goodManifest);
        integrityTestClearInject();
        CHECK(t1 == true, "R3: transient read keeps previous (healthy) verdict");
        CHECK(failHits == fBase && restoreHits == rBase, "R3: transient fires no hook");

        // (d) Real content mismatch: verify returns false IMMEDIATELY on first notice.
        std::ofstream(root / "system/bin/aatped") << "TAMPERED-BYTES";
        bool d1 = verifyAgainstRootAndManifest(root.string(), goodManifest);
        CHECK(d1 == false, "R3: real mismatch fails immediately (strike 1)");
        CHECK(failHits == fBase, "R3: strike 1 not yet latched (no irreversible hook)");
        // SEC-02: privileged ops must be revoked IMMEDIATELY on first definite tamper,
        // even though the irreversible hmacForceReadonly hook only fires after 3 strikes.
        CHECK(privilegedOpsAllowed() == false,
              "R3 SEC-02: strike 1 already revokes privileged ops (fail-closed)");

        // (b) Need 3 consecutive definite tampers to latch the fail hook.
        bool d2 = verifyAgainstRootAndManifest(root.string(), goodManifest);
        CHECK(d2 == false && failHits == fBase, "R3: strike 2 still not latched");
        bool d3 = verifyAgainstRootAndManifest(root.string(), goodManifest);
        CHECK(d3 == false && failHits == fBase + 1, "R3: strike 3 -> FAILED latched + fail hook");
        CHECK(verdict() == IntegrityVerdict::Failed, "R3: verdict latched Failed");

        // (c) Restore the file; a healthy verify -> OK fires the RESTORE hook exactly
        // once more than baseline.
        std::ofstream(root / "system/bin/aatped") << "base:system/bin/aatped";
        bool ok = verifyAgainstRootAndManifest(root.string(), goodManifest);
        CHECK(ok == true, "R3: restored tree verifies OK");
        CHECK(restoreHits == rBase + 1, "R3: recovery to OK fired restore hook");
        CHECK(verdict() == IntegrityVerdict::Ok, "R3: verdict back to Ok");

        fs::remove_all(root);
    }

    // ==================================================================
    // N-01: the critical-file SHA-256 set must be hashed EXACTLY once per verify.
    // Drives the FULL verifyModuleIntegrity() preamble (root resolve + manifest
    // read + enumerate + chain compare) against a hermetic temp tree via the
    // test-only seams. On the baseline code, verifyModuleIntegrity enumerated +
    // hashed the whole set (pass #1, result discarded) and then
    // verifyAgainstRootAndManifest enumerated + hashed it AGAIN (pass #2) -> 2x
    // the SHA-256 work over tens-of-MB binaries. After the fix the counter is 1x.
    // ==================================================================
    printf("=== N-01: single critical-file hash pass per verify ===\n");
    {
        namespace fs = std::filesystem;
        fs::path root = fs::temp_directory_path() / ("aatpe_n01_" + std::to_string((long)::getpid()));
        fs::remove_all(root);
        // Materialize every critical rel path as a regular file. The user-config
        // file (aatpe.conf) is intentionally NOT created -> its diagnostics loop
        // does not hash anything, so every hashFileHex call maps 1:1 to a
        // critical file.
        for (const auto& rel : enumerateCriticalRelPaths()) {
            fs::path p = root / rel;
            fs::create_directories(p.parent_path());
            std::ofstream(p) << "n01:" << rel;
        }
        const long criticalCount = (long)enumerateCriticalRelPaths().size();

        // Anchor the manifest over THIS tree's digests.
        std::string goodManifest = computeChainedManifest(enumerateCriticalFiles(root.string()));

        // Wire seams: explicit module root + explicit manifest file path.
        setModuleRootOverride(root.string());
        fs::path mfile = fs::temp_directory_path() /
                         ("aatpe_n01_man_" + std::to_string((long)::getpid()) + ".sha256");
        { std::ofstream(mfile) << goodManifest; }
        setIntegrityManifestPathForTest(mfile.string());

        // (1) Healthy tree through the real full preamble -> PASS.
        integrityTestResetHashFileCount();
        bool ok = verifyModuleIntegrity();
        long hashes = integrityTestHashFileCount();
        CHECK(ok == true, "N-01: healthy hermetic tree verifies OK via full preamble");
        CHECK(hashes == criticalCount,
              "N-01: critical files hashed exactly once per verify (no redundant 2nd pass)");
        printf("       [info] critical=%ld hashCalls=%ld (expect %ld)\n",
               criticalCount, hashes, criticalCount);

        // (2) Tamper one critical file -> must be rejected (fail-closed), and the
        // failure path must also hash the set exactly once (never double-hash).
        std::ofstream(root / "system/bin/aatped") << "EVIL-BYTES-N01";
        integrityTestResetHashFileCount();
        bool tampered = verifyModuleIntegrity();
        long hashesT = integrityTestHashFileCount();
        CHECK(tampered == false, "N-01: tampered critical file is rejected");
        CHECK(hashesT == criticalCount,
              "N-01: tamper path also hashes the critical set exactly once");
        printf("       [info] critical=%ld hashCalls(tamper)=%ld (expect %ld)\n",
               criticalCount, hashesT, criticalCount);

        // Clear seams + cleanup.
        setIntegrityManifestPathForTest("");
        setModuleRootOverride("");
        fs::remove(mfile);
        fs::remove_all(root);
    }

    printf("\n=== SEC-07: multi value_b64 line — producer/verifier agree ===\n");
    {
        namespace fs = std::filesystem;
        // A hand-edited / tampered .kv file may carry TWO "value_b64=" lines.
        // The producer (snapshotValueSha256) and the verifier
        // (verifySnapshotFile) MUST hash the same line. The old code broke on
        // the FIRST value_b64 in the producer but took the LAST in the verifier,
        // so a multi-line file yielded a digest the verifier could not reproduce.
        // We take the LAST occurrence on both sides now.
        const std::string firstB64  = "AAAA";                 // stale / old value
        const std::string lastB64   = "QUJDREVGRw==";         // newest value (wins)

        uint8_t d[32];
        sha256OneShot(reinterpret_cast<const uint8_t*>(lastB64.data()),
                      lastB64.size(), d);
        std::string shaOfLast = toHexStr(d, 32);

        fs::path kv = fs::temp_directory_path() /
                      ("aatpe_sec07_" + std::to_string((long)::getpid()) + ".kv");
        {
            std::ofstream f(kv);
            f << "path=/data/adb/aatpe/snap/state.kv\n";
            f << "value_b64=" << firstB64 << "\n";   // stale first
            f << "value_b64=" << lastB64 << "\n";     // newest last (wins)
            f << "sha256=" << shaOfLast << "\n";
        }

        // Producer hashes the LAST value (not the first).
        std::string produced = snapshotValueSha256(kv.string());
        CHECK(produced == shaOfLast,
              "SEC-07: snapshotValueSha256 hashes the LAST value_b64 line");

        // Verifier agrees: stored sha256 == sha256(last value) -> verify true.
        bool ok = verifySnapshotFile(kv.string());
        CHECK(ok == true,
              "SEC-07: verifySnapshotFile passes on a multi-line .kv (last-wins)");

        // Negative control: flip the stored digest to sha256(first) -> verify
        // must now FAIL (proving we are not just returning true unconditionally).
        uint8_t d2[32];
        sha256OneShot(reinterpret_cast<const uint8_t*>(firstB64.data()),
                      firstB64.size(), d2);
        std::string shaOfFirst = toHexStr(d2, 32);
        {
            std::ofstream f(kv);
            f << "path=/data/adb/aatpe/snap/state.kv\n";
            f << "value_b64=" << firstB64 << "\n";
            f << "value_b64=" << lastB64 << "\n";
            f << "sha256=" << shaOfFirst << "\n";
        }
        CHECK(verifySnapshotFile(kv.string()) == false,
              "SEC-07: verify fails when stored sha256 is the (stale) first-line digest");

        fs::remove(kv);
    }

    printf("\n========================================\n"
           "Integrity results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
