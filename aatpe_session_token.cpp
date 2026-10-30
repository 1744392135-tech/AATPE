#include "aatpe_session_token.h"
#include "aatped_logging.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <mutex>
#include <sys/mman.h>
#define AATPE_SESSION_TTL_SEC 300

namespace {

constexpr size_t kMaxSessions = 8;

struct SessionSlot {
    bool   used;
    time_t expiry;
    char   token[65];
};

std::mutex g_sessionMutex;
SessionSlot g_slots[kMaxSessions];

#ifdef AATPE_HOST_TEST
static int g_forceClockFailForTest = 0;
#endif

bool readMonotonicSec(time_t& out) {
#ifdef AATPE_HOST_TEST
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

static std::once_flag g_mlockOnce;
static void mlockSessionsOnce() {
    std::call_once(g_mlockOnce, [](){
        if (mlock(g_slots, sizeof(g_slots)) != 0) {
        }
    });
}

void zeroBytesVolatile(void* p, size_t n) {
    volatile char* vc = static_cast<volatile char*>(p);
    for (size_t i = 0; i < n; i++) vc[i] = 0;
}
void clearSlotLocked(SessionSlot& s) {
    s.used = false;
    s.expiry = 0;
    zeroBytesVolatile(s.token, sizeof(s.token));
}

void purgeExpiredLocked(time_t now) {
    for (auto& s : g_slots) {
        if (s.used && s.expiry <= now) clearSlotLocked(s);
    }
}

std::string generateTokenHex(void) {
    unsigned char buf[32];
    FILE* fp = fopen("/dev/urandom", "rb");
    if (!fp) return "";
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

}

std::string aatpeSessionIssue(void) {
    mlockSessionsOnce();
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    time_t now;
    if (!readMonotonicSec(now)) return "";
    purgeExpiredLocked(now);
    std::string tok = generateTokenHex();
    if (tok.empty()) {
        LOG_ERROR("SESSION: failed to generate token from /dev/urandom");
        return "";
    }
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
    if (token.size() != 64) return false;
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    time_t now;
    if (!readMonotonicSec(now)) return false;
    purgeExpiredLocked(now);
    unsigned char d = 0;
    bool live = false;
    for (const auto& s : g_slots) {
        unsigned char e = 0;
        for (size_t i = 0; i < 64; i++) {
            unsigned char a = (i < token.size()) ? (unsigned char)token[i] : 0;
            unsigned char b = (i < sizeof(s.token)) ? (unsigned char)s.token[i] : 0;
            e |= (unsigned char)(a ^ b);
        }
        if (e == 0 && s.used && s.expiry > now) live = true;
        d |= e;
    }
    (void)d;
    return live;
}

bool aatpeSessionAnyActive(void) {
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    time_t now;
    if (!readMonotonicSec(now)) return false;
    purgeExpiredLocked(now);
    for (const auto& s : g_slots) if (s.used) return true;
    return false;
}

void aatpeSessionPurgeAll(void) {
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    for (auto& s : g_slots) clearSlotLocked(s);
}

#ifdef AATPE_HOST_TEST
void aatpeSessionSetClockFailForTest(bool on) {
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    g_forceClockFailForTest = on ? 1 : 0;
}
#endif
