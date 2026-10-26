// AATPE - short-lived session token store (#61). See aatpe_session_token.h.
#include "aatpe_session_token.h"
#include "aatped_logging.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <mutex>
#include <sys/mman.h>
// Session token TTL: 5 minutes. Chosen so a long-lived WebUI page re-auths
// roughly once per 5 min window — frequent enough that a leaked token is short
// lived, rare enough that the HMAC signing path stays the exception rather than
// the per-command cost.
#define AATPE_SESSION_TTL_SEC 300

namespace {

// Max concurrent bearer sessions. 8 is far above any real WebUI (typically 1);
// kept identical to the historical std::map cap.
constexpr size_t kMaxSessions = 8;

// One live bearer credential. Token material lives in this fixed struct (in
// BSS) rather than in heap std::map nodes, so we can mlock ONLY this small
// region instead of the whole process heap (see mlockSessionsOnce()).
struct SessionSlot {
    bool   used;
    time_t expiry;        // CLOCK_MONOTONIC seconds; 0 when unused.
    char   token[65];     // NUL-terminated 64 lowercase hex (256-bit).
};

std::mutex g_sessionMutex;
// Fixed slot table: ~600 bytes in BSS. This is the ONLY memory that holds live
// bearer tokens; it is locked (mlock) once at first issue.
SessionSlot g_slots[kMaxSessions];

#ifdef AATPE_HOST_TEST
// SEC-04 test seam: when non-zero, readMonotonicSec() pretends clock_gettime
// failed. Only ever flipped by host unit tests; absent from release builds.
static int g_forceClockFailForTest = 0;
#endif

// Read monotonic seconds since boot (unaffected by wall-clock settimeofday
// jumps). CLOCK_MONOTONIC is a vDSO clock on Linux/Android and cannot
// realistically fail for a valid timespec pointer, but we treat the failure
// path as a security event rather than silently falling back to wall clock.
//
// SEC-04 (P3, fail-closed): the old code did `return time(nullptr)` on
// clock_gettime(CLOCK_MONOTONIC) failure. That wall-clock fallback is unsafe
// for bearer-session TTL: an NTP / settimeofday backward jump would make an
// unexpired token look expired (availability) or — worse — let a token
// whose expiry was stamped against monotonic now be compared against a wall
// clock that has run backwards, keeping an expired token accepted past its
// real TTL. The two clocks are not comparable and mixing them defeats the
// 300 s bearer window.
//
// Correct fail-closed semantics: if we cannot obtain a trustworthy monotonic
// clock, we cannot enforce TTL. We return false and let every public entry
// point (issue / verify / anyActive) degrade by refusing to mint or accept
// sessions rather than minting tokens with no enforceable expiry. This path
// is unreachable on a stock Android kernel; it exists so a hostile seccomp /
// broken vDSO cannot silently downgrade session auth to "no expiry".
bool readMonotonicSec(time_t& out) {
#ifdef AATPE_HOST_TEST
    // SEC-04 test seam: host tests force CLOCK_MONOTONIC to "fail" so the
    // fail-closed branch (issue rejects, verify rejects, anyActive=false) is
    // reachable without a hostile seccomp. Compiled out of release builds.
    if (g_forceClockFailForTest) {
        errno = EIO;
        return false;
    }
#endif
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        LOG_ERROR("SESSION: CLOCK_MONOTONIC failed (errno=%d); failing closed "
                  "(refusing to mint/verify bearer tokens until clock recovers)",
                  errno);
        return false;
    }
    out = (time_t)ts.tv_sec;
    return true;
}

// P3-25 / audit2: the session tokens are bearer credentials and must never be
// paged out to zram/swap, where a reader with disk access could recover live
// tokens. The historical implementation used mlockall(MCL_CURRENT|MCL_FUTURE),
// which on a low-RAM device pins the ENTIRE daemon heap (current pages AND every
// future anonymous allocation) — that risks RLIMIT_MEMLOCK failures and raises
// OOM pressure for no security benefit: only the ~600-byte token table actually
// holds credential material.
//
// Narrowed fix: keep the token table in a fixed BSS slot table and mlock just
// that one region (one 4 KiB page). This preserves the exact security property —
// "pages holding live bearer tokens are never swapped out" — without locking
// the rest of the heap. Best-effort: mlock may fail if the process lacks
// CAP_IPC_LOCK / hits RLIMIT_MEMLOCK; we log and continue (tokens are still
// 5-min lived and constant-time compared).
//
// P1-fix (issue 15): std::once_flag so two concurrent first-issuers cannot both
// mlock (happens-before edge).
static std::once_flag g_mlockOnce;
static void mlockSessionsOnce() {
    std::call_once(g_mlockOnce, [](){
        if (mlock(g_slots, sizeof(g_slots)) != 0) {
            // Non-fatal; best-effort hardening.
        }
    });
}

// Overwrite a slot so stale bearer bytes do not linger in the locked page after
// the session ends (eviction, expiry, purge). The volatile write prevents the
// optimizer from eliding the zeroing. Caller must hold g_sessionMutex.
void zeroBytesVolatile(void* p, size_t n) {
    volatile char* vc = static_cast<volatile char*>(p);
    for (size_t i = 0; i < n; i++) vc[i] = 0;
}
void clearSlotLocked(SessionSlot& s) {
    s.used = false;
    s.expiry = 0;
    zeroBytesVolatile(s.token, sizeof(s.token));
}

