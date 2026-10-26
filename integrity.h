// integrity.h - module-level anti-tamper hash-chain verification
// Self-contained FIPS 180-4 SHA-256 based chained integrity manifest.
// No external crypto dependency (no OpenSSL / libcrypto).
#ifndef AATPE_INTEGRITY_H
#define AATPE_INTEGRITY_H

#include <string>
#include <vector>
#include <cstdint>
#include "aatpe_crypto.h"       // LED-125 (R8-018): single canonical SHA-256.
                                // integrity used to carry its own private FIPS 180-4
                                // copy; it now delegates to aatpe_crypto so there is
                                // exactly one implementation in the build.

namespace aatpe {
namespace integrity {

// Per-file read/outcome classification (ROUND-003).
//  - Clean:   file present, successfully lstat+open+hashed; hexDigest is real.
//  - DefiniteTamper: we CONCLUSIVELY know the file is wrong (clean-lstat says
//    absent, or it is a symlink/non-regular, or we read it and the bytes differ).
//  - Transient: lstat/open/read failed with a transient errno (EIO/EINTR/EBUSY/
//    EACCES...) so we cannot conclude anything. This must NOT be treated as
//    tamper.
enum class ReadState { Clean, DefiniteTamper, Transient };

// Per-file integrity record.
struct FileEntry {
    std::string relPath;      // path relative to module root
    std::string absPath;      // fully resolved path on device
    std::string hexDigest;    // lowercase 64-char sha256 of file bytes
    bool        exists = false;   // AU-003: default-init (was uninitialized; UBSan)
    ReadState   readState = ReadState::Clean;  // ROUND-003
};

// ---------------------------------------------------------------------------
// Streaming SHA-256 (FIPS 180-4). LED-125: these are thin wrappers over the
// canonical implementation in aatpe_crypto.h (namespace aatpe_crypto). The
// context object wraps the canonical aatpe_crypto::Sha256Context, exposed under
// the historic name Sha256Ctx so existing call sites keep compiling. It is a
// distinct (non-typedef) type so unqualified sha256Init/Update/Final calls in
// this namespace do not become ambiguous with the aatpe_crypto:: overloads.
// ---------------------------------------------------------------------------
struct Sha256Ctx {
    aatpe_crypto::Sha256Context inner;
};

void sha256Init(Sha256Ctx* ctx);
void sha256Update(Sha256Ctx* ctx, const uint8_t* data, size_t len);
void sha256Final(Sha256Ctx* ctx, uint8_t out[32]);
// One-shot convenience: hash a buffer in a single call.
void sha256OneShot(const uint8_t* data, size_t len, uint8_t out[32]);

// Convert raw 32-byte digest to lowercase hex string (64 chars).
std::string toHexStr(const uint8_t* digest, size_t len = 32);

// Constant-time comparison of two hex strings (prevents timing attacks).
bool hexEqual(const std::string& a, const std::string& b);

// ---------------------------------------------------------------------------
// Module integrity high-level API.
// ---------------------------------------------------------------------------

// Resolve module root by probing standard module install paths.
// Returns "" when no candidate resolves.
std::string resolveModuleRoot();

// ROUND-002 (addendum): pin the module root explicitly. During a Magisk/KSU
// upgrade the new tree sits in modules_update while the OLD tree is still in
// modules; auto-probing picks the old tree and anchors stale hashes. The CLI
// (installer/service.sh) passes the active tree explicitly via argv or
// AATPE_MODULE_ROOT. When set, resolveModuleRoot() returns this path (after
// verifying it contains system/bin) and NEVER falls back to auto-probing.
// Empty string clears the override.
void setModuleRootOverride(const std::string& root);

// Enumerate the canonical set of critical files under moduleRoot.
// Order is load-bearing: the hash chain walks entries in this exact order.
std::vector<FileEntry> enumerateCriticalFiles(const std::string& moduleRoot);

// ROUND-002: the canonical relative-path list, for tests to assert the set does
// not regress to install-time-only / phantom entries.
std::vector<std::string> enumerateCriticalRelPaths();

// ROUND-002: true iff every critical entry resolves to an existing regular file
// under moduleRoot (no symlink, no missing). firstMissing receives the offending
// relative path on failure. Used by generateAndStoreManifest to refuse baking the
// sentinel into a baseline, and by the regression tests.
bool criticalSetComplete(const std::string& moduleRoot, std::string& firstMissing);

// Generate+store manifest against an explicit module root (the public
// generateAndStoreManifest() resolves the root itself; this overload exists for
// hermetic testing and reuses the same missing-file refusal).
bool generateAndStoreManifestAt(const std::string& moduleRoot);

// Stream-read a file and return its SHA-256 hex digest.
// Returns empty string on read failure.
std::string sha256FileHex(const std::string& absPath);

// ROUND-003: like sha256FileHex but reports the failure errno through *errOut so
// the caller can tell a transient IO error from a definite read failure.
std::string hashFileHex(const std::string& absPath, int* errOut);

// Compute the chained manifest digest over the given entries.
// Chain rule (blockchain-style):
//   cv_0 = 32 zero bytes
//   for each entry i (in order):
//     block_i = cv_{i-1} || entry.hexDigest_ascii (96 bytes total)
//     cv_i    = SHA256(block_i)
//   manifest = hex(cv_N)
std::string computeChainedManifest(const std::vector<FileEntry>& entries);

// Read the stored manifest from disk.
// Returns empty string when manifest file does not exist.
std::string readStoredManifest();

// Generate manifest from current on-disk state and persist to
// /data/adb/aatpe/manifest.sha256. Returns true on success.
bool generateAndStoreManifest();

// Full verification cycle:
//   1. Resolve module root (fail-closed)
//   2. Read stored manifest (fail-closed if missing)
//   3. Enumerate + hash all critical files
//   4. Recompute chain, compare to stored manifest
//   5. Write verdict to STATE_DIR/integrity_status ("OK" / "FAILED")
//   6. On failure, populate per-file tamper report
// Returns true when verification passes.
bool verifyModuleIntegrity();

// Human-readable tamper report: lists each file with expected/current hash
// and chain mismatch summary. Empty string when OK.
std::string getIntegrityReport();

// In-process cached verdict: 1 = OK, 0 = FAILED, -1 = not yet run.
int lastVerdict();

// ---------------------------------------------------------------------------
// Boot gate / capability revocation (G3-001).
//
// The verdict used to be written but never consumed: the daemon kept loading
// eBPF and accepting privileged /control writes even after integrity FAILED.
// These APIs make the verdict an enforceable gate. privilegedOpsAllowed() is
// fail-CLOSED: anything other than a confirmed OK (including the initial
// Unknown state before the first verify) denies privileged capability.
// ---------------------------------------------------------------------------
enum class IntegrityVerdict : int { Unknown = -1, Ok = 1, Failed = 0 };

// Thread-safe read of the cached verdict.
IntegrityVerdict verdict();

// True only when the last verify was OK. Used by the eBPF loader (G4) and the
// write-control gate (G8) to refuse privileged capability when tampering is
// detected. Unknown/Failed both return false.
bool privilegedOpsAllowed();

// Hook invoked on every verify that ends FAILED. The daemon wires this at
// startup to revoke privileged capability (e.g. force HMAC read-only mode) so a
// runtime re-verify failure immediately hardens the running process instead of
// only logging. Null in hermetic unit tests (no-op).
using IntegrityFailHook = void (*)(const char* reason);
void setIntegrityFailHook(IntegrityFailHook hook);

// ROUND-003: symmetric restore hook. Fired once when a previously-FAILED verdict
// recovers to OK (files re-match, e.g. user reverted / reinstalled). The daemon
// wires this to clear the HMAC read-only latch so a transiently-tripped device
// self-heals without a reboot.
using IntegrityRestoreHook = void (*)(const char* reason);
void setIntegrityRestoreHook(IntegrityRestoreHook hook);

// ROUND-003 test seams (used by hermetic unit tests; no-op in production).
#ifdef AATPE_HOST_TEST
void integrityTestInjectTransient(const std::string& absPath);
void integrityTestClearInject();
#endif

// N-01 test seams: prove the critical-file SHA-256 pass runs exactly once per
// verify (the daemon used to enumerate+hash the whole set twice per boot-gate).
// g_hashFileCalls counts every real on-disk file hash through hashFileHex();
// in-memory chain/sentinel digests go through sha256OneShot and are NOT counted.
#ifdef AATPE_HOST_TEST
long integrityTestHashFileCount();
void integrityTestResetHashFileCount();
#endif

// N-01 test seam: override the stored-manifest path so a host test can drive
// the FULL verifyModuleIntegrity() preamble (root resolve + manifest read +
// enumerate + chain compare) against a hermetic temp tree. Empty string restores
// the production default (/data/adb/aatpe/manifest.sha256). No-op in production.
#ifdef AATPE_HOST_TEST
void setIntegrityManifestPathForTest(const std::string& path);
#endif
// Run the full classify+debounce state machine against an explicit module root
// and explicit stored manifest (bypasses hardcoded /data/adb manifest path so
// tests can drive the strike counter and hooks deterministically).
bool verifyAgainstRootAndManifest(const std::string& root, const std::string& storedManifest);

// Boot gate: run a full verifyModuleIntegrity() and, on failure, fire the fail
// hook. Returns true when privileged ops may proceed. Designed to be called
// once at daemon startup (G1 patch) and reused by the hourly re-verify path.
bool runBootGate();

// TEST-ONLY seam (G11-002): force the cached verdict without running a manifest
// compare (which needs a real, trusted install). Hermetic/integration tests use
// this to drive the privileged write gate both ways: Unknown/Failed => 403,
// Ok => downstream whitelist/HMAC ordering becomes reachable. Production boot
// paths MUST NOT call this; it is no-op-visible only in test binaries.
#ifdef AATPE_HOST_TEST
void setIntegrityVerdictForTest(IntegrityVerdict v);
#endif

// Path to the persistent manifest file.
std::string manifestPath();

// ---------------------------------------------------------------------------
// Snapshot self-verification (used by uninstall.sh shell side, but also
// exposed here for the daemon to validate backup integrity on demand).
// ---------------------------------------------------------------------------

// Compute SHA-256 hex of a snapshot .kv file's value_b64 field.
// Returns empty string on read/parse failure.
std::string snapshotValueSha256(const std::string& kvFilePath);

// Verify a single snapshot .kv file: read path=, value_b64=, sha256= fields,
// recompute sha256(value_b64), compare to stored sha256= field.
// Returns true when the snapshot is intact and trustworthy.
bool verifySnapshotFile(const std::string& kvFilePath);

} // namespace integrity
} // namespace aatpe

#endif // AATPE_INTEGRITY_H
