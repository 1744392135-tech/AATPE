// AATPE - HMAC key management / SHA256 signature (extracted from aatped_main.cpp)
#include "aatped.h"
#include "aatpe_crypto.h"      // LED-125 (R8-018): canonical SHA-256/HMAC-SHA256.
                               // Eliminates the third private copy (this file used to
                               // carry its own sha256_transform/sha256/hmac_sha256, which
                               // had a signed-shift UB (R8-006) that had not been synced to
                               // the other two copies). All crypto now routes through the
                               // single inline implementation in aatpe_crypto.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

using namespace aatpe;

// P2-9: best-effort secure zero that the compiler cannot dead-store-eliminate.
// Volatile pointer writes are guaranteed not to be optimized away; used to scrub
// stack-resident key material before the function returns.
static void secureZero(void* p, size_t n) {
    volatile unsigned char* vp = (volatile unsigned char*)p;
    while (n--) *vp++ = 0;
}

// P2: HMAC key management (random generated, persisted, rotatable)
// Increased key size from 64 to 128 bytes for 508-bit entropy after hex encoding
char g_hmacKey[128] = {0};
std::string g_hmacKeyStatus = "uninitialized";
bool g_hmacActive = false;

// P23: secure-by-default; overwritten by loadConfig() from HMAC_REQUIRED (aatpe.conf).
bool g_hmacRequired = true;

// P1-4: mutex protecting g_hmacKey / g_hmacActive / g_hmacKeyLoaded /
// g_hmacKeyGeneratedAt / g_hmacReadonly.
std::mutex g_hmacMutex;

// P1-5: fail-closed read-only mode (set when key init fails).
// P2-1 (crash-loop hardening): DEFAULT fail-closed. Until initHmacKey() runs and
// either loads an existing key or generates+persists a fresh one, the daemon stays
// read-only. initHmacKey() explicitly clears this on success (both the load and the
// first-install generate paths), so first-install enrollment is NOT blocked — it
// just can't happen silently-writable before auth material exists.
bool g_hmacReadonly = true;

#ifdef AATPE_HOST_TEST
// Host-test seam: when set, generateHmacKey() fails, exercising the fail-closed
// branch (g_hmacReadonly stays true). Compiled out on device.
bool g_forceHmacGenFail = false;
#endif

bool isHmacReadonly(void) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    return g_hmacReadonly;
}

// G3-001: harden the running process at runtime. The integrity module calls
// this (via its fail-hook) the moment a re-verify detects tampering, so the
// daemon stops accepting privileged /control writes immediately instead of
// only logging. It never clears an existing readonly state (one-way latch).
void hmacForceReadonly(const char* reason) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    if (!g_hmacReadonly) {
        g_hmacReadonly = true;
        LOG_ERROR("HMAC: forced READ-ONLY by integrity/security gate: %s",
                  reason ? reason : "(unspecified)");
    }
}

// ROUND-003: symmetric release. Called by the integrity restore-hook when a
// previously-latched FAILED verdict recovers to OK, so the device self-heals
// without a reboot.
void hmacClearReadonly(const char* reason) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    if (g_hmacReadonly) {
        g_hmacReadonly = false;
        LOG_INFO("HMAC: READ-ONLY released (integrity recovered): %s",
                 reason ? reason : "(unspecified)");
    }
}

// P3-19: thread-safe snapshot of g_hmacKeyStatus.
std::string getHmacKeyStatus(void) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    return g_hmacKeyStatus;
}

static bool g_hmacKeyLoaded = false;
static time_t g_hmacKeyGeneratedAt = 0;
static const int HMAC_ROTATE_INTERVAL_SEC = 7 * 24 * 3600; // 7 days

// P2: Generate 64-byte random key from /dev/urandom
static bool generateHmacKey(char* outKey, size_t keyLen) {
    // keyLen is the total buffer size; reserve the last byte for the NUL
    // terminator so we produce exactly (keyLen-1) hex chars. The buffer must
    // be at least 65 bytes (64 hex chars + NUL).
    if (keyLen < 65) return false;
#ifdef AATPE_HOST_TEST
    if (g_forceHmacGenFail) return false;  // test seam: simulate urandom failure
#endif
    size_t keyBytes = keyLen - 1;  // one byte reserved for '\0'
    FILE* fp = fopen("/dev/urandom", "r");
    if (!fp) return false;
    size_t read = fread(outKey, 1, keyBytes, fp);
    fclose(fp);
    if (read != keyBytes) return false;
    // Ensure key is printable ASCII (hex) for safe storage
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < keyBytes; i++) {
        outKey[i] = hex[(unsigned char)outKey[i] % 16];
    }
    outKey[keyBytes] = '\0';
    return true;
}