// Remove expired entries in place. Caller must hold g_sessionMutex.
void purgeExpiredLocked(time_t now) {
    for (auto& s : g_slots) {
        if (s.used && s.expiry <= now) clearSlotLocked(s);
    }
}

// 256-bit random token from /dev/urandom, hex-encoded. Returns "" on failure.
std::string generateTokenHex(void) {
    unsigned char buf[32];
    FILE* fp = fopen("/dev/urandom", "rb");
    if (!fp) return "";
    // P2: fread() may return short even on /dev/urandom (e.g. a signal interrupted
    // the underlying read loop). Loop until all 32 bytes are collected; bail only
    // on a hard EOF/error.
    size_t off = 0;
    while (off < sizeof(buf)) {
        size_t rd = fread(buf + off, 1, sizeof(buf) - off, fp);
        if (rd == 0) { fclose(fp); return ""; }
        off += rd;
    }
    fclose(fp);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + size_t(i) * 2, 3, "%02x", buf[i]);
    hex[64] = '\0';
    return std::string(hex);
}

} // namespace

std::string aatpeSessionIssue(void) {
    mlockSessionsOnce();
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    time_t now;
    if (!readMonotonicSec(now)) return "";  // SEC-04: fail-closed, cannot mint TTL
    purgeExpiredLocked(now);
    std::string tok = generateTokenHex();
    if (tok.empty()) {
        LOG_ERROR("SESSION: failed to generate token from /dev/urandom");
        return "";
    }
    // Find a free slot; if none is free, evict the OLDEST-expiry live slot FIRST
    // so the freshly-issued token is guaranteed a slot (mirrors the old map cap
    // behavior). Evicting before insert also keeps the new token from being the
    // one dropped.
    SessionSlot* target = nullptr;
    for (auto& s : g_slots) {
        if (!s.used) { target = &s; break; }
    }
    if (target == nullptr) {
        SessionSlot* oldest = &g_slots[0];
        for (auto& s : g_slots) {
            if (s.expiry < oldest->expiry) oldest = &s;
        }
        target = oldest;
        clearSlotLocked(*target);
    }
    target->used = true;
    target->expiry = now + AATPE_SESSION_TTL_SEC;
    snprintf(target->token, sizeof(target->token), "%s", tok.c_str());
    size_t active = 0;
    for (const auto& s : g_slots) if (s.used) ++active;
    LOG_INFO("SESSION: issued new token (active=%zu)", active);
    return tok;
}

bool aatpeSessionVerify(const std::string& token) {
    if (token.empty()) return false;
    if (token.size() != 64) return false;  // tokens are fixed 64 hex
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    time_t now;
    if (!readMonotonicSec(now)) return false;  // SEC-04: fail-closed, no TTL => reject
    purgeExpiredLocked(now);
    // P2: constant-time membership test. Scan EVERY slot and fold all byte
    // comparisons with XOR accumulation (never early-return on a match) so the
    // wall time does not depend on how many leading bytes matched.
    //
    // v4.4.9 R3-F06: unused slots MUST also participate in the 64-byte XOR. The
    // old `if (!s.used) continue;` skipped the comparison for empty slots, so the
    // wall time varied with the number of LIVE sessions (a slot-state timing
    // oracle). Unused slots have their token bytes zeroed (clearSlotLocked) and
    // expiry==0; folding them in changes nothing functionally (they can never set
    // live=true because of the s.used / expiry>now gates) but makes the per-slot
    // work — and thus the total — independent of how many slots are populated.
    unsigned char d = 0;
    bool live = false;
    for (const auto& s : g_slots) {
        unsigned char e = 0;
        for (size_t i = 0; i < 64; i++) {
            unsigned char a = (i < token.size()) ? (unsigned char)token[i] : 0;
            unsigned char b = (i < sizeof(s.token)) ? (unsigned char)s.token[i] : 0;
            e |= (unsigned char)(a ^ b);
        }
        // Only an actively-used, unexpired slot can authenticate. Unused slots
        // (zeroed token, expiry==0) still folded into `e`/`d` above but can
        // never reach live=true.
        if (e == 0 && s.used && s.expiry > now) live = true;
        d |= e;  // fold every slot; keep scanning all entries (no early break).
    }
    (void)d;
    return live;
}

bool aatpeSessionAnyActive(void) {
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    time_t now;
    if (!readMonotonicSec(now)) return false;  // SEC-04: clock broken => show no active
    purgeExpiredLocked(now);
    for (const auto& s : g_slots) if (s.used) return true;
    return false;
}

void aatpeSessionPurgeAll(void) {
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    for (auto& s : g_slots) clearSlotLocked(s);
}

#ifdef AATPE_HOST_TEST
// SEC-04 test seam: force readMonotonicSec() to report clock failure (on=true)
// or resume normal operation (on=false). Release builds never reference this.
void aatpeSessionSetClockFailForTest(bool on) {
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    g_forceClockFailForTest = on ? 1 : 0;
}
#endif
