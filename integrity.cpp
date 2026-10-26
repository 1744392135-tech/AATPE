// integrity.cpp - module-level anti-tamper hash-chain verification
//
// Self-contained implementation:
//   * SHA-256 (FIPS 180-4) implemented inline here, no OpenSSL dependency.
//   * Hash chain: H1=SHA256(file1), H2=SHA256(H1||file2_hex), ...
//     Any single-file modification shifts the final manifest digest.
//   * Manifest persisted to /data/adb/aatpe/manifest.sha256 (outside module
//     tree so it cannot be trivially rewritten with the files it protects).
//   * Runtime verdict written to STATE_DIR/integrity_status.
//   * Snapshot self-verification helpers for uninstall restore safety.
//
// Build: compiled into aatped via Makefile (AATPED_SRCS).
#include "integrity.h"
#include "aatped.h"            // RUN_DIR, STATE_DIR, LOG_*, writeFile
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <vector>
#include <atomic>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

namespace aatpe {
namespace integrity {

// ============================================================================
// SHA-256 (FIPS 180-4). LED-125 (R8-018): integrity no longer keeps its own
// compression function / K table / rotations. The streaming entry points below
// delegate to the single canonical implementation in aatpe_crypto.h
// (namespace aatpe_crypto). This also retires the former private rotr32() whose
// `x << (32 - n)` was undefined behaviour for n == 0 (R8-020 / LED-127); the
// canonical rotr() carries the same n==0 guard.
// ============================================================================

void sha256Init(Sha256Ctx* ctx) {
    aatpe_crypto::sha256Init(&ctx->inner);
}

void sha256Update(Sha256Ctx* ctx, const uint8_t* data, size_t len) {
    aatpe_crypto::sha256Update(&ctx->inner, data, len);
}

void sha256Final(Sha256Ctx* ctx, uint8_t out[32]) {
    aatpe_crypto::sha256Final(&ctx->inner, out);
}

void sha256OneShot(const uint8_t* data, size_t len, uint8_t out[32]) {
    aatpe_crypto::sha256(data, len, out);
}

std::string toHexStr(const uint8_t* digest, size_t len) {
    static const char hexChars[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        result += hexChars[(digest[i] >> 4) & 0xF];
        result += hexChars[digest[i] & 0xF];
    }
    return result;
}

bool hexEqual(const std::string& a, const std::string& b) {
    // G3-005: delegate to the canonical constant-time comparison. The previous
    // implementation early-returned on length mismatch; this now folds the
    // length difference into the accumulator (no early exit).
    return aatpe_crypto::constantTimeHexEqual(a, b);
}

// ============================================================================
// Canonical critical-file list (RELATIVE paths under module root).
// Order is load-bearing: the hash chain walks entries in this exact order.
// ============================================================================
// IMMUTABLE integrity set. These are the binary / code /
// shell assets that must never change after install. aatpe.conf is deliberately
// EXCLUDED: it is a USER-CONFIG file that the CLI and WebUI rewrite at runtime, so
// treating it as tamper-on-change would false-positive on every config edit.
// Runtime data (recovery, state, logs, ML checkpoints) is also excluded.
static const char* kCriticalRelPaths[] = {
    "system/bin/aatped",
    "system/bin/aatpe-cli",
    "system/bin/aatpe-watchdog",
    "system/bin/aatpe-probe",
    "service.sh",
    "post-fs-data.sh",
    // ROUND-002: "customize.sh" removed. It runs only from the installer's temp
    // dir at install time and is NEVER present in the active module dir
    // /data/adb/modules/aatpe afterwards -> runtime verify saw it as MISSING and
    // baked the sentinel into the baseline, causing a permanent false TAMPER.
    "uninstall.sh",
    "module.prop",
    "webroot/index.html",
    "system/etc/aatpe/webui/index.html",
    "system/etc/aatpe/profiles/balanced.conf",
    "system/etc/aatpe/profiles/gaming.conf",
    "system/etc/aatpe/profiles/battery.conf",
    "system/etc/aatpe/profiles/thermal.conf",
    "system/etc/aatpe/ebpf/sched_events_embedded.h",
    // P1-4: any file that changes code / BPF / shell scripts / key scheduling
    // config / WebUI control logic MUST be in this list. Appended at the END so
    // the existing hash-chain prefix is unchanged; generate and verify both walk
    // this same list so they stay consistent.
    //
    // sched_events.bpf.o — the precompiled eBPF object loaded at runtime by
    //   EbpfScheduler::tryLoadSyscallPrecompiled() (aatpe_ebpf.cpp). Previously
    //   only sched_events_embedded.h was protected, so an attacker could swap the
    //   on-disk .o and the multi-strategy loader would bind a tampered program
    //   while integrity still reported OK.
    "system/etc/aatpe/ebpf/sched_events.bpf.o",
    // ROUND-002: "system/bin/aatpe-temp-probe.sh" removed. It was a phantom
    // entry: the file has never been shipped in pkg1/pkg2/fix_tree (the comment
    // claiming a "shipped diagnostic shell script" was wrong). Baking the
    // sentinel for a file that does not exist permanently is exactly the false
    // TAMPER that bricked eBPF attach after the G3-001 gate was wired.
    // thread_roles.conf — key scheduling config loaded by thread_scan.cpp.
    //   It classifies threads into roles (game/render/audio/...) which directly
    //   drives placement/migration decisions. Tampering would silently shift
    //   scheduling behavior while integrity stayed OK.
    "system/etc/aatpe/rules/thread_roles.conf",
    nullptr
};

// USER-CONFIG set. These files are allowed to change at
// runtime (user edits / CLI / WebUI). They are enumerated for diagnostics but do
// NOT feed the tamper chain; a change here is never reported as tampering.
static const char* kUserConfigRelPaths[] = {
    "system/etc/aatpe/aatpe.conf",
    nullptr
};

// Sentinel digest for a missing file: SHA-256("AATPE_MISSING_FILE_v1")
// so a deleted file is a deterministic, fail-closed chain input.
static std::string missingFileSentinel() {
    static std::string s;
    if (s.empty()) {
        const char* sentinel = "AATPE_MISSING_FILE_v1";
        uint8_t d[32];
        sha256OneShot(reinterpret_cast<const uint8_t*>(sentinel),
                      strlen(sentinel), d);
        s = toHexStr(d, 32);
    }
    return s;
}

// Cached last verdict: 1 OK, 0 FAILED, -1 unknown.
// P3-22: g_lastVerdict is atomic-int; g_lastReport is guarded by g_reportMutex
// (verifyModuleIntegrity writes it on the daemon thread; getIntegrityReport reads
// it from the HTTP server thread).
static std::atomic<int> g_lastVerdict{-1};
static std::string g_lastReport;
static std::mutex g_reportMutex;

// ROUND-002 (addendum): explicit module-root override (CLI argv / env).
static std::mutex g_rootMutex;
static std::string g_moduleRootOverride;

// G3-001: fail-hook (wired by the daemon at startup). On a FAILED verify the
// running process immediately revokes privileged capability instead of only
// logging. Guarded by g_reportMutex (same lock that protects verdict writes).
static IntegrityFailHook g_failHook = nullptr;

// ROUND-003: symmetric restore hook + debounce state.
static IntegrityRestoreHook g_restoreHook = nullptr;
// Consecutive definite-tamper runs. Only after N strikes do we latch FAILED and
// fire the fail hook; a single blip (including a genuinely-read mismatch on the
// first notice) still returns false from verify but does NOT latch the read-only
// gate until the next run(s) confirm. TRANSIENT read errors never advance this.
static std::atomic<int> g_consecutiveTamper{0};
static constexpr int kTamperLatchStrikes = 3;

// Test seam: when set, hashFileHex() forces a transient read error (EIO) on this
// absolute path, so the regression tests can exercise the TRANSIENT branch without
// a real fault injection.
static std::string g_injectTransientPath;
#ifdef AATPE_HOST_TEST
void integrityTestInjectTransient(const std::string& absPath) { g_injectTransientPath = absPath; }
void integrityTestClearInject() { g_injectTransientPath.clear(); }
#endif

// N-01: count of real on-disk file hashes performed through hashFileHex(). This
// is the expensive work (tens-of-MB binaries) that must happen exactly once per
// verify. In-memory digests (chain/sentinel/snapshot) go through sha256OneShot
// and are deliberately NOT counted. Exposed only for the hermetic regression test.
static std::atomic<long> g_hashFileCalls{0};
#ifdef AATPE_HOST_TEST
long integrityTestHashFileCount() { return g_hashFileCalls.load(std::memory_order_relaxed); }
void integrityTestResetHashFileCount() { g_hashFileCalls.store(0, std::memory_order_relaxed); }
#endif

// N-01: test-only manifest-path override so a host test can drive the full
// verifyModuleIntegrity() preamble against a hermetic temp tree. Empty = use the
// production default path. Guarded by g_rootMutex (cheap; same lifecycle).
static std::string g_manifestPathOverride;
#ifdef AATPE_HOST_TEST
void setIntegrityManifestPathForTest(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_rootMutex);
    g_manifestPathOverride = path;
}
#endif

// Invoke the wired fail-hook (if any). Called on every FAILED verify so the
// live process hardens immediately, not just at the next boot gate.
static void fireFailHook(const char* reason) {
    IntegrityFailHook h;
    {
        std::lock_guard<std::mutex> lk(g_reportMutex);
        h = g_failHook;
    }
    if (h) h(reason);
}

// ROUND-003: fire the symmetric restore hook on Failed->OK transition.
static void fireRestoreHook(const char* reason) {
    IntegrityRestoreHook h;
    {
        std::lock_guard<std::mutex> lk(g_reportMutex);
        h = g_restoreHook;
    }
    if (h) h(reason);
}

// ============================================================================
// Local file I/O helpers (self-contained, no dependency on aatped_utils)
// ============================================================================

static bool readFileToString(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' '  || out.back() == '\t')) {
        out.pop_back();
    }
    return true;
}