// P2: Save key to persistent storage (0600 permissions).
// Key file format is now "<key>\n<created_at_unix_ts>\n".
// /dev/aatpe is tmpfs (wiped on reboot), so ALWAYS mirror the key
// to the persistent module path /data/adb/modules/aatpe/hmac_key as well. The old
// code returned after the first successful write (the tmpfs one), meaning the
// persistent copy was never created; after reboot loadHmacKey() found no key and
// generated a NEW one, which silently desynced the WebUI's cached signing key
// from the daemon -> every command then failed HMAC verification (403).
// persistent HMAC key path resolved through the single canonical
// module dir (Magisk / KernelSU / APatch), never hardcoded to
// /data/adb/modules/aatpe. getModuleDir() picks the live module root.
static std::string persistentHmacKeyPath() {
    return getModuleDir() + "/hmac_key";
}

static bool saveHmacKey(const char* key, time_t createdAt) {
    std::string persistent = persistentHmacKeyPath();
    const char* paths[] = {
        "/dev/aatpe/hmac_key",
        persistent.c_str(),
        nullptr
    };
    std::string payload = key;
    payload += "\n";
    payload += std::to_string((long long)createdAt);
    payload += "\n";
    // SEC-01: track per-path success. Index 0 = tmpfs (ephemeral), index 1 =
    // persistent (survives reboot). The persistent write is mandatory: if it
    // fails, returning true would cause the daemon to believe the key is durably
    // stored, but on reboot loadHmacKey finds no persistent key and generates a
    // new one — silently desyncing all clients (403 on every command).
    bool okTmpfs = false, okPersistent = false;
    for (int i = 0; paths[i]; i++) {
        // G3-002: O_NOFOLLOW refuses to write through a symlink planted at the key
        // path; O_CLOEXEC keeps the credential fd out of spawned children.
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            size_t len = payload.size();
            size_t off = 0;
            bool complete = true;
            while (off < len) {
                ssize_t w = write(fd, payload.c_str() + off, len - off);
                if (w <= 0) {
                    if (errno == EINTR) continue;
                    complete = false;
                    break;
                }
                off += (size_t)w;
            }
            if (complete && fsync(fd) != 0) complete = false;
            close(fd);
            if (i == 0) okTmpfs = complete;
            else        okPersistent = complete;
        }
    }
    if (!okPersistent) {
        LOG_ERROR("HMAC key: persistent write FAILED (%s); key will not survive reboot",
                  persistent.c_str());
        return false;
    }
    if (!okTmpfs) {
        LOG_WARN("HMAC key: tmpfs write failed (persistent OK; current session still works)");
    }
    return true;
}

// P2: Load key from persistent storage.
static bool loadHmacKey(char* outKey, size_t keyLen, time_t* outCreatedAt) {
    std::string persistent = persistentHmacKeyPath();
    const char* paths[] = {
        "/dev/aatpe/hmac_key",
        persistent.c_str(),
        nullptr
    };
    for (int i = 0; paths[i]; i++) {
        // G3-002: O_NOFOLLOW refuses to load a key through a symlink (prevents
        // loading an attacker-controlled key material planted at the key path).
        int fd = open(paths[i], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) continue;
        // P2: 1024-byte read buffer (was 512). Key is 128 chars + timestamp +
        // newlines, well under 512, but a future rotation that bumps keyLen
        // would otherwise silently truncate the parse.
        char buf[1024];
        ssize_t n;
        do {
            n = read(fd, buf, sizeof(buf) - 1);
        } while (n < 0 && errno == EINTR);
        struct stat st;
        time_t mtime = 0;
        if (fstat(fd, &st) == 0) mtime = st.st_mtime;
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string content(buf, n);
        size_t nl1 = content.find('\n');
        std::string keyStr;
        std::string tsStr;
        if (nl1 == std::string::npos) {
            keyStr = content;
        } else {
            keyStr = content.substr(0, nl1);
            size_t nl2 = content.find('\n', nl1 + 1);
            if (nl2 != std::string::npos) {
                tsStr = content.substr(nl1 + 1, nl2 - nl1 - 1);
            } else {
                tsStr = content.substr(nl1 + 1);
            }
        }
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' '||s.back()=='\t')) s.pop_back();
            while (!s.empty() && (s.front()==' '||s.front()=='\t'||s.front()=='\r')) s.erase(s.begin());
        };
        trim(keyStr);
        trim(tsStr);
        if (keyStr.size() < 64 || keyStr.size() >= keyLen) continue;
        memcpy(outKey, keyStr.c_str(), keyStr.size());
        outKey[keyStr.size()] = '\0';
        time_t created = 0;
        if (!tsStr.empty()) {
            try { created = (time_t)std::stoll(tsStr); } catch (...) { created = 0; }
        }
        if (created <= 0) created = mtime;
        if (outCreatedAt) *outCreatedAt = created;
        return true;
    }
    return false;
}