static bool writeStringToFile(const std::string& path, const std::string& content) {
    // P2: 0600 (owner-only) not 0644. The chained manifest must not be world-
    // readable: it lets an offline attacker compute which file they need to
    // replace to match a chosen chain.
    // G3-002: O_NOFOLLOW refuses to write through a symlink planted at the
    // manifest path (defense-in-depth; the dir is 0700 root-only today).
    // O_CLOEXEC keeps the fd out of spawned children.
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    // P3-21: loop write() to handle short writes, then fsync() before close so a
    // crash cannot leave a truncated manifest on disk.
    size_t off = 0;
    size_t len = content.size();
    bool complete = true;
    while (off < len) {
        ssize_t w = write(fd, content.data() + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            complete = false;
            break;
        }
        if (w == 0) { complete = false; break; }  // P3 round7-rework
        off += (size_t)w;
    }
    if (complete && fsync(fd) != 0) complete = false;
    close(fd);
    return complete;
}

// ============================================================================
// Module root resolution
// ============================================================================

std::string resolveModuleRoot() {
    // ROUND-002 (addendum): explicit override wins over auto-probe. This is set
    // by the CLI from argv / AATPE_MODULE_ROOT during install/upgrade so we never
    // accidentally anchor the OLD tree still sitting in modules/ while the new
    // tree is in modules_update. When set, we validate it and return it as-is;
    // if it is invalid we return "" (no silent fallback to auto-probing another
    // tree -- the caller must report the error and exit non-zero).
    {
        std::lock_guard<std::mutex> lk(g_rootMutex);
        if (!g_moduleRootOverride.empty()) {
            std::string binPath = g_moduleRootOverride + "/system/bin";
            struct stat st;
            if (stat(binPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                return g_moduleRootOverride;
            }
            return "";
        }
    }
    const char* candidates[] = {
        "/data/adb/modules/aatpe",
        "/data/adb/ksu/modules/aatpe",
        "/data/adb/ap/modules/aatpe",
        "/data/adb/modules_update/aatpe",
        nullptr
    };
    for (int i = 0; candidates[i]; i++) {
        std::string binPath = std::string(candidates[i]) + "/system/bin";
        struct stat st;
        if (stat(binPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            return candidates[i];
        }
    }
    return "";
}

void setModuleRootOverride(const std::string& root) {
    std::lock_guard<std::mutex> lk(g_rootMutex);
    g_moduleRootOverride = root;
}

// ============================================================================
// File hashing
// ============================================================================

std::string sha256FileHex(const std::string& absPath) {
    int err;
    return hashFileHex(absPath, &err);
}

// ROUND-003: like sha256FileHex but reports the errno of the last failure so the
// caller can distinguish a transient IO error (EIO/EBUSY/EACCES...) from a
// definitive read failure. Returns "" with *errOut set on failure; the hex
// digest on success.
std::string hashFileHex(const std::string& absPath, int* errOut) {
    auto fail = [&](int e) -> std::string { if (errOut) *errOut = e; return std::string(); };
    // N-01: count every real file-hash attempt (success or failure). The
    // regression test asserts a single verify performs exactly one pass over the
    // critical set; this counter is the empirical seam.
    g_hashFileCalls.fetch_add(1, std::memory_order_relaxed);
    // Test seam: simulate a transient read error on the injected path.
    if (!g_injectTransientPath.empty() && absPath == g_injectTransientPath) {
        return fail(EIO);
    }
    // P2-7: close the TOCTOU window. Open with O_NOFOLLOW then fstat() to confirm
    // the fd still points at a regular file before hashing.
    int fd = open(absPath.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return fail(errno);
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return fail(EIO); // unexpected fstat result -> treat as transient-ish
    }

    Sha256Ctx ctx;
    sha256Init(&ctx);

    std::vector<uint8_t> buf(65536);
    ssize_t n;
    while (true) {
        n = read(fd, buf.data(), buf.size());
        if (n > 0) {
            sha256Update(&ctx, buf.data(), static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    int readErr = (n < 0) ? errno : 0;
    close(fd);
    if (n < 0) return fail(readErr);

    uint8_t digest[32];
    sha256Final(&ctx, digest);
    if (errOut) *errOut = 0;
    return toHexStr(digest, 32);
}

// ============================================================================
// Critical file enumeration
// ============================================================================

std::vector<FileEntry> enumerateCriticalFiles(const std::string& moduleRoot) {
    std::vector<FileEntry> out;
    if (moduleRoot.empty()) return out;

    const std::string sentinel = missingFileSentinel();

    for (int i = 0; kCriticalRelPaths[i] != nullptr; i++) {
        FileEntry e;
        e.relPath = kCriticalRelPaths[i];
        e.absPath = moduleRoot + "/" + e.relPath;

        // P1-12: use lstat() (NOT stat()) so a symlink placed at the critical path
        // is NOT transparently followed. ROUND-003: classify into Clean /
        // DefiniteTamper / Transient so a transient IO blip is not mistaken for
        // tampering.
        struct stat st;
        int lstatRc = lstat(e.absPath.c_str(), &st);
        if (lstatRc != 0) {
            // lstat itself failed. ENOENT = file genuinely gone (definite).
            // Any other errno (EIO/EACCES transient on the direntry) = transient.
            e.exists = false;
            e.readState = (errno == ENOENT) ? ReadState::DefiniteTamper
                                           : ReadState::Transient;
        } else if (!S_ISREG(st.st_mode)) {
            // Symlink / directory / device: definite tamper (we can see it).
            e.exists = false;
            e.readState = ReadState::DefiniteTamper;
        } else {
            int herr = 0;
            e.hexDigest = hashFileHex(e.absPath, &herr);
            if (!e.hexDigest.empty()) {
                e.exists = true;
                e.readState = ReadState::Clean;
            } else {
                // We lstat'd a regular file but could not open/read it.
                // Transient errnos -> Transient (do not treat as tamper);
                // anything else -> DefiniteTamper.
                e.exists = false;
                switch (herr) {
                    case EIO: case EINTR: case EAGAIN: case EBUSY:
                    case ENOMEM: case EACCES: case EPERM: case ETIMEDOUT:
                        e.readState = ReadState::Transient;
                        break;
                    default:
                        e.readState = ReadState::DefiniteTamper;
                        break;
                }
            }
        }

        if (e.readState != ReadState::Clean) {
            e.hexDigest = sentinel;
        }
        out.push_back(std::move(e));
    }

    // enumerate the user-config set for diagnostics. These files are allowed to
    // change at runtime; a missing one is logged, never treated as tampering, and
    // they are intentionally NOT fed into the chained manifest (otherwise a user
    // editing aatpe.conf would look like an attack).
    //
    // P1-fix (issue 19): the previous version computed e.absPath/e.hexDigest
    // and then `(void)e;` — all the SHA-256 work was discarded on every verify.
    // We now actually log the digest at DEBUG level so the diagnostics is real,
    // and skip the SHA-256 unless debug logging is enabled (best-effort).
    for (int i = 0; kUserConfigRelPaths[i] != nullptr; i++) {
        std::string absPath = moduleRoot + "/" + kUserConfigRelPaths[i];
        struct stat st;
        bool exists = (lstat(absPath.c_str(), &st) == 0 && S_ISREG(st.st_mode));
        // Only compute the (expensive) SHA-256 if the file is present; the
        // diagnostics log line is informational, not part of the trust chain.
        if (exists) {
#ifdef AATPE_DEBUG
            // F05[P3]: the digest feeds ONLY this LOG_DEBUG line, which compiles out
            // in release. Previously sha256FileHex() ran on EVERY verify() regardless,
            // wasting CPU and incrementing g_hashFileCalls on every pass — which
            // broke the production "verify touches each expected file exactly once"
            // counter test. Only hash when debug logging is actually enabled.
            if (g_logLevel.load(std::memory_order_relaxed) >= 3) {
                std::string digest = sha256FileHex(absPath);
                LOG_DEBUG("integrity: user-config '%s' digest=%s (diagnostics only, not chained)",
                          kUserConfigRelPaths[i], digest.c_str());
            }
#else
            (void)absPath;  // release: no diagnostic hashing on the hot verify path
#endif
        } else {
#ifdef AATPE_DEBUG
            LOG_DEBUG("integrity: user-config '%s' absent (diagnostics only)", kUserConfigRelPaths[i]);
#endif
        }
    }
    return out;
}

// ============================================================================
// Chained manifest computation
//
//   cv_0 = 32 zero bytes
//   for each entry i:
//     block_i = cv_{i-1} || entry.hexDigest_ascii (96 bytes)
//     cv_i    = SHA256(block_i)
//   manifest = hex(cv_N)
// ============================================================================

std::string computeChainedManifest(const std::vector<FileEntry>& entries) {
    uint8_t cv[32];
    memset(cv, 0, sizeof(cv));

    for (const auto& e : entries) {
        // block = cv (32 bytes binary) || hex digest ASCII (64 bytes) = 96 bytes
        uint8_t block[96];
        memcpy(block, cv, 32);
        // G3-006: the chain rule assumes each digest is exactly 64 hex chars.
        // That invariant holds today (sha256FileHex/sentinel both produce 64),
        // but guard against a future shorter/empty digest causing an OOB read
        // from std::string's internal buffer. A malformed entry is treated as a
        // zero digest rather than read out of bounds.
        if (e.hexDigest.size() == 64) {
            memcpy(block + 32, e.hexDigest.data(), 64);
        } else {
            memset(block + 32, 0, 64);
        }

        uint8_t next[32];
        sha256OneShot(block, sizeof(block), next);
        memcpy(cv, next, 32);
    }
    return toHexStr(cv, 32);
}

// ============================================================================
// Manifest persistence
// ============================================================================

std::string manifestPath() {
    // N-01: honor the test-only override (hermetic host tests). Falls through to
    // the production default when no override is set.
    std::lock_guard<std::mutex> lk(g_rootMutex);
    if (!g_manifestPathOverride.empty()) return g_manifestPathOverride;
    return "/data/adb/aatpe/manifest.sha256";
}

std::string readStoredManifest() {
    std::string m;
    if (readFileToString(manifestPath(), m)) {
        return m;
    }
    return "";
}

std::vector<std::string> enumerateCriticalRelPaths() {
    std::vector<std::string> out;
    for (int i = 0; kCriticalRelPaths[i]; i++) out.emplace_back(kCriticalRelPaths[i]);
    return out;
}

bool criticalSetComplete(const std::string& moduleRoot, std::string& firstMissing) {
    auto entries = enumerateCriticalFiles(moduleRoot);
    for (const auto& e : entries) {
        if (!e.exists) {
            firstMissing = e.relPath;
            return false;
        }
    }
    return true;
}

bool generateAndStoreManifest() {
    std::string root = resolveModuleRoot();
    if (root.empty()) {
        LOG_ERROR("integrity: cannot resolve module root for manifest generation");
        return false;
    }
    return generateAndStoreManifestAt(root);
}

bool generateAndStoreManifestAt(const std::string& root) {
    if (root.empty()) {
        LOG_ERROR("integrity: cannot resolve module root for manifest generation");
        return false;
    }
    auto entries = enumerateCriticalFiles(root);
    if (entries.empty()) {
        LOG_ERROR("integrity: no critical files enumerated under %s", root.c_str());
        return false;
    }
    // ROUND-002: defense-in-depth. Refuse to anchor a baseline that bakes in the
    // missing-file sentinel. If any critical entry is absent or not a regular
    // file (symlink/device/dir), the critical list is wrong (or the install is
    // incomplete) — writing the manifest now would persist the sentinel into the
    // baseline and turn a one-off install mistake into a permanent runtime
    // TAMPER. Report and refuse; the operator fixes the list/install, then
    // re-runs --generate-manifest. Verify-side stays fail-closed either way.
    for (const auto& e : entries) {
        if (!e.exists) {
            LOG_ERROR("integrity: refusing to generate baseline — critical entry "
                      "is missing or not a regular file: %s (would otherwise bake the "
                      "missing-file sentinel into the trusted chain)", e.relPath.c_str());
            return false;
        }
    }
    std::string manifest = computeChainedManifest(entries);

    // R8-022 / LED-129: the manifest directory must not be world-listable. The
    // manifest file itself is 0600, but a 0755 dir lets any local non-root user
    // enumerate the module's manifest existence/name. Use 0700 (owner-only),
    // matching the on-device guidance that the manifest must not be world-readable.
    // SEC-06: check mkdir's return value. EEXIST is the normal steady-state case
    // (dir already exists from a prior generate). Any other errno (EACCES on a
    // read-only /data, ENOENT because /data/adb is missing in an early-boot /
    // reduced environment, EROFS) means the subsequent writeStringToFile will
    // fail; log the root cause explicitly so the failure reads as "mkdir: EROFS"
    // rather than the opaque downstream "write manifest failed".
    if (mkdir("/data/adb/aatpe", 0700) != 0 && errno != EEXIST) {
        LOG_ERROR("integrity: mkdir(/data/adb/aatpe, 0700) failed: errno=%d (%s); "
                  "manifest write below will likely fail",
                  errno, strerror(errno));
    }
    std::string mpath = manifestPath();
    if (!writeStringToFile(mpath, manifest)) {
        LOG_ERROR("integrity: failed to write manifest to %s", mpath.c_str());
        return false;
    }
    LOG_INFO("integrity: manifest generated (%zu files, chain=%s...)",
             entries.size(), manifest.substr(0, 16).c_str());
    return true;
}

// ============================================================================
// Full verification cycle
// ============================================================================

// N-01 forward decl: shared classify+debounce+chain-compare core over
// PRE-ENUMERATED entries (defined below). verifyModuleIntegrity() enumerates the
// critical set exactly once and hands the result here, instead of dropping it and
// re-enumerating inside verifyAgainstRootAndManifest().
static bool verifyCore(const std::string& root,
                       const std::string& storedManifest,
                       const std::vector<FileEntry>& entries);

bool verifyModuleIntegrity() {
    std::string root = resolveModuleRoot();
    if (root.empty()) {
        g_lastVerdict = 0;
        {
            std::lock_guard<std::mutex> lk(g_reportMutex);
            g_lastReport = "FATAL: cannot resolve module root";
        }
        writeFile((std::string(STATE_DIR) + "/integrity_status").c_str(),
                  std::string("FAILED"));
        LOG_ERROR("integrity: cannot resolve module root");
        fireFailHook("integrity: cannot resolve module root");
        return false;
    }

    // Read stored manifest.
    // P1-13: DO NOT auto-bootstrap (generate) when the manifest is missing.
    // The previous code generated a fresh manifest on first verify, which an
    // attacker could abuse: install a tampered module, start the daemon once
    // (no manifest present -> daemon writes a baseline over the tampered files),
    // and all subsequent verifications pass against the attacker's own baseline.
    // The trusted baseline is generated ONLY by --generate-manifest, which the
    // installer (customize.sh) invokes during a privileged, trusted install.
    // If a verify ever runs without a stored manifest, that is a trust failure:
    // mark FAILED (fail closed) and let the operator reinstall.
    //
    // P1-fix (issue 18): previously this wrote "DEGRADED" to the state file while
    // setting g_lastVerdict=0 (FAILED). The header contract says consumers MUST
    // treat lastVerdict()==0 (i.e. integrity_status=="FAILED") as fail-closed;
    // writing "DEGRADED" here meant a state-file-only consumer (watchdog/WebUI)
    // would see a recoverable status while the in-memory verdict said hard-fail.
    // Unify on "FAILED" so both signals agree.
    std::string storedManifest = readStoredManifest();
    if (storedManifest.empty()) {
        g_lastVerdict = 0;
        {
            std::lock_guard<std::mutex> lk(g_reportMutex);
            g_lastReport = "FAILED: no stored integrity manifest at " + manifestPath() +
                           " (expected from trusted install; refusing to auto-bootstrap)";
        }
        writeFile((std::string(STATE_DIR) + "/integrity_status").c_str(),
                  std::string("FAILED"));
        LOG_ERROR("integrity: no stored integrity manifest; refusing to auto-bootstrap");
        fireFailHook("integrity: no stored manifest (refusing to auto-bootstrap)");
        return false;
    }

    // N-01: enumerate + hash the critical set EXACTLY once, then hand the results
    // to the shared verify core. Previously this computed `entries` and
    // immediately discarded it; verifyAgainstRootAndManifest() then re-enumerated
    // and re-hashed every critical file a second time (2x SHA-256 over
    // tens-of-MB binaries on every boot gate / hourly re-verify).
    auto entries = enumerateCriticalFiles(root);
    return verifyCore(root, storedManifest, std::move(entries));
}

// Public 2-arg entry point (hermetic tests drive this directly). It enumerates
// once and funnels into the shared core -- never double-hashes.
bool verifyAgainstRootAndManifest(const std::string& root, const std::string& storedManifest) {
    auto entries = enumerateCriticalFiles(root);
    return verifyCore(root, storedManifest, entries);
}

// N-01 core definition: classify + debounce + chain compare over the
// already-computed per-file entries. All security semantics (transient
// passthrough, 3-strike latch, fail/restore hooks, per-file tamper report) are
// unchanged from the previous inline body.
static bool verifyCore(const std::string& root,
                       const std::string& storedManifest,
                       const std::vector<FileEntry>& entries) {
    // ROUND-003: classify the run. TRANSIENT (couldn't read due to a transient
    // errno) must NOT flip a healthy verdict into FAILED nor advance the tamper
    // counter -- a single overlayfs/EIO blip previously latched the daemon into
    // read-only until reboot.
    bool anyTransient = false;
    bool anyDefiniteTamper = false;
    for (const auto& e : entries) {
        if (e.readState == ReadState::Transient) anyTransient = true;
        else if (e.readState == ReadState::DefiniteTamper) anyDefiniteTamper = true;
    }
    if (anyTransient) {
        // Keep the previous verdict; do NOT touch the latch counter or hooks.
        LOG_WARN("integrity: transient read error on >=1 critical file; keeping "
                 "previous verdict (no latch, no hook)");
        return g_lastVerdict.load(std::memory_order_acquire) == 1;
    }

    std::string currentManifest = computeChainedManifest(entries);
    bool chainMatch = hexEqual(currentManifest, storedManifest);
    bool tamper = anyDefiniteTamper || !chainMatch;

    if (!tamper) {
        // OK. Reset the strike counter; if we were previously latched FAILED, this
        // is a recovery -> symmetrically release the read-only latch.
        int prev = g_lastVerdict.exchange(1, std::memory_order_acq_rel);
        g_consecutiveTamper.store(0, std::memory_order_release);
        writeFile((std::string(STATE_DIR) + "/integrity_status").c_str(),
                  std::string("OK"));
        {
            std::lock_guard<std::mutex> lk(g_reportMutex);
            g_lastReport.clear();
        }
        if (prev == 0) {
            LOG_INFO("integrity: recovered from FAILED -> OK; releasing read-only latch");
            fireRestoreHook("integrity: recovered to OK");
        }
        LOG_INFO("integrity: OK (%zu files, chain=%s...)",
                 entries.size(), currentManifest.substr(0, 16).c_str());
        return true;
    }

    // ---- Definite tamper: confirmed hash mismatch, confirmed-missing, or symlink.
    // SEC-02: immediately revoke privileged ops (g_lastVerdict=0) on the FIRST
    // definite tamper. privilegedOpsAllowed() then returns false right away;
    // the 3-strike debounce ONLY controls whether the irreversible fail hook
    // (hmacForceReadonly) fires — to avoid bricking the device on a single
    // transient overlayfs blip that classifies as DefiniteTamper due to e.g.
    // ENOENT during an OTA swap. Transient read errors never reach this code.
    int strikes = g_consecutiveTamper.fetch_add(1, std::memory_order_acq_rel) + 1;
    bool latch = (strikes >= kTamperLatchStrikes);

    // Immediate privilege revocation on every definite tamper (first strike included).
    g_lastVerdict.store(0, std::memory_order_release);
    writeFile((std::string(STATE_DIR) + "/integrity_status").c_str(),
              std::string("FAILED"));

    if (!latch) {
        LOG_WARN("integrity: definite-tamper notice strike %d/%d "
                 "(privileged ops revoked; irreversible read-only latch will engage after %d strikes)",
                 strikes, kTamperLatchStrikes, kTamperLatchStrikes);
        return false;
    }

    // Latched (strikes >= 3): build per-file report and fire the irreversible fail hook.
    std::ostringstream rep;
    rep << "INTEGRITY TAMPER DETECTED\n";
    rep << "module_root=" << root << "\n";
    rep << "stored_chain="  << storedManifest << "\n";
    rep << "current_chain=" << currentManifest << "\n";
    rep << "files_scanned="  << entries.size() << "\n";
    rep << "--- per-file digest ---\n";
    int tamperedCount = 0;
    for (const auto& e : entries) {
        bool fileTampered = (e.readState != ReadState::Clean);
        rep << e.relPath << " | "
            << (e.readState == ReadState::Clean ? "present" : "MISSING") << " | "
            << e.hexDigest;
        if (fileTampered) {
            rep << " | TAMPERED";
            tamperedCount++;
        }
        rep << "\n";
    }
    rep << "tampered_files=" << tamperedCount << "\n";
    {
        std::lock_guard<std::mutex> lk(g_reportMutex);
        g_lastReport = rep.str();
    }

    LOG_ERROR("integrity: TAMPER DETECTED stored=%s current=%s tampered=%d",
              storedManifest.substr(0, 16).c_str(),
              currentManifest.substr(0, 16).c_str(),
              tamperedCount);
    for (const auto& e : entries) {
        if (e.readState != ReadState::Clean) {
            LOG_ERROR("integrity: MISSING file: %s", e.relPath.c_str());
        }
    }
    fireFailHook("integrity: TAMPER DETECTED (chain mismatch, latched)");
    return false;
}

// ---------------------------------------------------------------------------
// G3-001: boot gate / capability revocation APIs.
// ---------------------------------------------------------------------------
IntegrityVerdict verdict() {
    int v = g_lastVerdict.load(std::memory_order_acquire);
    if (v == 1) return IntegrityVerdict::Ok;
    if (v == 0) return IntegrityVerdict::Failed;
    return IntegrityVerdict::Unknown;
}

bool privilegedOpsAllowed() {
    // fail-closed: only a confirmed OK grants privileged capability.
    return g_lastVerdict.load(std::memory_order_acquire) == 1;
}

void setIntegrityFailHook(IntegrityFailHook hook) {
    std::lock_guard<std::mutex> lk(g_reportMutex);
    g_failHook = hook;
}

void setIntegrityRestoreHook(IntegrityRestoreHook hook) {
    std::lock_guard<std::mutex> lk(g_reportMutex);
    g_restoreHook = hook;
}

bool runBootGate() {
    bool ok = verifyModuleIntegrity();
    // verifyModuleIntegrity already fires the hook on failure; this wrapper just
    // centralises the boot/runtime call site and reflects the verdict.
    return ok;
}

#ifdef AATPE_HOST_TEST
void setIntegrityVerdictForTest(IntegrityVerdict v) {
    // TEST-ONLY: bypass manifest compare for hermetic/integration tests.
    g_lastVerdict.store(static_cast<int>(v), std::memory_order_release);
}
#endif

std::string getIntegrityReport() {
    std::lock_guard<std::mutex> lk(g_reportMutex);
    return g_lastReport;
}

int lastVerdict() {
    return g_lastVerdict.load(std::memory_order_relaxed);
}

// ============================================================================
// Snapshot self-verification helpers
// ============================================================================

// R8-015 / LED-122: strip a trailing CR (and stray trailing whitespace) from a
// parsed field. A .kv file written over CRLF (Windows editor / adb push) would
// otherwise carry a '\r' into the hashed value and never match the stored sha256.
static void stripTrailingCr(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                          s.back() == ' '  || s.back() == '\t')) {
        s.pop_back();
    }
}

// SEC-07: parse a .kv snapshot file into its (value_b64, sha256) pair. A
// hand-edited / tampered .kv file may carry more than one "value_b64=" line.
// The producer (snapshotValueSha256) and the verifier (verifySnapshotFile)
// MUST agree on which line wins, otherwise a multi-line file yields a digest
// the verifier cannot reproduce. We take the LAST occurrence of each key
// (append-style rewrite: the producer writes the newest value last), which
// matches the historical verifySnapshotFile behavior; snapshotValueSha256 used
// to break on the FIRST line and would hash a stale value on a multi-line file.
// Returns false if the file cannot be opened; out params are cleared first.
static bool parseSnapshotKv(const std::string& kvFilePath,
                             std::string& outValueB64,
                             std::string& outStoredSha) {
    std::ifstream ifs(kvFilePath, std::ios::binary);
    if (!ifs.is_open()) return false;
    std::string line;
    outValueB64.clear();
    outStoredSha.clear();
    while (std::getline(ifs, line)) {
        if (line.rfind("value_b64=", 0) == 0) {
            outValueB64 = line.substr(10);   // last wins
        } else if (line.rfind("sha256=", 0) == 0) {
            outStoredSha = line.substr(7);    // last wins
        }
    }
    stripTrailingCr(outValueB64);
    stripTrailingCr(outStoredSha);
    return true;
}

std::string snapshotValueSha256(const std::string& kvFilePath) {
    std::string b64Value, storedSha;
    if (!parseSnapshotKv(kvFilePath, b64Value, storedSha)) return "";
    if (b64Value.empty()) return "";

    uint8_t d[32];
    sha256OneShot(reinterpret_cast<const uint8_t*>(b64Value.data()),
                  b64Value.size(), d);
    return toHexStr(d, 32);
}

bool verifySnapshotFile(const std::string& kvFilePath) {
    std::string b64Value, storedSha;
    if (!parseSnapshotKv(kvFilePath, b64Value, storedSha)) return false;
    if (b64Value.empty() || storedSha.empty()) return false;

    uint8_t d[32];
    sha256OneShot(reinterpret_cast<const uint8_t*>(b64Value.data()),
                  b64Value.size(), d);
    std::string actual = toHexStr(d, 32);

    return hexEqual(actual, storedSha);
}

} // namespace integrity
} // namespace aatpe