// P2: Initialize HMAC key (load existing or generate new)
void initHmacKey(void) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    time_t loadedAt = 0;
    if (loadHmacKey(g_hmacKey, sizeof(g_hmacKey), &loadedAt)) {
        g_hmacKeyLoaded = true;
        g_hmacKeyGeneratedAt = (loadedAt > 0) ? loadedAt : time(nullptr);
        g_hmacActive = g_hmacRequired;
        g_hmacKeyStatus = "loaded";
        g_hmacReadonly = false;
        LOG_INFO("HMAC key: loaded from persistent storage (len=%zu, age=%ld days, active=%d)",
                 strlen(g_hmacKey),
                 (long)(time(nullptr) - g_hmacKeyGeneratedAt) / 86400,
                 (int)g_hmacActive);
    } else {
        if (generateHmacKey(g_hmacKey, sizeof(g_hmacKey))) {
            time_t now = time(nullptr);
            if (!saveHmacKey(g_hmacKey, now)) LOG_WARN("HMAC key: failed to persist to disk");
            g_hmacKeyLoaded = true;
            g_hmacKeyGeneratedAt = now;
            g_hmacActive = g_hmacRequired;
            g_hmacKeyStatus = "generated";
            g_hmacReadonly = false;
            LOG_INFO("HMAC key: generated new random key (len=%zu, active=%d)",
                     strlen(g_hmacKey), (int)g_hmacActive);
        } else {
            g_hmacKey[0] = '\0';
            g_hmacKeyLoaded = false;
            g_hmacActive = false;
            g_hmacKeyStatus = "disabled";
            // P1-5: fail-CLOSED — cannot establish authentication material.
            // Enter read-only mode: /control write commands will be rejected
            // regardless of the hmacRequired config flag. Previously this set
            // g_hmacActive=false but left g_hmacRequired untouched, meaning if
            // the user had set hmacRequired=false in aatpe.conf ALL write
            // commands went through with no authentication at all.
            g_hmacReadonly = true;
            LOG_ERROR("HMAC key: CRITICAL - failed to generate from /dev/urandom, "
                      "HMAC verification DISABLED, entering READ-ONLY mode "
                      "(all /control write commands rejected)");
        }
    }
    // W-013: once the key material is resident, mlock() it so the secret is never
    // paged out to swap / zram. mlock on an already-locked range is a no-op success;
    // rotation (checkHmacKeyRotation) reuses the same buffer so the lock persists.
    // On exit the daemon does explicit_bzero + munlock in the shutdown path.
    if (g_hmacKeyLoaded) {
        if (mlock(g_hmacKey, sizeof(g_hmacKey)) != 0) {
            LOG_WARN("HMAC key: mlock(g_hmacKey) failed (%s); key may be swappable",
                     aatpeErrStr(errno));
        }
    }
}

// P2: Check if key needs rotation (7 days)
void checkHmacKeyRotation(void) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    if (!g_hmacKeyLoaded) return;
    time_t now = time(nullptr);
    if (now - g_hmacKeyGeneratedAt > HMAC_ROTATE_INTERVAL_SEC) {
        LOG_INFO("HMAC key: rotating (age=%ld days)", (long)(now - g_hmacKeyGeneratedAt) / 86400);
        // P1-10: generate into a TEMP buffer first; only commit into g_hmacKey
        // after the new key is fully persisted. If save fails, keep the OLD key
        // in memory (it is still on disk and clients still have it) rather than
        // swapping to an un-persisted key that would desync every client.
        char newKey[128];
        if (generateHmacKey(newKey, sizeof(newKey))) {
            if (!saveHmacKey(newKey, now)) {
                LOG_WARN("HMAC key: rotation FAILED to persist; keeping old key");
            } else {
                // R8-016 / LED-123: explicitly scrub the old key material before the
                // new key is committed. The memcpy below overwrites the buffer, but an
                // explicit secureZero guarantees the previous secret is not left
                // recoverable in the mlocked region (e.g. if the copy is ever partial
                // or a future change shrinks the key), and documents intent.
                secureZero(g_hmacKey, sizeof(g_hmacKey));
                memcpy(g_hmacKey, newKey, sizeof(g_hmacKey));
                g_hmacKeyGeneratedAt = now;
                g_hmacKeyStatus = "rotated";
                LOG_INFO("HMAC key: rotated successfully (active=%d)", (int)g_hmacActive);
            }
            // P2-9: scrub the stack-resident new key before returning. The comment
            // in initHmacKey() claimed "exit cleanup" but nothing actually zeroed
            // this buffer — it stayed on the thread stack until reused.
            secureZero(newKey, sizeof(newKey));
        }
    }
}

// P2-9: shutdown-time scrub. Called by the daemon shutdown path (after stopHttpServer,
// before exit) to zero g_hmacKey and munlock() the locked region so the long-term
// signing key does not remain resident (and mlocked) after the daemon stops.
void shutdownHmacKey(void) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    secureZero(g_hmacKey, sizeof(g_hmacKey));
    munlock(g_hmacKey, sizeof(g_hmacKey));
    g_hmacKeyStatus = "shutdown";
    g_hmacActive = false;
}

// LED-125 (R8-018): the local sha256_transform / sha256 / hmac_sha256 copies were
// removed. The canonical implementations live in aatpe_crypto.h and are used
// directly by computeCommandHmac() below. (The removed sha256_transform also
// contained R8-006 / LED-113: `block[i*4]<<24` on a signed int — UB when the high
// bit was set; the canonical copy always casts to uint32_t before shifting.)

std::string computeCommandHmac(const char* command) {
    // SEC-03: defensive NULL check. strlen(NULL) would SIGSEGV.
    if (!command) return "";
    // P1-4: snapshot the key under g_hmacMutex so the HTTP server thread
    // cannot observe a half-rotated key from checkHmacKeyRotation().
    char keySnap[128];
    {
        std::lock_guard<std::mutex> lk(g_hmacMutex);
        memcpy(keySnap, g_hmacKey, sizeof(g_hmacKey));
    }
    unsigned char out[32];
    aatpe_crypto::hmacSha256(reinterpret_cast<const uint8_t*>(keySnap), strlen(keySnap),
                reinterpret_cast<const uint8_t*>(command), strlen(command), out);
    // P2-9: scrub the stack copy of the key before returning.
    secureZero(keySnap, sizeof(keySnap));
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + size_t(i)*2, 3, "%02x", out[i]);
    hex[64] = 0;
    return std::string(hex);
}

bool verifyCommandHmac(const char* command, const char* signature) {
    // SEC-03: defensive NULL check on command (would crash strlen in computeCommandHmac).
    if (!command) return false;
    // P3-18: fail-CLOSED on an empty signature. The old code returned !required
    // (i.e. accepted when hmacRequired=false), which is a fail-open trap: an empty
    // signature must NEVER verify, regardless of the required flag. If hmac is not
    // required, callers simply don't call this; an empty/missing signature here is
    // always a rejection.
    if (!signature || signature[0] == '\0') return false;
    std::string expected = computeCommandHmac(command);
    // R8-008 / LED-115: do NOT short-circuit on length mismatch (that leaks the
    // expected signature length through timing). Pad both operands to the fixed
    // digest-hex width (64 chars) and compare over the full width in constant time,
    // so the comparison always does the same amount of work regardless of where the
    // mismatch (or the differing length) is.
    constexpr size_t kHexLen = 64;
    // Bounded length scan over up to 256 bytes. We need the TRUE length of the
    // provided signature (not just the first kHexLen chars) so that an over-long
    // signature — e.g. a captured valid 64-char signature with attacker-controlled
    // trailing garbage — is rejected. The scan is bounded to prevent an unbounded
    // strlen on a non-null-terminated malicious buffer.
    size_t siglen = 0;
    while (siglen < 256 && signature[siglen] != '\0') ++siglen;
    unsigned char diff = 0;
    // Reject length mismatches in constant time: if the signature is not exactly
    // kHexLen chars, fold a non-zero byte into diff. We do NOT early-return here
    // because that would leak the expected length through timing; instead we still
    // run the full kHexLen-byte comparison below and OR in the length-mismatch flag.
    if (siglen != kHexLen) diff |= 0xFF;
    for (size_t i = 0; i < kHexLen; i++) {
        char e = (i < expected.size()) ? expected[i] : 0;
        char s = (i < siglen) ? signature[i] : 0;   // zero-padded, never OOB
        diff |= (unsigned char)(e ^ s);
    }
    return diff == 0;
}
