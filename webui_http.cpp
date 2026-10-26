// P1-7: Built-in HTTP server (real original webui_http.cpp, ~486 lines)
// Lightweight HTTP/1.1 server for WebUI aggregation and health checks.
// No external dependencies — raw POSIX sockets.
#include "webui_http.h"
#include "aatped.h"
#include "aatpe_session_token.h"
#include "aatpe_command_queue.h"
#include "aatpe_command_dispatcher.h"
#include "integrity.h"   // G3-001: privilegedOpsAllowed() fail-closed gate
#include "aatpe_config_schema.h"  // ROUND-004: configKeyHotReloadClass()
#include "aatped_apklabel.h"  // v4.3.2: resolve installed package's real label from its APK

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <netinet/in.h>
#include <signal.h>
#include <unordered_set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace aatpe;

// [P2-test] These are defined at GLOBAL scope in aatped.cpp. Declaring them here
// (at global scope) — NOT inside the anonymous namespace below — so the symbol
// is ::g_hmacKey / ::g_hmacActive and links against aatped.cpp. (The previous
// function-local `extern` inside the anonymous namespace bound to
// (anon)::g_hmacKey and caused an undefined-reference at link time.)
extern char g_hmacKey[128];
extern bool g_hmacActive;
// live SSE connection counter defined in aatped_main.cpp. The
// daemon main loop reads it to throttle the heavy threads/decisions/explain JSON
// writers to 5s when no EventSource stream is attached. Incremented here on SSE
// connect and RAII-decremented on disconnect (see handleSseConnection).
extern std::atomic<int> g_sseClientCount;

// W2: per-app frame serialization defined in webui_json.cpp (NOT in this
// anonymous namespace) — must be declared at GLOBAL scope, otherwise a decl
// inside `namespace {}` gets internal linkage and clang errors
// [-Werror,-Wundefined-internal] at the use site.
std::string aatpe_buildFrameAppsJson();

namespace {

std::atomic<bool> g_httpRunning{false};
std::atomic<int> g_httpPort{0};
int g_listenFd = -1;
std::thread g_httpThread;
auto g_serverStart = std::chrono::steady_clock::now();
// v94 P2 问题23/24: per-connection concurrency means multiple threads call
// handleRequest() concurrently. g_currentOrigin is set per-request and consumed
// by buildHttpResponse() in the same call stack, so it must be thread-local to
// avoid a data race between two simultaneous connections.
thread_local std::string g_currentOrigin;

// P1-3: thread-local flag set by handleConnection() when getpeername() confirms
// the peer is on 127.0.0.1. The daemon_token endpoint uses this as a second
// factor alongside the Origin header check — Origin: null is trivially
// spoofable by any local process that can set headers, so the loopback peer
// check is the trusted signal, not the Origin header itself.
thread_local bool g_peerIsLoopback = false;

// fix-v96 #3: real identity boundary. The HTTP Origin header is sent BY the
// client, so a local App can trivially forge `Origin: http://127.0.0.1` /
// `Origin: null` and scrape the bootstrap daemon token (which then mints HMAC
// keys and signs privileged /control commands). Origin is NOT authentication.
// We instead read the kernel-verified peer credential via SO_PEERCRED.
//   -1  => could not be obtained (TCP loopback socket: SO_PEERCRED is only
//          valid on AF_UNIX) -> treated as UNTRUSTED.
//   0   => root
//   1000=> system
// The daemon_token endpoint hands out its token ONLY to a trusted peer UID on a
// trusted IPC channel. A TCP loopback connection (where we cannot verify the
// caller UID) is refused — the token must instead be read through the root
// module path / trusted IPC, exactly as the hardening requires.
thread_local int g_peerUid = -1;

// LED-156 (SEC-003): rate-limit bucket key. On AF_UNIX this equals the verified
// peer UID (real per-app isolation). On TCP loopback SO_PEERCRED fails, so every
// client previously collapsed into the shared g_peerUid==-1 bucket and one chatty
// local app could exhaust the 100 req/s budget and 429 the legitimate WebUI.
// We now key the TCP bucket by the peer's ephemeral source port (offset above the
// range of real UIDs) so distinct connections get isolated buckets.
thread_local int g_rateBucketKey = -1;

// round-4 (P2 #6): SSE connection admission control.
//   - g_sseTotal: hard cap on simultaneous /events connections (one OS thread per
//     connection). Bounds thread/memory usage against a client opening hundreds of
//     EventSource streams.
//   - Per-UID cap: on AF_UNIX we know the real caller UID; on TCP loopback the
//     UID is -1, so all loopback clients share the "-1" bucket. This still caps a
//     single misbehaving local process while allowing multiple legit clients.
static constexpr int kSseMaxTotal = 16;
static constexpr int kSseMaxPerUid = 3;
// G8-002 (ROUND-001): TCP loopback cannot distinguish apps, so the shared
// g_peerUid=-1 bucket gets a tighter cap than a verified AF_UNIX UID. The legit
// WebUI uses 1-2 SSE streams; ROUND-003 raised this from 4 to 6 to absorb a
// fast page-refresh (old slot releases within ~250ms via MSG_DONTWAIT EOF poll,
// but a 1s reconnect race could otherwise 429 the second tab). 6 is still
// well below kSseMaxTotal=16.
static constexpr int kSseMaxPerSharedTcp = 6;
std::atomic<int> g_sseTotal{0};

// G-TSan fix: per-UID SSE admission state is intentionally heap-allocated and
// NEVER freed. stopHttpServer() has only an 8s join budget; under ThreadSanitizer
// (which slows execution ~10x) an in-flight /events SSE thread can still be
// running when the budget expires and its registry entry is detach()ed. That
// detached thread may keep running past process exit; when it finally unwinds,
// its SseSlotGuard dtor would lock the mutex and erase from the map AFTER the
// static std::mutex/std::map have already been destroyed by atexit -> TSan
// reports heap-use-after-free + data race. By leaking the singleton (function-
// local static pointer, constructed on first use, deliberately not deleted) the
// admission state outlives every thread — including late-exiting detached ones.
// g_sseTotal stays a plain atomic (already teardown-safe).
struct SseAdmission {
    std::mutex mu;
    std::map<int,int> byUid;
};
static SseAdmission& sseAdmission() {
    static SseAdmission* s = new SseAdmission();  // intentionally leaked, never destroyed
    return *s;
}

// RAII guard that releases an admitted SSE slot on scope exit.
struct SseSlotGuard {
    int uid = 0;          // cppcheck uninitvar: default so early-return path has a defined value
    bool acquired = false;
    ~SseSlotGuard() {
        if (!acquired) return;
        g_sseTotal.fetch_sub(1, std::memory_order_relaxed);
        SseAdmission& adm = sseAdmission();
        std::lock_guard<std::mutex> lk(adm.mu);
        auto it = adm.byUid.find(uid);
        if (it != adm.byUid.end()) {
            it->second -= 1;
            if (it->second <= 0) adm.byUid.erase(it);
        }
    }
};

// Try to admit a new SSE connection for peer UID. Returns the guard (acquired=true)
// on success, or acquired=false when either the total or per-UID cap is hit.
static SseSlotGuard sseTryAcquire(int uid) {
    SseSlotGuard g;
    if (g_sseTotal.load(std::memory_order_relaxed) >= kSseMaxTotal) return g;
    int perUidLimit = (uid < 0) ? kSseMaxPerSharedTcp : kSseMaxPerUid;
    SseAdmission& adm = sseAdmission();
    std::lock_guard<std::mutex> lk(adm.mu);
    if (adm.byUid[uid] >= perUidLimit) return g;
    adm.byUid[uid] += 1;
    g_sseTotal.fetch_add(1, std::memory_order_relaxed);
    g.uid = uid;
    g.acquired = true;
    return g;
}

// Trusted peer UIDs for the privileged bootstrap endpoint.
static bool peerUidIsTrusted(int uid) {
    return uid == 0 || uid == 1000;  // root, system
}

// v94 P2 问题21/22: Max request body size (64KB). Bounds memory and prevents
// oversized-body DoS. The /control command itself is <=512 bytes; 64KB leaves
// ample headroom for headers + body.
static const size_t kMaxRequestBody = 65536;
// v94 P2 问题23: per-connection read timeout (2s). A slow client cannot block
// its handler thread for more than this long.
static const int kRecvTimeoutSec = 2;
// round-3 (P0-1 / P1-18): per-connection WRITE timeout. A client that
// accepts the connection but never reads the response would otherwise pin the
// send() loop forever; stopHttpServer() would then have to detach() the thread
// and (because DoneGuard kept a raw ConnEntry*) hit a use-after-free once the
// registry was cleared. SO_SNDTIMEO bounds send() to 5s so every handler thread
// always terminates in a bounded time, and we pair it with explicit
// shutdown(fd, SHUT_RDWR) of every in-flight client socket at stop time.
// G-test: AATPE_SSE_HALFOPEN_TIMEOUT_SEC env override (default 5s, product
// behavior unchanged). Tests set this to 1s for deterministic, fast slot
// reclaim on half-open SSE connections (silent peer that never FINs).
static int sendTimeoutSec() {
    static const int kDefault = 5;
    const char* env = getenv("AATPE_SSE_HALFOPEN_TIMEOUT_SEC");
    if (!env) return kDefault;
    int v = atoi(env);
    return v < 1 ? kDefault : v;
}

// v94 改进包B 任务2: bounded, joinable connection-thread registry.
// round-3 (P0-1): UAF hardening. ConnEntry is now held by shared_ptr.
// The handler thread takes a shared_ptr copy (via DoneGuard), so the ConnEntry
// object outlives the registry: even if stopHttpServer() runs past its join
// budget and detaches/erases the registry entry, the thread's DoneGuard still
// owns a valid ConnEntry when it sets ->done and exits. No raw ConnEntry*
// crosses the thread boundary, so there is no use-after-free path.
//   * stopHttpServer() drains the registry with a timeout (recv+send timeouts
//     bound each in-flight connection).
static const size_t kMaxConnections = 32;
struct ConnEntry {
    std::thread th;
    // P0-1: atomic so the handler thread (DoneGuard dtor) can clear it while
    // stopHttpServer() reads it from another thread.
    std::atomic<int> clientFd{-1};
    std::atomic<bool> done{false};
    // P3-11: removed the write-only `bool isSse` field. hasActiveClients() counts
    // active connections via !done + the accept grace window, never reading isSse,
    // so the field was written but never read (dead state).
};
std::mutex g_connMutex;
std::list<std::shared_ptr<ConnEntry>> g_connThreads;

// v94 P0-bugfix (Bug-1/2/3): last time any client connection was accepted, in
// steady_clock ms. The 2s daemon write block gates the expensive
// threads/decisions/explain serialization behind hasActiveClients(). On a real
// device the WebView's SSE stream can be closed by the browser (CORS/mixed-content
// flap, tab switch, WebView recycle) even though the page is still open and
// polling /state/all; a pure "any !done conn" probe then returns false and the
// gated JSON silently stops being written -> thread monitor / decisions /
// explain show "no data" / "loading" forever. We keep a short grace window: any
// accept in the last kClientGraceMs keeps the writer enabled, which survives an
// SSE reconnect flap while still stopping the waste when the page actually
// closes. Bumped on every successful accept(); cheap.
std::atomic<long long> g_lastClientAcceptMs{0};
static const long long kClientGraceMs = 60000;  // 60s

// Read entire file into string. Returns empty string on failure.
std::string readFileToString(const std::string& path) {
    // G8-005: O_NOFOLLOW | O_CLOEXEC for defense-in-depth on root-owned state files.
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return "";
    char buf[4096];
    std::string result;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        result.append(buf, n);
    }
    close(fd);
    return result;
}

// P0-2: loop send() until every byte in data[0..len) has been queued.
// A single send() on a TCP socket can return short (kernel send buffer full) or
// -1/EAGAIN. On a blocking fd with SO_SNDTIMEO, EAGAIN means the send timeout
// (5s) has already elapsed. To bound half-open SSE connections (silent peer that
// never reads and never RSTs) we track last-progress time and give up after
// kSendTimeoutSec(5s) of zero byte progress, returning false so the SSE loop
// breaks and SseSlotGuard releases the slot.
static bool sendAll(int fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    size_t off = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (off < len) {
        ssize_t w = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (w > 0) { off += (size_t)w; t0 = std::chrono::steady_clock::now(); continue; }
        if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (errno == EINTR) continue;
            // EAGAIN/EWOULDBLOCK: on a blocking fd with SO_SNDTIMEO set, this
            // means the OS already blocked send() for the FULL sendTimeoutSec
            // budget because neither our sndbuf nor the peer's receive window
            // had room. That is the half-open (silent, no-FIN) signal. Use the
            // SAME env-driven budget as SO_SNDTIMEO (sendTimeoutSec, default 5s
            // product / 1s test) instead of a hardcoded 5s so the test knob
            // actually governs reclaim; previously the hardcoded 5 meant the
            // AATPE_SSE_HALFOPEN_TIMEOUT_SEC=1 test could not drive a 1s break.
            auto now = std::chrono::steady_clock::now();
            auto stalled = std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();
            if (stalled >= sendTimeoutSec()) return false;
            struct timespec ts = {0, 1000};  // 1ms
            nanosleep(&ts, nullptr);
            continue;
        }
        return false;  // EPIPE / ECONNRESET / hard error
    }
    return true;
}

// F-A6-08: jsonEscape() is now the single inline canonical implementation in
// webui_http.h (shared with webui_json.cpp). The local copy was removed so the
// escaping rules can no longer drift between HTTP responses and state.json.

// Constant-time string comparison (prevents timing attacks on token comparison).
// Returns false immediately on length mismatch, but compares all bytes when equal
// length so no early-exit leak. Equivalent to aatpe_crypto::constantTimeHexEqual
// but kept local to avoid pulling the crypto header into this TU.
bool constantTimeStringEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

// Robust URL-decode. Handles '+' as space. For "%XX":
//  - If the two following characters are valid hex digits, decode the byte.
//  - If there are fewer than 2 chars left, or the digits are NOT hex, keep the
//    '%' byte literally (preserves malformed input, avoids NULL-byte injection and
//    avoids out-of-bounds reads). Consecutive "%%" sequences are handled naturally.
std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%') {
            // Need at least two characters after this '%' to attempt a decode.
            if (i + 2 < s.size()) {
                int hi = hexVal(s[i + 1]);
                int lo = hexVal(s[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out += (char)(hi * 16 + lo);
                    i += 2;
                    continue;
                }
            }
            // Invalid/incomplete %XX: keep '%' verbatim.
            out += c;
        } else {
            out += c;
        }
    }
    return out;
}

// Forward declaration: defined below handleControl (kept here so handleControl can
// call it; the full definition documents the whitelist).
bool isCommandAllowed(const std::string& rawCmd);

// Forward declaration: defined ~line 1228; used by handleSession (line ~956) which
// appears earlier in this translation unit.
static bool checkDaemonTokenAuth(const std::string& request, const std::string& path);
static bool checkSseTicketAuth(const std::string& request, const std::string& path);

// W-002: read-only /control commands that are safe to accept on GET. Today the
// /control surface has NO read-only commands (everything in the whitelist mutates
// state); this helper exists so a future "get_status"/"state" probe can be added
// without re-opening the CSRF discussion. State-changing commands MUST ride POST
// (or carry a valid HMAC signature / session bearer token, which an attacker cannot
// forge cross-origin).
bool isReadOnlyControlCommand(const std::string& rawCmd) {
    std::string cmd = rawCmd;
    while (!cmd.empty() && (cmd.back()=='\n'||cmd.back()=='\r'||cmd.back()==' '||cmd.back()=='\t')) cmd.pop_back();
    while (!cmd.empty() && (cmd.front()==' '||cmd.front()=='\t')) cmd.erase(cmd.begin());
    size_t eq = cmd.find('=');
    std::string key = (eq == std::string::npos) ? cmd : cmd.substr(0, eq);
    if (key == "get_status" || key == "state" || key == "get_state") return true;
    return false;
}

// ============================================================================
// WebUI hardening infrastructure
//
//  (a) Sliding-window rate limiter. The listener is loopback-only but any local
//      app can reach 127.0.0.1; without a cap a tight loop of GET /state/all
//      burns CPU on the ~180-file aggregate + JSON serialization. On TCP loopback
//      SO_PEERCRED is unavailable (g_peerUid stays -1) and the peer IP is always
//      127.0.0.1, so the per-IP bucket is effectively a single global bucket.
//  (b) HMAC nonce replay cache. The ±300s timestamp window lets a captured
//      (cmd,ts,sig) triple be replayed verbatim inside the window. When the
//      client ALSO sends .nonce=<hex> (bound into the signed data) we reject a
//      triple seen before. A nonce-less request is still accepted for backward
//      compatibility with older WebUI builds.
//  (c) Command audit log. Every dispatched /control command (profile/thermal/
//      migration/ML/control-mode) is appended to STATE_DIR/audit.log with
//      timestamp, peer uid and result for post-hoc forensics.
// ============================================================================
static const size_t kRateLimitPerSec = 100;
// G8-003 (ROUND-001): TCP loopback cannot identify the peer UID, so all TCP
// clients share ONE strict bucket. 50 req/s total across every local app is
// plenty for the legit WebUI (it polls at ~1-5 req/s) but bounds a flood from
// a malicious neighbor. AF_UNIX (verified UID) keeps 100 req/s per UID.
static const size_t kRateLimitPerSecTcpShared = 50;
// G8-003: fixed bucket key used for all TCP loopback clients (g_peerUid < 0).
// Chosen to be negative and outside the real-UID range so it never collides
// with a verified UID bucket.
static const int kSharedTcpBucketKey = -1;
// P1-5: per-peer-UID token buckets. On AF_UNIX we get a real UID via
// SO_PEERCRED; on TCP loopback all clients share kSharedTcpBucketKey.
std::mutex g_rateMutex;
std::map<int, std::list<std::chrono::steady_clock::time_point>> g_rateWindowByUid;

bool rateLimitAllowed() {
    auto now = std::chrono::steady_clock::now();
    int uid = g_rateBucketKey;
    // G8-003: TCP shared bucket is stricter than per-UID AF_UNIX buckets.
    size_t limit = (uid == kSharedTcpBucketKey) ? kRateLimitPerSecTcpShared
                                                : kRateLimitPerSec;
    std::lock_guard<std::mutex> lk(g_rateMutex);
    auto& win = g_rateWindowByUid[uid];
    while (!win.empty() &&
           now - win.front() > std::chrono::seconds(1)) {
        win.pop_front();
    }
    if (win.size() >= limit) return false;
    win.push_back(now);
    return true;
}

// --- HMAC nonce replay cache ---
struct NonceEntry { std::string key; int64_t seenTs; };
std::mutex g_nonceMutex;
std::deque<NonceEntry> g_nonceCache;
// Issue-14: hash set for O(1) membership test instead of O(n) linear scan.
// The deque handles ordered expiration (pop_front oldest), the unordered_set
// handles fast duplicate detection.
static std::unordered_set<std::string> g_nonceSet;
// P2-5: bump capacity to ~32k. The old 512-entry cache was aggressively evicting
// in-window nonces (pop_front on overflow), letting a chatty legitimate client
// flush the cache and open a replay window inside the ±300s acceptance period.
// We now ONLY evict entries whose seenTs is outside the replay window (600s =
// 300s skew margin); in-window entries are never evicted by size. 32k bounds
// memory even under a burst of legit signed requests.
static const size_t kNonceCacheMax = 32768;
// Returns true if the nonce was ALREADY seen (replay -> reject). Inserts on miss.
// F-A6-03[P2]: the eviction timestamp is the SERVER wall clock captured here, NOT
// the client-supplied reqTs. A client inside the ±300s window can still send a
// future-dated reqTs (e.g. now+1e9); using that as seenTs made `now - seenTs`
// permanently negative, so the entry was never swept and 32k entries of poisoned
// future nonces could evict legitimate in-window nonces. reqTs remains the
// client timestamp that the CALLER already validated against the window; it is
// deliberately NOT trusted for cache lifetime.
bool nonceReplayOrInsert(const std::string& nonceKey, int64_t reqTs) {
    (void)reqTs;  // validated by caller against the ±300s window; not trusted here
    std::lock_guard<std::mutex> lk(g_nonceMutex);
    int64_t now = (int64_t)time(nullptr);
    // Sweep ONLY entries older than the replay window (600s with skew margin).
    // In-window entries are never evicted by the size cap below.
    while (!g_nonceCache.empty() && (now - g_nonceCache.front().seenTs) > 600) {
        g_nonceSet.erase(g_nonceCache.front().key);
        g_nonceCache.pop_front();
    }
    // Issue-14: O(1) hash-set lookup instead of linear scan.
    if (g_nonceSet.count(nonceKey)) return true;
    g_nonceSet.insert(nonceKey);
    g_nonceCache.push_back({nonceKey, now});  // F-A6-03: server time, not reqTs
    // Hard backstop: if the cache somehow exceeds 32k entries ALL within the
    // window (pathological burst), drop the oldest.
    while (g_nonceCache.size() > kNonceCacheMax) {
        g_nonceSet.erase(g_nonceCache.front().key);
        g_nonceCache.pop_front();
    }
    return false;
}

// --- Command audit log ---
// P3-42: every sensitive operation (profile change, control-mode change, ML enable,
// federated import, thermal override, etc.) is recorded with timestamp, source,
// command, result and reason. Written to STATE_DIR/audit.log (0600 root-only).
std::mutex g_auditMutex;
// round-3 (P1-23): audit log rotation. audit.log is capped at ~2 MB; on
// exceeding the cap it is renamed to audit.log.1 (overwriting the previous .1)
// and a fresh audit.log is started. This bounds disk use without losing the most
// recent history.
static const off_t kAuditMaxBytes = off_t(2) * 1024 * 1024;  // 2 MB

// P3-42: core audit writer. source is one of "webui" / "cli" / "watchdog" /
// "federated" / "internal". reason is empty on success, or a short rejection
// explanation on failure (e.g. "not-in-whitelist", "hmac-failed", "read-only").
void auditCommand(const std::string& actualCmd, int peerUid, bool ok,
                  const std::string& source, const std::string& reason) {
    std::lock_guard<std::mutex> lk(g_auditMutex);
    std::string path = std::string(STATE_DIR) + "/audit.log";
    // Rotate if the current file has grown past the cap.
    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && st.st_size >= kAuditMaxBytes) {
        std::string oldPath = path + ".1";
        // P3: check rename() result. A failed rotation (e.g. .1 on a different
        // mount) must not silently drop audit records.
        if (::rename(path.c_str(), oldPath.c_str()) != 0) {
            LOG_WARN("audit: rotate rename(%s -> %s) failed: %s",
                     path.c_str(), oldPath.c_str(), aatpeErrStr(errno));
        }
    }
    // G8-005/G3-002: O_NOFOLLOW prevents symlink redirection if STATE_DIR ever
    // becomes non-root-only; O_CLOEXEC keeps the fd out of child processes.
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    char tsbuf[32];
    time_t now = time(nullptr);
    struct tm tm_{};
    localtime_r(&now, &tm_);
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S", &tm_);
    std::string src = source.empty() ? "webui" : source;
    std::string rs = reason.empty() ? (ok ? "applied" : "rejected") : reason;
    // W-012: the audit line is a single newline-terminated record. If actualCmd
    // (or reason) contained a raw \n/\r it would inject a forged second line into
    // the log (log forging). Escape CR/LF so every audit event stays on one line.
    auto escapeNewlines = [](const std::string& in) -> std::string {
        std::string out;
        out.reserve(in.size() + 2);
        for (char c : in) {
            if (c == '\n') { out += "\\n"; }
            else if (c == '\r') { out += "\\r"; }
            else out.push_back(c);
        }
        return out;
    };
    std::string safeCmd = escapeNewlines(actualCmd);
    std::string safeReason = escapeNewlines(rs);
    std::string line = std::string(tsbuf) + " source=" + src +
                       " uid=" + std::to_string(peerUid) +
                       " cmd=" + safeCmd +
                       " result=" + (ok ? "ok" : "rej") +
                       " reason=" + safeReason + "\n";
    // P2: loop write() until every byte is persisted (short write on a busy
    // tmpfs must not truncate the audit record).
    {
        size_t off = 0, len = line.size();
        while (off < len) {
            ssize_t w = write(fd, line.c_str() + off, len - off);
            if (w < 0) { if (errno == EINTR) continue; break; }
            if (w == 0) break;  // P3 round7-rework
            off += (size_t)w;
        }
    }
    close(fd);
}

// v4.3.0 CORS isolation fix. The root-manager WebView (KernelSU / Magisk / APatch)
// loads the WebUI from a WebViewAssetLoader origin (https://appassets.androidplatform.net)
// or directly from https://kernelsu.org, and its fetch()/EventSource() to the
// loopback daemon are CROSS-ORIGIN. The browser then requires a matching
// Access-Control-Allow-Origin on every response — without it the fetch is blocked
// at the network layer, so the UI shows "HTTP 未发现" even though
// 127.0.0.1:8888 is LISTEN and curl /health returns 200.
//
// Two SEPARATE predicates by design ("fix A, self-check B"):
//
//   isTrustedManagerOrigin() = the NARROW, non-spoofable set of explicit manager
//     https roots. It is used both for the response CORS echo AND as the trusted
//     half of the CSRF-safe POST gate.
//
//   isCorsOriginAllowed() = the BROAD response-header set. It additionally admits
//     the literal "null" origin (what a file:// / sandboxed-iframe WebView emits as
//     Origin: null on the real device). We echo ACAO:null so the root manager can
//     READ loopback liveness/state responses, BUT:
//       * "null" is emit-able by ANY web page (sandboxed iframe / data: URL), so it
//         must NOT widen the CSRF gate (isCsrfTrustedOrigin excludes it) and must NOT
//         be paired with Access-Control-Allow-Credentials:true.
//       * authentication is NEVER the Origin header: /control writes still require the
//         daemon token or a per-command HMAC; /state/all still requires the daemon token.
//         CORS only controls whether the browser exposes the response to JS — it does
//         not grant any privilege.
//
// The loopback prefix check keeps a boundary guard so "http://127.0.0.1.evil.com"
// (char after the prefix is '.', not ':') is rejected.
static bool loopbackHttpOrigin(const std::string& origin) {
    static const char* kLoopbackPrefixes[] = {
        "http://127.0.0.1",
        "http://localhost"
    };
    for (const char* prefix : kLoopbackPrefixes) {
        size_t plen = strlen(prefix);
        if (origin.size() >= plen && origin.compare(0, plen, prefix) == 0) {
            if (origin.size() == plen || origin[plen] == ':') return true;
        }
    }
    return false;
}

// Explicit, non-spoofable manager WebView origins (KernelSU / Magisk / APatch).
// These are the roots a root-manager WebView actually loads the UI from. Kept as an
// enumerable whitelist so a new manager is a one-line addition.
static bool isTrustedManagerOrigin(const std::string& origin) {
    static const char* kManagerOrigins[] = {
        "https://appassets.androidplatform.net",   // WebViewAssetLoader (KSU/Magisk/APatch default)
        "https://kernelsu.org",                     // KernelSU manager webview (real-device observed)
        "https://www.kernelsu.org",
        "https://ps.kernelui.github.io",           // defensive: known Manager origin
        "https://topjohnwu.github.io",             // Magisk webui asset host
        "https://127.0.0.1",                        // https loopback (local dev)
        "https://localhost"
    };
    for (const char* o : kManagerOrigins) {
        if (origin == o) return true;
    }
    return false;
}

// NARROW CSRF-trusted origin. A cross-origin POST carrying this Origin MAY be
// treated as CSRF-safe. Deliberately EXCLUDES the literal "null" origin (spoofable
// by any web page) so broadening the response whitelist to admit "null" cannot
// widen the write CSRF gate.
static bool isCsrfTrustedOrigin(const std::string& origin) {
    if (isTrustedManagerOrigin(origin)) return true;
    if (loopbackHttpOrigin(origin)) return true;
    return false;
}

// BROAD CORS-response allowlist. Reflect the request Origin into
// Access-Control-Allow-Origin only when it is a trusted manager origin, a loopback
// origin, or the sandboxed "null" origin. Everything else gets NO CORS headers —
// the browser then blocks the cross-origin response, defeating CSRF-style attacks
// from arbitrary web pages.
static bool isCorsOriginAllowed(const std::string& origin) {
    if (origin.empty()) return false;
    if (isTrustedManagerOrigin(origin)) return true;
    if (loopbackHttpOrigin(origin)) return true;
    // Sandboxed webview origin (file:// / WebViewAssetLoader / data:). Echoed so the
    // root manager can read loopback responses; guardrailed in appendCorsHeaders.
    if (origin == "null") return true;
    return false;
}

// P3: centralized CORS header emission. Both buildHttpResponse() (short HTTP
// responses) and handleSseConnection() (long-lived /events) share this — one
// source of truth. Echoes the allowlisted Origin byte-for-byte and advertises the
// preflight surface. The credentials flag is withheld for the spoofable literal
// "null" origin (any web page can emit it); the root manager authenticates with the
// X-Daemon-Token header, not cookies, so it does not need the flag.
static void appendCorsHeaders(std::ostringstream& resp, const std::string& origin) {
    if (origin.empty() || !isCorsOriginAllowed(origin)) return;
    resp << "Access-Control-Allow-Origin: " << origin << "\r\n";
    resp << "Vary: Origin\r\n";
    resp << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    resp << "Access-Control-Allow-Headers: Content-Type, X-Daemon-Token\r\n";
    resp << "Access-Control-Max-Age: 86400\r\n";
    // Do NOT pair the spoofable literal "null" with credentials. All previously
    // allowlisted (non-null) origins keep the flag (unchanged behavior).
    if (origin != "null") {
        resp << "Access-Control-Allow-Credentials: true\r\n";
    }
}

// P2-4: CSRF safety check for a POST /control request. A cross-origin HTML form
// can POST to 127.0.0.1 with Content-Type: text/plain (a "simple" request that
// triggers no CORS preflight) and drive /control when hmacRequired=false. We
// treat a POST as CSRF-safe ONLY if at least one of the following holds, all of
// which a simple cross-origin form CANNOT satisfy:
//   (a) Content-Type is application/json (non-simple -> browser preflights),
//   (b) an X-Daemon-Token header is present (custom header -> preflight),
//   (c) the Origin header is present AND passes isCorsOriginAllowed (i.e. it is
//       our own WebUI webview origin, not an arbitrary evil page).
// Signed/.stok requests are independently authenticated and bypass this check.
static bool isCsrfSafePost(const std::string& request) {
    // G8-004 (ROUND-001): only search the HEADERS block (up to \r\n\r\n), not the
    // whole request. Previously a POST body containing the literal string
    // "x-daemon-token:" (e.g. a form field value) was treated as a CSRF-safe
    // header. That didn't open a command channel (LED-158 still requires
    // sig/stok) but eroded the defense-in-depth gate.
    size_t headerEnd = request.find("\r\n\r\n");
    std::string headers = (headerEnd == std::string::npos) ? request
                                                           : request.substr(0, headerEnd);
    // (b) X-Daemon-Token header present?
    {
        std::string lower = headers;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
        if (lower.find("x-daemon-token:") != std::string::npos) return true;
    }
    // Parse Content-Type and Origin case-insensitively.
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
    // (a) application/json content type?
    size_t ct = lower.find("content-type:");
    if (ct != std::string::npos) {
        size_t eol = lower.find("\r\n", ct);
        std::string ctLine = lower.substr(ct, (eol==std::string::npos)?eol:eol-ct);
        if (ctLine.find("application/json") != std::string::npos) return true;
    }
    // (c) Origin present and allowlisted? Use the NARROW csrf-trusted predicate,
    // NOT the broad response whitelist: the spoofable literal "null" origin must
    // never make a cross-origin POST CSRF-safe.
    size_t op = lower.find("\r\norigin:");
    if (op != std::string::npos) {
        size_t os = op + 9;
        size_t oe = headers.find("\r\n", os);
        std::string origin = (oe != std::string::npos) ? headers.substr(os, oe-os) : headers.substr(os);
        while (!origin.empty() && origin.front()==' ') origin.erase(origin.begin());
        if (isCsrfTrustedOrigin(origin)) return true;
    }
    return false;
}

// Build HTTP response with given status, content-type, and body.
std::string buildHttpResponse(int status, const std::string& contentType,
                              const std::string& body, const std::string& origin) {
    const char* reason = "OK";
    if (status == 400) reason = "Bad Request";
    else if (status == 401) reason = "Unauthorized";
    else if (status == 403) reason = "Forbidden";
    else if (status == 404) reason = "Not Found";
    else if (status == 405) reason = "Method Not Allowed";
    else if (status == 413) reason = "Payload Too Large";
    else if (status == 500) reason = "Internal Server Error";

    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " " << reason << "\r\n";
    resp << "Content-Type: " << contentType << "\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    // P0 keep-alive: handleConnection loops requests on one fd; idle 2s recv
    // timeout or the client's "Connection: close" ends the connection. This kills
    // the per-request TIME_WAIT storm on :8888.
    resp << "Connection: keep-alive\r\n";
    resp << "Server: AATPE-WebUI/1.0\r\n";
    // P3-16: defense-inference response headers. nosniff stops the browser from
    // MIME-sniffing a JSON error body into HTML; CSP locks the page down so any
    // reflected content cannot load scripts/frames from anywhere.
    resp << "X-Content-Type-Options: nosniff\r\n";
    resp << "X-Frame-Options: DENY\r\n";
    resp << "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'; base-uri 'none'\r\n";
    appendCorsHeaders(resp, origin);
    resp << "\r\n";
    resp << body;
    return resp.str();
}

// Handle GET /health — liveness probe.
std::string handleHealth() {
    auto now = std::chrono::steady_clock::now();
    long long uptimeSec = std::chrono::duration_cast<std::chrono::seconds>(
        now - g_serverStart).count();
    std::ostringstream body;
    // v94-sec: removed "pid" field. /health is a liveness probe and does not need
    // to disclose the daemon's PID (information disclosure / local enumeration).
    body << "{\"status\":\"ok\",\"uptime_s\":" << uptimeSec
         << ",\"port\":" << g_httpPort.load() << "}";
    return buildHttpResponse(200, "application/json", body.str(), g_currentOrigin);
}

// #22: server-side delta baseline for /state/all?delta=1. The first delta request
// from a client returns every key (diff from empty); later requests return only the
// keys whose value (or presence) changed since the previous delta request. This is
// a coarse, connection-free delta — good enough for the loopback WebUI and fully
// backward compatible (no ?delta= -> identical full JSON as before).
static std::mutex g_deltaMutex;
// P1-3: per-client baselines keyed by ?cid=<id>. The old code shared ONE global
// baseline across all clients, so two browsers stole each other's deltas.
// HTTP-01 fix: each entry records last-access time; when the table is full and a
// NEW cid arrives, we evict the LRU entry (smallest lastAccessMs) instead of
// erase(begin()) which removed the lexicographically smallest cid (std::map is
// ordered by key, not by recency).
struct DeltaBaseline {
    std::map<std::string,std::string> state;
    long long lastAccessMs = 0;
};
static std::map<std::string, DeltaBaseline> g_deltaBaselines;
static const size_t kMaxDeltaClients = 32;

// P2: exact k=v parser (old query.find("delta=1") also matched "notdelta=1").
static bool queryParamEq(const std::string& query, const std::string& key, std::string& valOut) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = (amp == std::string::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        std::string k = (eq == std::string::npos) ? pair : pair.substr(0, eq);
        std::string v = (eq == std::string::npos) ? std::string() : pair.substr(eq + 1);
        if (k == key) { valOut = v; return true; }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return false;
}

// Handle GET /state/all. query is the raw "k=v&..." portion after '?' (may be
// empty). Without query params it returns the full aggregate JSON (now embedding
// "state_generation"). With ?delta=1 it returns only changed keys.
std::string handleStateAll(const std::string& query) {
    // P2: exact parse of ?delta=1 (not substring match).
    std::string deltaVal;
    bool wantDelta = queryParamEq(query, "delta", deltaVal) && (deltaVal == "1");
    std::string cid;
    queryParamEq(query, "cid", cid);
    // P3-17: bound the cid length. An unbounded cid becomes an unbounded key in
    // g_deltaBaselines (a map keyed by attacker-controlled string); truncate to
    // 64 chars so the per-client baseline table stays memory-bounded.
    if (cid.size() > 64) cid.resize(64);
    if (cid.empty()) cid = "anon";

    if (!stateCacheIsSeeded()) stateCacheSeedFromDir(STATE_DIR);
    std::vector<std::pair<std::string,std::string>> snap = stateCacheSnapshot();
    uint64_t gen = stateCacheGeneration();

    if (!wantDelta) {
        // Full payload (backward compatible). state_generation already embedded.
        std::string body = aatpe::webui_http::buildAggregateStateJson();
        return buildHttpResponse(200, "application/json", body, g_currentOrigin);
    }

    // Delta mode: diff current snapshot against THIS CLIENT's baseline.
    std::map<std::string,std::string> cur(snap.begin(), snap.end());
    std::lock_guard<std::mutex> lk(g_deltaMutex);
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_deltaBaselines.size() >= kMaxDeltaClients &&
        g_deltaBaselines.find(cid) == g_deltaBaselines.end()) {
        // HTTP-01: evict the LRU entry (smallest lastAccessMs), not begin().
        auto lruIt = g_deltaBaselines.begin();
        long long lruTime = lruIt->second.lastAccessMs;
        for (auto it = std::next(g_deltaBaselines.begin()); it != g_deltaBaselines.end(); ++it) {
            if (it->second.lastAccessMs < lruTime) {
                lruTime = it->second.lastAccessMs;
                lruIt = it;
            }
        }
        g_deltaBaselines.erase(lruIt);
    }
    auto& baseline = g_deltaBaselines[cid];
    baseline.lastAccessMs = nowMs;
    std::ostringstream body;
    body << "{";
    body << "\"state_generation\":" << gen << ",";
    body << "\"changed_keys\":[";
    bool first = true;
    // changed / added keys
    for (const auto& kv : cur) {
        auto bit = baseline.state.find(kv.first);
        bool changed = (bit == baseline.state.end()) || (bit->second != kv.second);
        if (!changed) continue;
        if (!first) body << ",";
        first = false;
        body << "\"" << jsonEscape(kv.first) << "\"";
    }
    body << "],";
    // removed keys
    body << "\"removed_keys\":[";
    bool firstR = true;
    for (const auto& kv : baseline.state) {
        if (cur.find(kv.first) == cur.end()) {
            if (!firstR) body << ",";
            firstR = false;
            body << "\"" << jsonEscape(kv.first) << "\"";
        }
    }
    body << "],";
    // only the changed values
    body << "\"state\":{";
    bool firstV = true;
    for (const auto& kv : cur) {
        auto bit = baseline.state.find(kv.first);
        bool changed = (bit == baseline.state.end()) || (bit->second != kv.second);
        if (!changed) continue;
        if (!firstV) body << ",";
        firstV = false;
        body << "\"" << jsonEscape(kv.first) << "\":\"" << jsonEscape(kv.second) << "\"";
    }
    body << "}}";
    // Advance THIS CLIENT's baseline to the current snapshot.
    baseline.state = std::move(cur);
    return buildHttpResponse(200, "application/json", body.str(), g_currentOrigin);
}

// ============================================================================
// #78 WebUI data classification: /state/public
// ----------------------------------------------------------------------------
// Returns ONLY non-sensitive, user-visible metrics (CPU, temperature, frame
// rate, system state). Does NOT require daemon token auth — safe for any local
// app to read. Sensitive keys (token, HMAC, thread internals, ML state, config
// details, decisions) are filtered out.
//
// Classification tiers (for documentation / future enforcement):
//   PUBLIC   : CPU freq/util, temperature, frame rate, system state, battery
//   DEBUG    : thread details, decision logs, per-decision JSON, explain data
//   ROOT     : daemon token, HMAC key, ML model weights, internal state machine
// ============================================================================
static const char* kPublicStateWhitelist[] = {
    "system_state",
    "control_mode",
    "thermal_emergency",
    "state_duration_sec",
    "thermal_cpu",
    "thermal_skin",
    "thermal_battery",
    "cpu_usage",
    "system_load",
    "battery_level",
    "battery_status",
    "refresh_rate",
    "frame_rate",
    "jank_percent",
    "fg_package",          // foreground app name (already considered low-sensitivity)
    "daemon_uptime",
    "version",
    "placement_avg_gain",
    "placement_avg_confidence",
    "state_ttl_sweeps",
    "state_ttl_removed",
    nullptr
};

static bool isPublicStateKey(const std::string& key) {
    for (int i = 0; kPublicStateWhitelist[i]; i++) {
        if (key == kPublicStateWhitelist[i]) return true;
    }
    return false;
}

std::string handleStatePublic() {
    if (!stateCacheIsSeeded()) stateCacheSeedFromDir(STATE_DIR);
    std::vector<std::pair<std::string,std::string>> snap = stateCacheSnapshot();

    std::ostringstream body;
    body << "{";
    body << "\"state_generation\":" << stateCacheGeneration();
    for (const auto& kv : snap) {
        if (!isPublicStateKey(kv.first)) continue;
        body << ",";
        body << "\"" << jsonEscape(kv.first) << "\":\"" << jsonEscape(kv.second) << "\"";
    }
    body << "}";
    return buildHttpResponse(200, "application/json", body.str(), g_currentOrigin);
}

// v68/P23/P24: /control interface with HMAC signature enforcement.
// Supports both POST (body) and GET (?cmd=) for WebUI compatibility.
// Command format: <command>.ts=<unix_ts>.sig=<hex_hmac_sha256>
//   The signature is computed over "<command>.ts=<unix_ts>" (timestamp included),
//   so the timestamp is bound to the signature (replay protection).
// When hmacRequired=true (P23 default): timestamp must be within ±300s window and
//   signature must verify. When hmacRequired=false: unsigned accepted (backward compat).
std::string handleControl(const std::string& cmd, bool viaGet = false, bool csrfSafe = false, const std::string* requestForAuth = nullptr) {
    (void)viaGet;  // retained for ABI/readability; CSRF safety now driven by csrfSafe.
    std::string command = cmd;
    while (!command.empty() && (command.back()=='\n'||command.back()=='\r'||command.back()==' '||command.back()=='\t')) command.pop_back();
    while (!command.empty() && (command.front()==' '||command.front()=='\t')) command.erase(command.begin());
    if (command.empty()) return buildHttpResponse(400,"application/json","{\"ok\":false,\"error\":\"empty command\"}",g_currentOrigin);
    // P25: reject over-long commands before any parsing (input-length bound).
    // Raw wire form includes a 64-hex .sig= + ~10-char .ts=; 512 leaves ample room.
    if (command.size() > 512) {
        return buildHttpResponse(400,"application/json","{\"ok\":false,\"error\":\"command too long\"}",g_currentOrigin);
    }

    // v68: Parse .ts=<ts>.sig=<sig> for HMAC verification + replay protection
    std::string actualCmd = command;
    std::string sig;
    int64_t reqTs = 0;
    bool hasSig = false;
    bool hasTs = false;
    // Parse .sig= first (suffix)
    size_t sigPos = command.rfind(".sig=");
    if (sigPos != std::string::npos && sigPos > 0) {
        sig = command.substr(sigPos + 5);
        actualCmd = command.substr(0, sigPos);
        hasSig = true;
    }
    // Parse .ts= from remaining (after stripping .sig)
    size_t tsPos = actualCmd.rfind(".ts=");
    if (tsPos != std::string::npos && tsPos > 0) {
        std::string tsStr = actualCmd.substr(tsPos + 4);
        try { reqTs = std::stoll(tsStr); hasTs = true; }
        catch (...) { hasTs = false; LOG_DEBUG("http: bad .ts= '%s', ignoring", tsStr.c_str()); }
        actualCmd = actualCmd.substr(0, tsPos);
    }

    // optional .nonce=<hex> bound into the signed data so that a
    // captured (cmd,ts,sig) triple cannot be replayed verbatim inside the ±300s
    // window. Older WebUI builds omit .nonce and stay fully functional (backward
    // compatible); the replay cache only kicks in when a nonce is presented.
    std::string nonce;
    size_t noncePos = actualCmd.rfind(".nonce=");
    if (noncePos != std::string::npos && noncePos > 0) {
        nonce = actualCmd.substr(noncePos + 7);
        actualCmd = actualCmd.substr(0, noncePos);
    }

    // F-01[P1]: .stok= is NO LONGER accepted as a write credential on /control.
    // It leaks into URL/history/logcat and is a 5-min reusable ticket. Write
    // auth is now ONLY: (1) per-command HMAC .sig/.ts/.nonce, or
    // (2) X-Daemon-Token header (checked below). stok remains SSE-only.

    // v94 P2 问题16/17: g_cfg.hmacRequired is written by the main thread
    // (loadConfig / processControlCommands). The HTTP server thread must not
    // read it directly — snapshot it under g_cfgMutex to avoid a torn bool read
    // racing with a config reload.
    bool hmacRequired;
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        hmacRequired = g_cfg.hmacRequired;
    }

    // P1-5: fail-closed read-only mode. If HMAC key init failed (g_hmacReadonly),
    // reject ALL /control write commands regardless of the hmacRequired config
    // flag. Previously a user with hmacRequired=false in aatpe.conf would get
    // zero authentication when the key could not be generated.
    if (isHmacReadonly() || !aatpe::integrity::privilegedOpsAllowed()) {
        // ROUND-003: machine-readable reason so the frontend can pop a one-time
        // toast instead of the user seeing "button does nothing".
        const char* reason = !aatpe::integrity::privilegedOpsAllowed()
            ? "integrity_failed" : "hmac_unavailable";
        std::string body = std::string("{\"ok\":false,\"read_only\":true,\"reason\":\"") + reason +
            "\",\"error\":\"daemon in read-only mode (" + reason + ")\"}";
        return buildHttpResponse(403, "application/json", body, g_currentOrigin);
    }

    // v94 P2 问题25: validation order — scheme B.
    //   1) command whitelist (syntax/format) — runs BEFORE HMAC so that obviously
    //      illegal commands (e.g. "rm -rf /", "set_profile=bogus") get 400 even
    //      when unsigned. Whitelist membership is a basic safety filter; it does
    //      not depend on authentication.
    //   2) HMAC authentication — unsigned/invalid signatures get 403.
    //   3) parameter-value validation happens inside isCommandAllowed() (enum
    //      membership, numeric range), so out-of-enum values are already 400.
    // This yields: illegal command -> 400 (no signature needed), signed-but-bad
    // params -> 400, unsigned-but-valid command -> 403.
    if (!isCommandAllowed(actualCmd)) {
        return buildHttpResponse(400,"application/json",
            "{\"ok\":false,\"error\":\"command not in whitelist\"}",g_currentOrigin);
    }

    // v68/P24: HMAC enforcement when required.
    // Signed payload = "<actualCmd>.ts=<reqTs>": the timestamp is bound INTO the
    // signature, so a captured (cmd,ts,sig) triple cannot be replayed with a
    // modified/extended timestamp to defeat the replay window below.
    //
    // #61: if a valid session bearer token is present, it satisfies auth without
    // needing the long-term HMAC signature. Otherwise fall through to HMAC.
    bool sessionAuthenticated = false;
    bool sigAuthenticated = false;
    // F-01: X-Daemon-Token header authorizes writes (CSRF-resistant custom header).
    if (requestForAuth && checkDaemonTokenAuth(*requestForAuth, "")) {
        sessionAuthenticated = true;
    }
    if (hmacRequired && !sessionAuthenticated) {
        if (!hasSig || sig.empty()) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"HMAC signature required but missing\"}",g_currentOrigin);
        }
        if (!hasTs) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"timestamp required for HMAC verification\"}",g_currentOrigin);
        }
        // LED-154 (SEC-001): .nonce= is now MANDATORY on the HMAC-signed control path.
        // Previously it was optional; when omitted the replay cache (nonceReplayOrInsert)
        // was never consulted, so a captured (cmd,ts,sig) triple could be replayed
        // verbatim inside the ±300s window. Align with handleSession()/session_issue
        // which already require a nonce. A missing nonce is rejected before any signature
        // work.
        if (nonce.empty()) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"request nonce (.nonce=<hex>) required for replay protection\"}",g_currentOrigin);
        }
        // P24 replay protection: reject timestamps outside the accepted window.
        // ±HMAC_REPLAY_WINDOW_SEC allows for clock skew between WebUI and daemon.
        const int64_t HMAC_REPLAY_WINDOW_SEC = 300;  // ±5 minutes
        int64_t now = (int64_t)time(nullptr);
        int64_t age = now - reqTs;
        if (age > HMAC_REPLAY_WINDOW_SEC || age < -HMAC_REPLAY_WINDOW_SEC) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"timestamp expired or out of window (replay protection)\"}",g_currentOrigin);
        }
        // P24: verify HMAC over actualCmd + ".ts=" + reqTs (timestamp included in signed data).
        // when a .nonce= is present it is ALSO bound into the signed data,
        // so the signature covers cmd+ts+nonce together.
        std::string signedData = actualCmd + ".ts=" + std::to_string(reqTs);
        if (!nonce.empty()) signedData += ".nonce=" + nonce;
        if (!verifyCommandHmac(signedData.c_str(), sig.c_str())) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"HMAC signature verification failed\"}",g_currentOrigin);
        }
        sigAuthenticated = true;
        // replay guard AFTER signature verification (so an unauthenticated
        // client cannot poison the nonce cache). A (cmd,ts,nonce) triple seen before
        // inside the window is a replay -> 403.
        if (!nonce.empty() && nonceReplayOrInsert(signedData, reqTs)) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"replay detected (nonce already used)\"}",g_currentOrigin);
        }
    } else if (hasSig && hasTs) {
        // P1-11 (fail-closed): HMAC not REQUIRED by config, but the client DID
        // present a timestamped signature. We must still VERIFY it: if the
        // signature is present but wrong, reject (downgrade/replay defense).
        // The old code discarded verifyCommandHmac()'s return value here, so a
        // client with a stale/tampered signature sailed through.
        std::string signedData = actualCmd + ".ts=" + std::to_string(reqTs);
        if (!nonce.empty()) signedData += ".nonce=" + nonce;
        if (!verifyCommandHmac(signedData.c_str(), sig.c_str())) {
            auditCommand(actualCmd, g_peerUid, false, "webui",
                         "hmac_required=false but presented signature failed verification");
            return buildHttpResponse(403, "application/json",
                "{\"ok\":false,\"error\":\"presented signature failed verification\"}",
                g_currentOrigin);
        }
        // F-A6-01[P2]: in downgrade mode a captured (cmd,ts,sig,nonce) triple must
        // NOT be replayable. The required-HMAC path enforces a ±300s window and
        // consumes a one-time nonce; this branch used to skip BOTH, so an attacker
        // who captured one signed write could replay it forever (flip profile /
        // clear logs / change control mode) on the loopback server. We now apply
        // the SAME replay protection here:
        //   1) reject timestamps outside the ±300s window;
        //   2) a state-changing command MUST carry a one-time .nonce we consume.
        const int64_t HMAC_REPLAY_WINDOW_SEC = 300;
        int64_t nowTs = (int64_t)time(nullptr);
        int64_t ageDowngrade = nowTs - reqTs;
        if (ageDowngrade > HMAC_REPLAY_WINDOW_SEC || ageDowngrade < -HMAC_REPLAY_WINDOW_SEC) {
            auditCommand(actualCmd, g_peerUid, false, "webui",
                         "hmac_required=false: signed command timestamp out of replay window");
            return buildHttpResponse(403, "application/json",
                "{\"ok\":false,\"error\":\"signed command timestamp expired or out of window (replay protection)\"}",
                g_currentOrigin);
        }
        if (!isReadOnlyControlCommand(actualCmd) && nonce.empty()) {
            auditCommand(actualCmd, g_peerUid, false, "webui",
                         "hmac_required=false: signed write requires a one-time nonce");
            return buildHttpResponse(403, "application/json",
                "{\"ok\":false,\"error\":\"signed state-changing command requires a one-time .nonce for replay protection\"}",
                g_currentOrigin);
        }
        if (!nonce.empty() && nonceReplayOrInsert(signedData, reqTs)) {
            auditCommand(actualCmd, g_peerUid, false, "webui",
                         "hmac_required=false: replay detected (nonce already used)");
            return buildHttpResponse(403, "application/json",
                "{\"ok\":false,\"error\":\"replay detected (nonce already used)\"}",
                g_currentOrigin);
        }
        sigAuthenticated = true;
    } else if (hasSig) {
        // P1-11: signature present without timestamp. A bare signature over the
        // command alone cannot bind a replay window or consume a nonce, so it is
        // inherently replayable. In downgrade mode we therefore REFUSE to let it
        // authorize a state-changing command (F-A6-01). Read-only probes do not
        // mutate state, so a bare signature there is harmless.
        if (!verifyCommandHmac(actualCmd.c_str(), sig.c_str())) {
            auditCommand(actualCmd, g_peerUid, false, "webui",
                         "hmac_required=false but unsigned-timestamp signature failed");
            return buildHttpResponse(403, "application/json",
                "{\"ok\":false,\"error\":\"presented signature failed verification\"}",
                g_currentOrigin);
        }
        if (!isReadOnlyControlCommand(actualCmd)) {
            auditCommand(actualCmd, g_peerUid, false, "webui",
                         "hmac_required=false: bare (no .ts=/.nonce=) signature cannot authorize a write");
            return buildHttpResponse(403, "application/json",
                "{\"ok\":false,\"error\":\"signed write must bind .ts= and a one-time .nonce for replay protection\"}",
                g_currentOrigin);
        }
        sigAuthenticated = true;
    }

    // LED-158 (SEC-007): hmacRequired=false must NOT leave /control as an open
    // control plane. The CSRF gate below only blocks cross-ORIGIN *browser* requests;
    // a local native app can freely send Content-Type: application/json (which the
    // CSRF gate treats as "safe"), so a CSRF-safe content-type is NOT authentication.
    // When the operator has disabled HMAC, a state-changing command still requires a
    // verified signature or a valid session bearer token — an unsigned application/json
    // POST is rejected (403) rather than enqueued. Read-only probes stay allowed.
    if (!hmacRequired && !sessionAuthenticated && !sigAuthenticated &&
        !isReadOnlyControlCommand(actualCmd)) {
        return buildHttpResponse(403, "application/json",
            "{\"ok\":false,\"error\":\"hmac_required=false: state-changing /control requires a verified signature or session token (open control plane disallowed)\"}",
            g_currentOrigin);
    }

    // W-002 / P2-4: CSRF guard. A browser can be made to issue a cross-origin GET
    // (via <img>, <script>, window.open, or a plain link) OR a cross-origin POST
    // form (text/plain / urlencoded, no preflight) but cannot forge an HMAC
    // signature, set a bearer token, set an X-Daemon-Token header, or send an
    // application/json body from an unallowlisted origin. So when the request is
    // NOT CSRF-safe (GET always unsafe; POST unsafe unless app/json / daemon-token
    // / allowlisted-Origin) AND carries no valid .sig / .stok, reject state-changing
    // commands with 405. Read-only probes are still allowed.
    //
    // HTTP-05 (P3, audit 2026-10-26): this branch is currently UNREACHABLE by
    // construction, and is deliberately RETAINED as a defense-in-depth backstop,
    // not deleted. Proof of subsumption:
    //   * hmacRequired=true: the block at the "hmacRequired && !sessionAuthenticated"
    //     arm returns 403 on every failure path; reaching this point means either
    //     sessionAuthenticated==true (X-Daemon-Token) or sigAuthenticated==true
    //     (verified HMAC) — so the conjunction below cannot hold.
    //   * hmacRequired=false: the LED-158 gate immediately above returns 403 for any
    //     !sessionAuthenticated && !sigAuthenticated && !readOnly write; reaching
    //     this point means authenticated OR read-only — again the conjunction
    //     cannot hold.
    // If either upstream gate is ever weakened/refactored, this 405 is the second
    // line that still refuses a cross-origin browser write. Do not "tidy" it away
    // without restoring the CSRF coverage it represents.
    if (!csrfSafe && !sessionAuthenticated && !sigAuthenticated &&
        !isReadOnlyControlCommand(actualCmd)) {
        return buildHttpResponse(405, "application/json",
            "{\"ok\":false,\"error\":\"state-changing command requires a CSRF-safe request (valid HMAC signature / session token, or application/json / X-Daemon-Token / allowlisted Origin)\"}",
            g_currentOrigin);
    }

    // round-4 (P1 #1/#2): enqueue into the real command queue directory
    // (/dev/aatpe/commands/) instead of overwriting the single control_cmd mailbox.
    // The response now carries a command_id the client can poll via
    // GET /api/command/<id>. "ok":true means the command was QUEUED, not yet
    // executed; status:"queued" reflects that. On queue-full we return 503
    // (ERR_BUSY from the non-blocking tryPush path: service temporarily busy,
    // client SHOULD retry) — NOT 429, which is rate-limiting semantics.
    aatpe_cmdq::EnqueueResult eq = aatpe_cmdq::commandQueueEnqueue(actualCmd);
    // P3-42: audit the dispatched privileged command (profile/thermal/migration/
    // ML/control-mode/federated). Best-effort append to STATE_DIR/audit.log.
    // source=webui; reason carries the queue failure message on rejection.
    auditCommand(actualCmd, g_peerUid, eq.ok, "webui",
                 eq.ok ? std::string() : eq.message);
    if (!eq.ok) {
        int httpStatus = (eq.error_code == aatpe::ERR_BUSY) ? 503 : 500;
        // P1-7: eq.message is free-form error text; escape before embedding in
        // JSON so a message containing a quote/backslash cannot break out of
        // the JSON string and inject keys.
        std::string body = "{\"ok\":false,\"status\":\"rejected\",\"error_code\":" +
                           std::to_string(eq.error_code) + ",\"error\":\"" +
                           jsonEscape(eq.message) + "\"}";
        return buildHttpResponse(httpStatus, "application/json", body, g_currentOrigin);
    }
    // P1-7: actualCmd is user-supplied wire text; escape it in the echo field.
    std::string resp = "{\"ok\":true,\"status\":\"queued\",\"command_id\":\"" + eq.command_id +
                        "\",\"command\":\"" + jsonEscape(actualCmd) +
                        "\",\"hmac_required\":" + (hmacRequired ? "1" : "0") +
                        ",\"session_auth\":" + (sessionAuthenticated ? "1" : "0") + "}";
    return buildHttpResponse(200,"application/json",resp,g_currentOrigin);
}

// #61: GET/POST /api/session — exchange a valid HMAC-signed proof for a short-lived
// bearer session token. The request itself must be authenticated exactly like a
// /control command (we reuse handleControl-style HMAC verification by requiring a
// signed "session_issue" command). To keep the surface minimal, the client sends
//   /api/session?cmd=session_issue.ts=<ts>.sig=<sig>
// where the signed data is "session_issue.ts=<ts>". A successful issue returns
// {"ok":true,"token":"<64hex>","expires_in":300}. The long-term HMAC path stays
// available; this is purely an additive convenience.
std::string handleSession(const std::string& request, const std::string& fullPath) {
    // Reuse the same daemon-token/HMAC gating as hmac_key: on TCP loopback we
    // require a valid HMAC-signed command. We parse ?cmd= and verify it with the
    // same verifyCommandHmac() used by /control.
    std::string cmd;
    size_t qpos = fullPath.find('?');
    if (qpos != std::string::npos) {
        std::string qs = fullPath.substr(qpos + 1);
        // P3-12: exact k=v parse (queryParamEq) — the old qs.find("cmd=") matched
        // "xcmd=<evil>" as a prefix.
        queryParamEq(qs, "cmd", cmd);
    }
    // R4-C07: POST body branch removed. The routing layer returns 405 for any
    // non-GET method on /api/session (see routeSession gate), so this body
    // fallback was unreachable dead code. Session bootstrap is GET-only by design
    // (frontend uses /api/session?cmd=session_issue.ts=...&sig=...).
    // URL-decode the cmd.
    cmd = urlDecode(cmd);
    while (!cmd.empty() && (cmd.back()=='\n'||cmd.back()=='\r'||cmd.back()==' '||cmd.back()=='\t')) cmd.pop_back();

    // Parse .ts=/.sig= from the offered proof.
    std::string actualCmd = cmd, sig;
    int64_t reqTs = 0; bool hasSig=false, hasTs=false;
    size_t sigPos = actualCmd.rfind(".sig=");
    if (sigPos != std::string::npos && sigPos > 0) {
        sig = actualCmd.substr(sigPos+5); actualCmd = actualCmd.substr(0,sigPos); hasSig=true;
    }
    size_t tsPos = actualCmd.rfind(".ts=");
    if (tsPos != std::string::npos && tsPos > 0) {
        try { reqTs = std::stoll(actualCmd.substr(tsPos+4)); hasTs=true; }
        catch(...) { hasTs=false; LOG_DEBUG("http: bad cmd .ts offset, ignoring"); }
        actualCmd = actualCmd.substr(0,tsPos);
    }
    // W-006: parse .nonce=<hex> and bind it into the signed data, then consume it
    // through the shared nonce replay cache. Without this, a captured
    // "session_issue.ts=<ts>.sig=<sig>" proof could be replayed verbatim inside the
    // ±300s window to mint a second bearer token.
    std::string nonce;
    size_t noncePos = actualCmd.rfind(".nonce=");
    if (noncePos != std::string::npos && noncePos > 0) {
        nonce = actualCmd.substr(noncePos+7);
        actualCmd = actualCmd.substr(0, noncePos);
    }
    bool hmacRequired;
    { std::lock_guard<std::mutex> cfgLock(g_cfgMutex); hmacRequired = g_cfg.hmacRequired; }
    if (isHmacReadonly()) {
        return buildHttpResponse(403,"application/json","{\"ok\":false,\"error\":\"read-only mode\"}",g_currentOrigin);
    }
    bool authed = false;
    // G8-001 (ROUND-001): when HMAC auth is disabled (HMAC_REQUIRED=0), do NOT
    // auto-authenticate session minting. Previously any loopback app could POST
    // /api/session with an empty body and receive a 5-minute bearer stok, which
    // then satisfied LED-158's "sessionAuthenticated" bypass on /control —
    // completely defeating the degraded-mode open-control-plane defense.
    //
    // Fix: in the no-HMAC mode, session minting requires the root-minted daemon
    // token (X-Daemon-Token header or trusted-channel ?token=). The legitimate
    // WebUI reads the 0600 token via the KSU/Magisk root exec bridge, so this
    // does not break the UI; an ordinary local third-party app cannot read the
    // token and therefore cannot mint a stok. If the operator has intentionally
    // disabled HMAC, they do not need a session bearer (there is nothing to
    // sign), so the stok is purely an additional credential surface that must
    // not be freely mintable.
    if (!hmacRequired) {
        // fall through to the daemon-token check below; authed stays false
        // until checkDaemonTokenAuth() passes.
    } else if (hasSig && hasTs && actualCmd == "session_issue") {
        if (nonce.empty()) {
            // W-006: require a fresh nonce when HMAC auth is on, so the proof cannot
            // be replayed. Older clients that omit .nonce get a clear 403.
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"session_issue requires .nonce=<hex> replay protection\"}",
                g_currentOrigin);
        }
        int64_t now = (int64_t)time(nullptr);
        int64_t age = now - reqTs;
        if (age <= 300 && age >= -300) {
            std::string signedData = actualCmd + ".ts=" + std::to_string(reqTs) + ".nonce=" + nonce;
            authed = verifyCommandHmac(signedData.c_str(), sig.c_str());
            // Consume the nonce ONLY after a valid signature (don't let an
            // unauthenticated client poison the replay cache).
            if (authed && nonceReplayOrInsert(signedData, reqTs)) {
                return buildHttpResponse(403,"application/json",
                    "{\"ok\":false,\"error\":\"replay detected (session proof already used)\"}",
                    g_currentOrigin);
            }
        }
    }
    // P1-15: also allow a valid daemon token (sent via X-Daemon-Token header,
    // never in the URL) to mint a short-lived SSE ticket. This lets the WebUI
    // bootstrap SSE without carrying the long-lived daemon token in the EventSource
    // URL (which leaks into logs/history).
    if (!authed && checkDaemonTokenAuth(request, fullPath)) authed = true;
    if (!authed) {
        // G8-001: when HMAC is off, the error explicitly says the loopback
        // control plane is open and the operator should re-enable HMAC.
        if (!hmacRequired) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"hmac_required=false: /api/session requires the daemon token (root bridge); loopback control plane is open to any local app — set HMAC_REQUIRED=1\"}",
                g_currentOrigin);
        }
        return buildHttpResponse(403,"application/json",
            "{\"ok\":false,\"error\":\"HMAC authentication required to issue session\"}",g_currentOrigin);
    }
    std::string tok = aatpeSessionIssue();
    if (tok.empty()) {
        return buildHttpResponse(500,"application/json","{\"ok\":false,\"error\":\"session issue failed\"}",g_currentOrigin);
    }
    // SEC-08 (P3): the STATE_DIR/hmac_session_active WebUI badge has a SINGLE
    // owner — the daemon main loop (aatped_main.cpp), which recomputes
    // aatpeSessionAnyActive() fresh every cycle and writes "1"/"0". This HTTP
    // handler USED to write "1" immediately on issue, creating a second writer:
    // the main loop could then clobber that fresh "1" with a stale "0" sampled
    // in the microsecond window between its AnyActive() call and writeFile().
    // The badge is purely cosmetic (token verification itself never reads this
    // file; it consults the in-memory slot table under g_sessionMutex), so we
    // let the next main-loop cycle (sub-second) flip it to "1" rather than keep
    // a second writer that could race.
    std::string body = "{\"ok\":true,\"token\":\"" + tok + "\",\"expires_in\":300}";
    return buildHttpResponse(200,"application/json",body,g_currentOrigin);
}

// v94-sec: /control command whitelist.
// Only a known set of commands is accepted. Parses the command's key (part before
// '=') and validates the value for parameterized commands. Any command not in the
// whitelist is rejected by handleControl() with 400. This provides a baseline
// defense-in-depth layer in addition to HMAC authentication.
//
// NOTE: hmacRequired defaults to true (secure-by-default; see aatped_types.h and
//       aatped_hmac.cpp).  Operators who explicitly set HMAC_REQUIRED=0 in
//       aatpe.conf to disable auth should understand that the whitelist below is
//       only a coarse command-level guard, not a substitute for request authentication.
bool isCommandAllowed(const std::string& rawCmd) {
    std::string cmd = rawCmd;
    // trim whitespace
    while (!cmd.empty() && (cmd.back()=='\n'||cmd.back()=='\r'||cmd.back()==' '||cmd.back()=='\t')) cmd.pop_back();
    while (!cmd.empty() && (cmd.front()==' '||cmd.front()=='\t')) cmd.erase(cmd.begin());
    if (cmd.empty()) return false;

    // Split into key/value at first '='
    size_t eq = cmd.find('=');
    std::string key = (eq == std::string::npos) ? cmd : cmd.substr(0, eq);
    std::string value = (eq == std::string::npos) ? std::string() : cmd.substr(eq + 1);

    // Commands with no value (or value already folded into key list below).
    // value-less commands: restart, stop, clear_log, reload_config, unlock_watchdog
    // W-001: unlock_watchdog was accepted by aatped_control.cpp but missing here,
    // so the WebUI unlock button was rejected by the whitelist before reaching the
    // dispatcher. Align with the CLI/control-pipe surface.
    if (eq == std::string::npos) {
        if (key == "restart" || key == "stop" || key == "clear_log" ||
            key == "reload_config" || key == "unlock_watchdog") return true;
        return false;
    }

    // Whitelisted keys whose value must be one of a fixed enum / numeric set.
    if (key == "set_mode") {
        return value == "assist" || value == "passive" || value == "active";
    }
    if (key == "set_profile") {
        return value == "balanced" || value == "gaming" ||
               value == "battery" || value == "thermal";
    }
    if (key == "set_auto_profile") {
        return value == "1" || value == "0";
    }
    if (key == "set_log_level") {
        return value == "0" || value == "1" || value == "2" ||
               value == "3" || value == "4" || value == "5";
    }
    if (key == "model_reset" || key == "model_save") {
        return value == "1";
    }
    if (key == "set_ml_hybrid" || key == "set_ml_cold_start" ||
        key == "set_ml_learning" || key == "battery_aging" ||
        key == "set_thermal_enabled" ||
        key == "set_c4_freezer" || key == "set_ebpf" || key == "set_federated") {
        // P1 round5d: round4c feature toggles were missing from this whitelist, so
        // WebUI clicks (HMAC path) got 400 "command not in whitelist" even though
        // the root pipe accepted them. Strict 0/1 (matches aatped_control P3-2).
        return value == "1" || value == "0";
    }
    if (key == "explain_tid") {
        // P25: value must be a pure (non-empty) integer within a sane pid/tid range.
        // Kernel TIDs are <= 65535 in the classic pid namespace; 0 disables explain.
        // Reject absurd lengths (leading-zero DoS / overflow) before parsing.
        if (value.empty() || value.size() > 6) return false;
        for (char c : value) {
            if (c < '0' || c > '9') return false;
        }
        errno = 0;
        char* endp = nullptr;
        long tid = strtol(value.c_str(), &endp, 10);
        if (errno != 0 || !endp || *endp != '\0') return false;
        return tid >= 0 && tid <= 65535;
    }
    // v94-fix: numeric-range validation helper for runtime-tunable config commands.
    // Parses a non-negative integer value and checks it falls within [lo, hi].
    auto inRange = [](const std::string& v, long lo, long hi) -> bool {
        if (v.empty() || v.size() > 6) return false;
        for (char c : v) {
            if (c < '0' || c > '9') return false;
        }
        errno = 0;
        char* endp = nullptr;
        long n = strtol(v.c_str(), &endp, 10);
        if (errno != 0 || !endp || *endp != '\0') return false;
        return n >= lo && n <= hi;
    };
    if (key == "set_decision_interval") {
        // DECISION_INTERVAL: 10-5000 ms (aatped_control.cpp:897)
        return inRange(value, 10, 5000);
    }
    if (key == "set_confidence_threshold") {
        // CONFIDENCE_THRESHOLD: 1-100 (aatpe.conf default 5)
        return inRange(value, 1, 100);
    }
    if (key == "set_migration_cost_threshold") {
        // MIGRATION_COST_THRESHOLD: WebUI whitelist 1-100 (aatpe.conf default 3).
        // P3-1: WebUI is intentionally STRICTER than aatped_control.cpp clamp
        // (0-10000) — the wider clamp is for CLI/conf hot-reload, the WebUI only
        // exposes the sane 1-100 band. Do NOT widen this to 10000.
        return inRange(value, 1, 100);
    }
    if (key == "set_max_migrations") {
        // MAX_MIGRATIONS_PER_SEC: 1-1000 (aatped_control.cpp:971)
        return inRange(value, 1, 1000);
    }
    if (key == "set_thermal_limit_cpu") {
        // THERMAL_LIMIT_CPU: 0-200 (aatped_control.cpp:1020)
        return inRange(value, 0, 200);
    }
    if (key == "set_thermal_limit_skin") {
        // THERMAL_LIMIT_SKIN: 0-200 (aatped_control.cpp:1038)
        return inRange(value, 0, 200);
    }
    if (key == "set_thermal_limit_battery") {
        // THERMAL_LIMIT_BATTERY: 0-200 (aatped_control.cpp:1063)
        return inRange(value, 0, 200);
    }
    if (key == "set_thermal_emergency") {
        // THERMAL_EMERGENCY: 0-200 (aatped_control.cpp:1090)
        return inRange(value, 0, 200);
    }
    // v4.2.1: manual game-override list. Value must be a syntactically valid
    // Android package name ([A-Za-z0-9_.], leading alpha, no traversal). Same
    // validator the daemon write path uses — rejects shell/path metacharacters.
    if (key == "game_add" || key == "game_remove") {
        if (value.empty() || value.size() > 256) return false;
        if (!isalpha((unsigned char)value[0])) return false;
        if (value.front() == '.' || value.back() == '.') return false;
        if (value.find("..") != std::string::npos) return false;
        for (char c : value) {
            if (isalnum((unsigned char)c)) continue;
            if (c == '.' || c == '_') continue;
            return false;   // reject ; | & $ ` space / quote etc.
        }
        return true;
    }
    return false;
}

// P3-40: single-key lookup against the unified daemon->snapshot->WebUI cache.
// No WebUI endpoint may open()/read() a STATE_DIR file directly; every observable
// value comes from stateCacheSnapshot() (which is kept byte-for-byte in sync with
// disk by writeFile()/writeFileAtomic()). Returns "" when the key is absent.
static std::string stateSnapshotLookup(const std::string& key) {
    if (!stateCacheIsSeeded()) stateCacheSeedFromDir(STATE_DIR);
    std::vector<std::pair<std::string,std::string>> snap = stateCacheSnapshot();
    for (const auto& kv : snap) {
        if (kv.first == key) return kv.second;
    }
    return std::string();
}

// P2-fix: /api/fusion — returns decision fusion average weights from the unified snapshot.
std::string handleFusion() {
    std::string value = stateSnapshotLookup("fusion_weights");
    if (value.empty()) {
        value = "{\"heuristic_weight\":0,\"ml_weight\":0,\"hybrid_weight\":0,\"history_weight\":0,\"total_decisions\":0}";
    }
    return buildHttpResponse(200, "application/json", value, g_currentOrigin);
}

// v4.2.1: /api/apps — installed third-party packages (read-only JSON) for the
// WebUI "游戏管理" picker. Returns {"apps":[{"pkg":"...","label":""}],"manual":[...]}.
// label is reserved (empty for now; pkg is what users recognise, e.g.
// com.tencent.tmgp.sgame). NOT pushed into the state snapshot (avoid bloat).
std::string handleApps() {
    // v4.4.1 P0 (RC-2c): enumerate ALL users (work profile / user 10/99). The old
    // listInstalledPackages() relied on `pm list packages`, which by default only
    // returns user 0 — work-profile apps never appeared in the add picker, so the
    // user literally could not add them. AllUsers reads the global packages.list.
    // Fall back to the legacy enumeration only if the all-users scan comes back
    // empty (fail-open, never regresses the normal user0 path).
    std::vector<std::string> pkgs = aatpe::apklabel::listInstalledPackagesAllUsers();
    if (pkgs.empty()) pkgs = aatpe::apps::listInstalledPackages();
    std::string body = "{\"apps\":[";
    for (size_t i = 0; i < pkgs.size(); ++i) {
        if (i) body += ",";
        // Non-blocking: only read the already-warm cache. First poll returns the
        // bare package name; the background frame-loop warmer fills the cache, and
        // the next 2-3s poll resolves the Chinese label. Never parses an APK on the
        // HTTP request thread (hundreds of packages would stall the response).
        std::string lab = aatpe::apklabel::peekCachedLabel(pkgs[i]);
        if (lab.empty()) lab = pkgs[i];
        body += "{\"pkg\":\"" + jsonEscape(pkgs[i]) + "\",\"label\":\"" + jsonEscape(lab) + "\"}";
    }
    body += "],\"manual\":[";
    // current manual list -> array
    std::string manual = aatpe::gamelist::currentManualGameList();
    std::istringstream ss(manual);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        // trim
        size_t a = line.find_first_not_of(" \t\r\n");
        size_t b = line.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        line = line.substr(a, b - a + 1);
        if (!first) body += ",";
        body += "\"" + jsonEscape(line) + "\"";
        first = false;
    }
    body += "]}";
    // P0: when every enumeration path failed, tell the WebUI WHY (root /data/app
    // scan + pm + cmd all returned nothing) instead of a silent empty list.
    if (pkgs.empty()) {
        std::string note = "empty: pm/cmd/and /data/app scan all returned no packages (check root, SELinux)";
        body.insert(body.size() - 1, ",\"note\":\"" + note + "\"");
    }
    return buildHttpResponse(200, "application/json", body, g_currentOrigin);
}

// P2-fix: /api/frame_time — frame-time outcome counters + snapshot (from unified snapshot).
std::string handleFrameTime() {
    auto readIntState = [&](const char* name) -> long long {
        std::string v = stateSnapshotLookup(name);
        if (v.empty()) return 0;
        try { return std::stoll(v); } catch (...) { LOG_DEBUG("http: bad int state '%s'=%s ->0", name, v.c_str()); return 0; }
    };
    auto readFloatState = [&](const char* name) -> float {
        std::string v = stateSnapshotLookup(name);
        if (v.empty()) return 0.0f;
        try { return std::stof(v); } catch (...) { LOG_DEBUG("http: bad float state '%s'=%s ->0", name, v.c_str()); return 0.0f; }
    };
    std::ostringstream body;
    body << "{";
    body << "\"samples\":" << readIntState("ft_outcome_samples");
    body << ",\"improved\":" << readIntState("ft_outcome_improved");
    body << ",\"degraded\":" << readIntState("ft_outcome_degraded");
    body << ",\"fallback\":" << readIntState("ft_outcome_fallback");
    body << ",\"gfxinfo_calls\":" << readIntState("ft_gfxinfo_calls");
    body << ",\"gfxinfo_cache_hits\":" << readIntState("ft_gfxinfo_cache_hits");
    body << ",\"frame_time_avg\":" << readFloatState("frame_time_avg");
    body << ",\"frame_time_p95\":" << readFloatState("frame_time_p95");
    body << ",\"frame_time_p99\":" << readFloatState("frame_time_p99");
    body << "}";
    return buildHttpResponse(200, "application/json", body.str(), g_currentOrigin);
}

// W2: /api/frame_apps — per-app rolling frame stats (MRU order). Gate behind
// daemon token exactly like /api/frame_time (LED-155); NOT public.
std::string handleFrameApps() {
    std::string body = aatpe_buildFrameAppsJson();
    return buildHttpResponse(200, "application/json", body, g_currentOrigin);
}

// P0: GET /api/diag_bundle — root-packs a FIXED WHITELIST of diagnostic files
// into a tar.gz and returns it. Session-token gated (same as /api/frame_apps).
// HARD invariants (no traversal, no user-controlled path):
//   - NO query/body/path parameter is ever read — every path is a compile-time
//     literal. argv exec (no shell) so no expansion.
//   - Missing files are skipped by tar; only the fixed list is packed.
//   - Output written to a fixed tmp path, read back, size-capped, then unlinked.
// If anything fails, fall through to 204 so the UI uses its root-command fallback.
// F-A6-02[P2]: diag bundle output must NOT go to the world-writable
// /data/local/tmp (a local app could pre-plant a symlink and induce root tar to
// write through it onto an attacker-chosen target). We write into the private
// 0700 STATE_DIR, drop any stale entry, then create the output atomically with
// O_CREAT|O_EXCL|O_NOFOLLOW so we hand tar a regular file we own — never a path
// an attacker controls. Returns false (fail-closed) if the safe create fails.
static std::string diagBundleOutPath() {
    return std::string(STATE_DIR) + "/aatpe-diag.tgz";
}
static bool diagBundlePrepareOutput(const std::string& path) {
    // unlink() removes the symlink ITSELF (not its target); this also clears a
    // stale bundle from a prior run. In the 0700 STATE_DIR only root can plant a
    // symlink, but O_EXCL|O_NOFOLLOW below closes the door regardless.
    (void)::unlink(path.c_str());
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;  // do NOT run tar against an unsafe path
    ::close(fd);
    return true;
}
std::string handleDiagBundle() {
    std::string outPath = diagBundleOutPath();
    if (!diagBundlePrepareOutput(outPath)) {
        return buildHttpResponse(204, "application/json", "", g_currentOrigin);
    }
    // fixed absolute whitelist (missing files -> tar skips them).
    // F-01[P1]: do NOT recurse /dev/aatpe/state (it contains daemon_token,
    // hmac_key, session_token). Only pack named diagnostic files; any
    // *token*/*key*/hmac*/*session* path is explicitly excluded.
    std::vector<std::string> argv = {
        "/system/bin/tar", "-czf", outPath,
        "/data/adb/modules/aatpe/aatped.log",
        "/data/adb/modules/aatpe/service.log",
        "/dev/aatpe/state/frame_apps.json", // per-app frame stats (incl VM attribution)
        "/dev/aatpe/state/audit.log",       // full command audit log
        "/data/tombstones/tombstone_00",
    };
    int st = -1;
    (void)aatpeExecArgv(argv, "DIAG_BUNDLE", &st, true);
    std::string bytes = readFileToString(outPath);
    (void)::unlink(outPath.c_str());
    const size_t kMaxDiag = size_t(2u) * 1024u * 1024u;  // 2MB cap
    if (bytes.empty() || bytes.size() > kMaxDiag) {
        return buildHttpResponse(204, "application/json", "", g_currentOrigin);
    }
    return buildHttpResponse(200, "application/gzip", bytes, g_currentOrigin);
}

// ROUND-004: /api/config_meta — static map of config key -> hot-reload class.
// Returns ONLY key names and their reload class (live/hook/boot). Never echoes
// values. The UI uses this to tag boot-class controls with a "重启后生效" badge.
// Requires daemon token (same gate as /api/fusion).
static const std::vector<std::pair<const char*, const char*>> kConfigMetaKeys = {
    {"LITTLE_POOL",               "pool"},
    {"MID_POOL",                  "pool"},
    {"BIG_POOL",                  "pool"},
    {"PRIME_POOL",                "pool"},
    {"REFRESH_RATE_EVENT_DRIVEN", "refresh"},
    {"LOG_LEVEL",                 "log"},
    {"EBPF_ENABLED",              "ebpf"},
    {"EBPF_RING_PAGES",           "ebpf"},
    {"ENABLE_HTTP_SERVER",        "http"},
};
std::string handleConfigMeta() {
    std::ostringstream body;
    body << "{\"keys\":{";
    for (size_t i = 0; i < kConfigMetaKeys.size(); i++) {
        if (i) body << ",";
        const char* key = kConfigMetaKeys[i].first;
        std::string cls = configKeyHotReloadClass(key);
        body << "\"" << key << "\":\"" << cls << "\"";
    }
    body << "}}";
    return buildHttpResponse(200, "application/json", body.str(), g_currentOrigin);
}

// ROUND-007: GET /logs — tail the daemon log file. ?tail=N (default 200).
// Requires daemon token. Returns JSON {"lines":["...","..."]}.
std::string handleLogs(const std::string& query) {
    int tailLines = 200;
    {
        std::string t;
        queryParamEq(query, "tail", t);
        if (!t.empty()) {
            try { int n = std::stoi(t); if (n > 0 && n <= 2000) tailLines = n; }
            catch(...) { LOG_DEBUG("http: bad ?tail= '%s', keeping default", t.c_str()); }
        }
    }
    std::string logPath = std::string(STATE_DIR) + "/aatpe.log";
    std::string content = readFileToString(logPath);
    if (content.empty()) {
        return buildHttpResponse(200, "application/json", "{\"lines\":[]}", g_currentOrigin);
    }
    // Split into lines, take last N.
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= content.size(); i++) {
        if (i == content.size() || content[i] == '\n') {
            lines.push_back(content.substr(start, i - start));
            start = i + 1;
        }
    }
    if ((int)lines.size() > tailLines) {
        lines.erase(lines.begin(), lines.end() - tailLines);
    }
    std::ostringstream body;
    body << "{\"lines\":[";
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) body << ",";
        body << "\"" << jsonEscape(lines[i]) << "\"";
    }
    body << "]}";
    return buildHttpResponse(200, "application/json", body.str(), g_currentOrigin);
}

// v68: /api/hmac_key interface — returns HMAC key, requires daemon token authentication.
// Daemon token is generated at startup and stored in STATE_DIR/daemon_token (0600).
// Clients must pass ?token=<daemon_token> query param or X-Daemon-Token header.
//
// P1-4: cache the token in memory. The old code readFileToString()'d the 0600
// token file on EVERY authenticated request (every SSE push, every /state/all
// poll, every /control command) — a syscall+read per request for a value that
// is generated once per boot and never rotates at runtime. We cache it behind a
// small mutex and re-stat() the file on each call; only when the mtime changes
// do we re-read. This keeps external rotation working while eliminating the
// per-request disk read.
static std::mutex g_tokenCacheMutex;
static std::string g_cachedDaemonToken;
static long long g_cachedTokenMtimeSec = 0;

static std::string getOrCreateDaemonToken() {
    std::string tokenPath = std::string(STATE_DIR) + "/daemon_token";

    // Fast path: cache holds a token. Validate freshness via mtime.
    {
        std::lock_guard<std::mutex> lk(g_tokenCacheMutex);
        if (!g_cachedDaemonToken.empty()) {
            struct stat st;
            if (stat(tokenPath.c_str(), &st) == 0 &&
                (long long)st.st_mtime == g_cachedTokenMtimeSec) {
                return g_cachedDaemonToken;
            }
            // mtime changed or file vanished: fall through to re-read/generate.
        }
    }

    std::string token = readFileToString(tokenPath);
    while (!token.empty() && (token.back()=='\n'||token.back()=='\r'||token.back()==' ')) token.pop_back();
    if (token.size() >= 64) {
        struct stat st;
        long long mt = (stat(tokenPath.c_str(), &st) == 0) ? (long long)st.st_mtime : 0;
        std::lock_guard<std::mutex> lk(g_tokenCacheMutex);
        g_cachedDaemonToken = token;
        g_cachedTokenMtimeSec = mt;
        return token;
    }
    // v94-sec: Generate a cryptographically-strong token from /dev/urandom.
    unsigned char buf[64];
    int urfd = open("/dev/urandom", O_RDONLY);
    if (urfd < 0) {
        LOG_ERROR("daemon token: open(/dev/urandom) failed (%s); refusing to fall back to weak RNG",
                  aatpeErrStr(errno));
        return std::string();
    }
    ssize_t nread = read(urfd, buf, sizeof(buf));
    close(urfd);
    if (nread != (ssize_t)sizeof(buf)) {
        LOG_ERROR("daemon token: short read from /dev/urandom (%zd of %zu); refusing to fall back to weak RNG",
                  nread, sizeof(buf));
        return std::string();
    }
    static const char hex[] = "0123456789abcdef";
    token.clear();
    token.reserve(128);
    for (int i = 0; i < 64; i++) {
        token += hex[(buf[i] >> 4) & 0xF];
        token += hex[buf[i] & 0xF];
    }
    // G8-005/G3-002: O_NOFOLLOW | O_CLOEXEC on the daemon token file.
    int fd = open(tokenPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        // P1-9: loop write() until every byte is persisted; fsync before close
        // so a crash cannot leave a truncated/partial token file that the next
        // boot would fail to parse.
        size_t off = 0;
        size_t len = token.size();
        bool ok = true;
        while (off < len) {
            ssize_t w = write(fd, token.c_str() + off, len - off);
            if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
            if (w == 0) { ok = false; break; }  // P3 round7-rework
            off += (size_t)w;
        }
        if (ok) ok = (fsync(fd) == 0);
        close(fd);
        if (!ok) {
            LOG_ERROR("daemon token: failed to fully persist token file");
            return std::string();
        }
    }
    {
        struct stat st;
        long long mt = (stat(tokenPath.c_str(), &st) == 0) ? (long long)st.st_mtime : 0;
        std::lock_guard<std::mutex> lk(g_tokenCacheMutex);
        g_cachedDaemonToken = token;
        g_cachedTokenMtimeSec = mt;
    }
    return token;
}

static bool checkDaemonTokenAuth(const std::string& request, const std::string& path) {
    std::string daemonToken = getOrCreateDaemonToken();
    if (daemonToken.empty()) return false;
    // P2-6: the long-lived master credential must not travel in the URL on the
    // TCP loopback HTTP port — it leaks into browser history, proxy logs and the
    // WebView console. Accept ?token= ONLY on a trusted channel where we can
    // kernel-verify the peer UID (root/system, i.e. AF_UNIX). On plain TCP loopback
    // (g_peerUid == -1) the master token must come via the X-Daemon-Token header
    // (or a short-lived ?stok= session ticket for EventSource).
    bool trustedChannel = (g_peerUid == 0 || g_peerUid == 1000);
    if (trustedChannel) {
        // Check query param ?token= (trusted channel only).
        // Issue-12: parse query string by '&' and match parameter names EXACTLY,
        // so "xtoken=" or "nottoken=" are not falsely matched by substring find().
        size_t qp = path.find('?');
        if (qp != std::string::npos) {
            std::string query = path.substr(qp + 1);
            size_t pos = 0;
            while (pos <= query.size()) {
                size_t amp = query.find('&', pos);
                std::string pair = (amp != std::string::npos)
                    ? query.substr(pos, amp - pos)
                    : query.substr(pos);
                size_t eq = pair.find('=');
                if (eq != std::string::npos) {
                    std::string name = pair.substr(0, eq);
                    std::string val = pair.substr(eq + 1);
                    if (name == "token") {
                        if (constantTimeStringEqual(val, daemonToken)) return true;
                    }
                }
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
        }
    }
    // Check X-Daemon-Token header (accepted on ALL channels, including TCP).
    std::string lowerReq = request;
    std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
    // P1 round7: only search the HEADER block (up to CRLFCRLF), never the body.
    size_t hdrEnd = lowerReq.find("\r\n\r\n");
    if (hdrEnd != std::string::npos) lowerReq.resize(hdrEnd);
    size_t hp = lowerReq.find("\r\nx-daemon-token:");
    if (hp != std::string::npos) {
        size_t hs = hp + 17;
        size_t he = request.find("\r\n", hs);
        std::string val = (he != std::string::npos) ? request.substr(hs, he - hs) : request.substr(hs);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        while (!val.empty() && (val.back()=='\r'||val.back()==' ')) val.pop_back();
        // v94-sec: constant-time comparison to avoid token timing oracles.
        if (constantTimeStringEqual(val, daemonToken)) return true;
    }
    // F-01[P1]: stok (short-lived 5-min session ticket) is NO LONGER accepted
    // here. It leaks into browser history/proxy logs/logcat and must only be
    // accepted on the SSE route (see checkSseTicketAuth). All sensitive
    // endpoints (/api/hmac_key, /api/diag_bundle, /api/session mint, /control)
    // only accept the long-lived daemon token via X-Daemon-Token header or
    // AF_UNIX trusted-channel ?token=.
    return false;
}

// F-01[P1]: SSE-only auth. Accepts the daemon token (header or trusted
// ?token=) OR a short-lived ?stok= session ticket. Used exclusively on the
// /events, /sse, /api/events routes.
static bool checkSseTicketAuth(const std::string& request, const std::string& path) {
    if (checkDaemonTokenAuth(request, path)) return true;
    size_t qp2 = path.find('?');
    std::string query2 = (qp2 != std::string::npos) ? path.substr(qp2 + 1) : std::string();
    size_t pos = 0;
    while (pos <= query2.size()) {
        size_t amp = query2.find('&', pos);
        std::string pair = (amp != std::string::npos)
            ? query2.substr(pos, amp - pos)
            : query2.substr(pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string name = pair.substr(0, eq);
            std::string val = pair.substr(eq + 1);
            if (name == "stok") {
                if (aatpeSessionVerify(val)) return true;
            }
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return false;
}

// v68: /api/hmac_key interface — returns HMAC key, requires daemon token authentication.
std::string handleHmacKey(const std::string& request, const std::string& fullPath) {
    // v68: Require daemon token authentication before returning HMAC key
    if (!checkDaemonTokenAuth(request, fullPath)) {
        return buildHttpResponse(401, "application/json",
            "{\"ok\":false,\"error\":\"daemon token authentication required\"}", g_currentOrigin);
    }
    // v94 P2 问题18: Return the IN-MEMORY g_hmacKey as the single source of truth,
    // not re-read from a file. The old code opened STATE_DIR/hmac_key (which is a
    // different path than the key actually persisted to by saveHmacKey:
    // /dev/aatpe/hmac_key), then generated a *different* random key when the file
    // was missing — causing signature mismatch between WebUI and daemon.
    // P1-4: read g_hmacKey / g_hmacActive under g_hmacMutex — the main daemon loop
    // calls checkHmacKeyRotation() which rewrites g_hmacKey.
    std::string key;
    bool hmacActive;
    {
        std::lock_guard<std::mutex> hmacLk(g_hmacMutex);
        key = g_hmacKey;
        hmacActive = g_hmacActive;
    }
    while (!key.empty() && (key.back()=='\n'||key.back()=='\r')) key.pop_back();
    // v94 P2 问题16: snapshot hmacRequired under g_cfgMutex (HTTP thread reads,
    // main thread writes via loadConfig).
    bool hmacRequired;
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        hmacRequired = g_cfg.hmacRequired;
    }
    // v94 P2 问题18: expose runtime active status alongside required flag so the
    // WebUI sees the same value as state.json "hmac.active".
    // P3-15: jsonEscape the key before embedding — although the key is generated
    // from hex chars (safe), defense-in-depth against a future key alphabet change.
    std::string resp = "{\"hmac_key\":\"" + jsonEscape(key) + "\",\"required\":" +
                        (hmacRequired ? "1" : "0") +
                        ",\"active\":" + (hmacActive ? "1" : "0") + "}";
    return buildHttpResponse(200,"application/json",resp,g_currentOrigin);
}

// v94-fix: /api/daemon_token — returns the daemon token over HTTP.
// This endpoint does NOT require authentication by design: the HTTP server
// only listens on 127.0.0.1 (loopback), so local access is inherently safe.
// The purpose is to let the WebUI bootstrap when opened via file:// protocol
// (KernelSU/Magisk WebUI entry), where the KSU exec API (smartExec) is not
// available and the frontend cannot `cat /dev/aatpe/daemon_token` directly.
// Flow: fetch /api/daemon_token -> get token -> fetch /api/hmac_key?token=xxx
//       -> get HMAC key -> sign commands -> send to /control.
// This breaks the chicken-and-egg bootstrapping problem without weakening
// HMAC enforcement on /control (hmacRequired stays true).
// 问题10 fix: handleDaemonToken() is no longer routed (the HTTP endpoint now
// returns 403 with a "read the root-owned file" message). Kept around as
// dead-but-referenced documentation of the original design; [[maybe_unused]]
// silences -Wunused-function without deleting the implementation.
// R4-C06: confirmed dead route — /api/daemon_token always returns 403 by design.
// Preserved for backwards-compat with older WebUI builds that might probe it.
[[maybe_unused]]
std::string handleDaemonToken() {
    // fix-v96 #3: identity is the KERNEL-VERIFIED peer UID, not the HTTP Origin.
    // A local App can connect to 127.0.0.1 and send any Origin header it likes,
    // so `Origin: http://127.0.0.1` / `Origin: null` must NOT be trusted as auth.
    //
    // Accept ONLY a trusted peer (root or system) on a loopback/trusted channel:
    //   * g_peerIsLoopback  -> network layer is local (defense in depth)
    //   * peerUidIsTrusted(g_peerUid) -> the caller's real UID is root/system.
    //     g_peerUid is -1 on a plain TCP loopback socket (SO_PEERCRED is
    //     AF_UNIX-only), so such a connection is refused here. The bootstrap
    //     token must be obtained over the root module path / trusted IPC.
    if (!g_peerIsLoopback) {
        return buildHttpResponse(403, "application/json",
            "{\"ok\":false,\"error\":\"loopback connection required\"}", g_currentOrigin);
    }
    if (!peerUidIsTrusted(g_peerUid)) {
        return buildHttpResponse(403, "application/json",
            "{\"ok\":false,\"error\":\"trusted caller (root/system) required; Origin is not authentication\"}",
            g_currentOrigin);
    }
    std::string token = getOrCreateDaemonToken();
    while (!token.empty() && (token.back()=='\n'||token.back()=='\r'||token.back()==' ')) token.pop_back();
    std::string resp = "{\"daemon_token\":\"" + token + "\"}";
    return buildHttpResponse(200, "application/json", resp, g_currentOrigin);
}

// Parse an HTTP request line and route to handler.
std::string handleRequest(const std::string& request) {
    // Parse method and path
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version;

    // P2-datafix: Strip query string from path before routing.
    // Requests like "GET /api/fusion?t=12345 HTTP/1.1" previously failed to
    // match any route (exact match on "/api/fusion"), returning 404.
    size_t qpos = path.find('?');
    std::string routePath = (qpos != std::string::npos) ? path.substr(0, qpos) : path;

    // merge: parse Origin header for CORS echo
    // P2-fix: explicit null origin support — when Origin: null is sent (e.g.
    // from file:// pages or sandboxed iframes), return ACAO: null instead of
    // echoing the string "null" as if it were a normal origin.
    g_currentOrigin.clear();
    std::string lowerReq = request;
    std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
    size_t op = lowerReq.find("\r\norigin:");
    if (op != std::string::npos) {
        size_t os = op + 9;
        size_t oe = request.find("\r\n", os);
        if (oe != std::string::npos) {
            g_currentOrigin = request.substr(os, oe - os);
            while (!g_currentOrigin.empty() && (g_currentOrigin.front()==' '||g_currentOrigin.front()=='\t')) g_currentOrigin.erase(g_currentOrigin.begin());
            // HTTP-07: trailing whitespace too (mirrors the SSE origin parse),
            // else "origin " with a trailing space fails the byte-exact allowlist.
            while (!g_currentOrigin.empty() && (g_currentOrigin.back()==' '||g_currentOrigin.back()=='\t'||g_currentOrigin.back()=='\r')) g_currentOrigin.pop_back();
            // P2-fix: explicit null origin — keep as "null" so buildHttpResponse
            // returns Access-Control-Allow-Origin: null (spec-compliant).
        }
    }
    if (method == "OPTIONS") return buildHttpResponse(200,"text/plain","",g_currentOrigin);

    // sliding-window rate limit (per peer; loopback = single bucket).
    // OPTIONS preflight is exempt above. 429 = Too Many Requests.
    if (!rateLimitAllowed()) {
        return buildHttpResponse(429, "application/json",
            "{\"ok\":false,\"error\":\"rate limit exceeded (max 100 req/s)\"}",
            g_currentOrigin);
    }
    // P2-fix: Support both POST /control (body) and GET /control?cmd= (query)
    // WebUI writeControl() sends GET with ?cmd= query parameter.
    // P1-6: EXACT match on /control and /api/control. The old rfind(prefix,0)==0
    // matched "/control", "/controlXYZ", "/control2", "/api/control-evil" etc.
    // which widened the privileged write endpoint to unintended paths. The
    // frontend always hits exactly "/control" (query stripped into routePath).
    if (routePath == "/control" || routePath == "/api/control") {
        std::string cmd;
        if (method == "POST") {
            size_t bp = request.find("\r\n\r\n");
            cmd = (bp!=std::string::npos) ? request.substr(bp+4) : "";
            // v94 P2 问题21: body is already bounded by readHttpRequest() to
            // kMaxRequestBody (64KB). This is a secondary guard on the extracted
            // body; the command itself is later length-checked in handleControl().
            if (cmd.size() > kMaxRequestBody) {
                return buildHttpResponse(413,"application/json",
                    "{\"ok\":false,\"error\":\"request body too large (max 65536 bytes)\"}",g_currentOrigin);
            }
        } else if (method == "GET") {
            // Parse ?cmd= value from query string
            size_t qp = path.find('?');
            if (qp != std::string::npos) {
                std::string query = path.substr(qp + 1);
                // P3-12: exact k=v parse (queryParamEq) — the old query.find("cmd=")
                // matched "xcmd=<evil>" as a substring.
                std::string cmdVal;
                if (queryParamEq(query, "cmd", cmdVal)) {
                    cmd = urlDecode(cmdVal);
                }
            }
        } else {
            return buildHttpResponse(405,"text/plain","Method Not Allowed",g_currentOrigin);
        }
        // W-002/P2-4: tell handleControl whether the command rode a GET (always
        // CSRF-unsafe) or a POST. For POST we additionally test whether the request
        // is CSRF-safe (app/json / X-Daemon-Token / allowlisted Origin); a plain
        // cross-origin form POST (text/plain) is NOT safe.
        bool postCsrfSafe = (method == "POST") && isCsrfSafePost(request);
        return handleControl(cmd, method == "GET", postCsrfSafe, &request);
    }
    if (method != "GET") return buildHttpResponse(405,"text/plain","Method Not Allowed",g_currentOrigin);

    // Route (use routePath with query string stripped)
    if (routePath == "/health" || routePath == "/healthz") {
        return handleHealth();
    }
    // #78: /state/public — non-sensitive data, no token required.
    if (routePath == "/state/public" || routePath == "/public") {
        return handleStatePublic();
    }
    if (routePath == "/state/all" || routePath == "/state" || routePath == "/" || routePath == "/api/state" ||
        routePath == "/status" || routePath == "/api/status") {
        // the aggregate state leaks daemon_pid, foreground package
        // name, CPU topology and scheduling layout. Although the listener is
        // loopback-only, any local app can connect to 127.0.0.1. Require the
        // root-minted daemon token (via ?token= for EventSource, or the
        // X-Daemon-Token header). /health and /version remain open for liveness.
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required for /state/all\"}",
                g_currentOrigin);
        }
        // #22: forward the raw query so ?delta=1 can select delta mode.
        {
            std::string stateQuery = (qpos != std::string::npos) ? path.substr(qpos + 1) : "";
            return handleStateAll(stateQuery);
        }
    }
    if (routePath == "/version") {
        std::string body = std::string("{\"version\":\"") + AATPE_VERSION + "\"}";
        return buildHttpResponse(200, "application/json", body, g_currentOrigin);
    }
    // ROUND-005: GET /state/<name> — single state file, explicit whitelist.
    // Strict single-segment [a-z0-9_] name, no path traversal. Requires daemon
    // token (same as /state/all). Public keys (already in /state/public) may be
    // exempted below.
    {
        const std::string prefix = "/state/";
        if (routePath.rfind(prefix, 0) == 0 && routePath.size() > prefix.size()) {
            std::string name = routePath.substr(prefix.size());
            // Strict: only lowercase letters, digits, underscore. No dots, slashes, traversal.
            bool valid = !name.empty() && name.size() <= 64;
            for (char c : name) {
                if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_')) { valid = false; break; }
            }
            if (!valid) {
                return buildHttpResponse(400, "application/json",
                    "{\"ok\":false,\"error\":\"invalid state name\"}", g_currentOrigin);
            }
            // Public-exempt keys (no token needed, matches /state/public whitelist).
            static const std::set<std::string> kPublicSingle = {
                "thermal_cpu","thermal_skin","thermal_battery","thermal_emergency",
                "control_mode","current_profile","battery_level","battery_status",
                "refresh_rate","frame_rate","jank_percent","fg_package",
                "daemon_uptime","version","system_state"
            };
            bool isPublic = kPublicSingle.count(name) > 0;
            if (!isPublic && !checkDaemonTokenAuth(request, path)) {
                return buildHttpResponse(401, "application/json",
                    "{\"ok\":false,\"error\":\"daemon token required\"}", g_currentOrigin);
            }
            std::string val = stateSnapshotLookup(name);
            if (val.empty()) {
                return buildHttpResponse(404, "application/json",
                    "{\"ok\":false,\"error\":\"not found\"}", g_currentOrigin);
            }
            std::string body = "{\"" + name + "\":\"" + jsonEscape(val) + "\"}";
            return buildHttpResponse(200, "application/json", body, g_currentOrigin);
        }
    }
    if (routePath == "/api/hmac_key" || routePath == "/hmac_key") return handleHmacKey(request, path);
    // #61: short-lived session token exchange (requires HMAC-signed proof).
    if (routePath == "/api/session" || routePath == "/session") return handleSession(request, path);
    // 问题10 fix: /api/daemon_token is DISABLED on the TCP listener. On TCP
    // loopback, SO_PEERCRED does not work (g_peerUid stays -1), so the old
    // handleDaemonToken() always returned 403 — the endpoint was dead. The
    // token now lives at STATE_DIR/daemon_token (0600) and is read directly by
    // the root WebUI bridge. We keep the route recognized but return 403 so
    // stale clients get a clear "use the root bridge" signal instead of a 404.
    if (routePath == "/api/daemon_token" || routePath == "/daemon_token") {
        return buildHttpResponse(403, "application/json",
            "{\"ok\":false,\"error\":\"daemon_token unavailable over HTTP; read /dev/aatpe/state/daemon_token as root\"}",
            g_currentOrigin);
    }
    // LED-155 (SEC-002): /api/fusion and /api/frame_time were open to any local app
    // on loopback (no daemon token), leaking fusion weights/decision counts and
    // gfxinfo frame-time counters. Gate them behind checkDaemonTokenAuth like
    // /state/all and /api/command/* for consistent access control.
    if (routePath == "/api/fusion" || routePath == "/fusion") {
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required for /api/fusion\"}",
                g_currentOrigin);
        }
        return handleFusion();
    }
    if (routePath == "/api/frame_time" || routePath == "/frame_time") {
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required for /api/frame_time\"}",
                g_currentOrigin);
        }
        return handleFrameTime();
    }
    // W2: /api/frame_apps — per-app rolling frame stats. Same LED-155 gate as
    // /api/frame_time (daemon token); deliberately NOT in the public whitelist.
    if (routePath == "/api/frame_apps" || routePath == "/frame_apps") {
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required for /api/frame_apps\"}",
                g_currentOrigin);
        }
        return handleFrameApps();
    }
    // P0: /api/diag_bundle — fixed-whitelist tar.gz of daemon/service logs +
    // state. Same daemon-token gate; never reads a user-supplied path.
    if (routePath == "/api/diag_bundle" || routePath == "/diag_bundle") {
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required for /api/diag_bundle\"}",
                g_currentOrigin);
        }
        return handleDiagBundle();
    }
    // v4.2.1: /api/apps — installed third-party packages + current manual
    // game-override list for the WebUI picker. Loopback-only + daemon-token.
    if (routePath == "/api/apps" || routePath == "/apps") {
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required for /api/apps\"}",
                g_currentOrigin);
        }
        return handleApps();
    }
    if (routePath == "/api/config_meta") {
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required\"}", g_currentOrigin);
        }
        return handleConfigMeta();
    }
    if (routePath == "/logs" || routePath == "/api/logs") {
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required\"}", g_currentOrigin);
        }
        std::string q = (qpos != std::string::npos) ? path.substr(qpos + 1) : "";
        return handleLogs(q);
    }

    // round-4 (P1 #2): GET /api/command/<id> — poll a queued command's
    // lifecycle (queued/executing/success/failed) and its recorded result. This is
    // the companion to POST /control returning command_id. Requires the daemon token
    // (it can echo the privileged command text back).
    {
        const std::string cmdPrefix = "/api/command/";
        if (routePath.rfind(cmdPrefix, 0) == 0) {
            if (!checkDaemonTokenAuth(request, path)) {
                return buildHttpResponse(401, "application/json",
                    "{\"ok\":false,\"error\":\"daemon token required\"}", g_currentOrigin);
            }
            std::string id = routePath.substr(cmdPrefix.size());
            // Sanitize: only cmd-NNNNNN style ids are valid; reject path traversal.
            bool ok = id.rfind("cmd-", 0) == 0 && id.size() > 4 && id.size() <= 16;
            if (ok) for (char c : id) {
                if (!((c>='0'&&c<='9')||c=='c'||c=='m'||c=='d'||c=='-')) { ok = false; break; }
            }
            if (!ok) {
                return buildHttpResponse(400, "application/json",
                    "{\"ok\":false,\"error\":\"invalid command id\"}", g_currentOrigin);
            }
            std::string body = aatpe_cmdq::commandStatusJson(id);
            return buildHttpResponse(200, "application/json", body, g_currentOrigin);
        }
    }

    // G-006: GET /webui/<name> — serve the WEBUI_DIR snapshot JSON files
    // (threads.json / percpu.json / system.json) over HTTP so the WebUI
    // readFileViaHttp() fast path works instead of degrading to a root exec(cat)
    // per file. The frontend already maps WEBUI_DIR -> /webui/<name> and sends
    // X-Daemon-Token (readFileViaHttp L1292-1307).
    //
    // These files carry the SAME daemon-internal state as /state/all (CPU
    // topology, PSI, eBPF status, per-thread display scores, foreground pkg), so
    // they are gated behind the SAME checkDaemonTokenAuth — never anonymous.
    // Access control / CORS / CSP are unchanged:
    //   * method gate already returned 405 above (GET-only routing section);
    //   * auth via the existing checkDaemonTokenAuth(request, path);
    //   * response via buildHttpResponse -> uniform CORS + X-Content-Type-Options
    //     nosniff + X-Frame-Options DENY + CSP; no new headers, no CSRF surface
    //     (read-only GET), no SSE interaction.
    // Path safety: strict basename whitelist (exactly the three snapshot files),
    // no slash, no "..", size-bounded; readFileToString() also opens O_NOFOLLOW.
    {
        const std::string webuiPrefix = "/webui/";
        if (routePath.rfind(webuiPrefix, 0) == 0) {
            std::string name = routePath.substr(webuiPrefix.size());
            static const std::set<std::string> kWebuiWhitelist = {
                "threads.json", "percpu.json", "system.json"
            };
            bool valid = !name.empty() && name.size() <= 32 &&
                         name.find('/') == std::string::npos &&
                         name.find("..") == std::string::npos &&
                         kWebuiWhitelist.count(name) > 0;
            if (!valid) {
                return buildHttpResponse(400, "application/json",
                    "{\"ok\":false,\"error\":\"invalid webui file\"}", g_currentOrigin);
            }
            if (!checkDaemonTokenAuth(request, path)) {
                return buildHttpResponse(401, "application/json",
                    "{\"ok\":false,\"error\":\"daemon token required\"}", g_currentOrigin);
            }
            std::string body = readFileToString(std::string(WEBUI_DIR) + "/" + name);
            if (body.empty()) {
                return buildHttpResponse(404, "application/json",
                    "{\"ok\":false,\"error\":\"not found\"}", g_currentOrigin);
            }
            return buildHttpResponse(200, "application/json", body, g_currentOrigin);
        }
    }

    return buildHttpResponse(404, "text/plain", "Not Found", g_currentOrigin);
}

// v94 P2 问题21/22: Read a complete HTTP request handling TCP segmentation.
// Phase 1: recv until the header terminator "\r\n\r\n" is seen (headers complete).
// Phase 2: if Content-Length is present, continue recv() until exactly
//   Content-Length body bytes have been read — the body may arrive in a later
//   TCP segment than the headers, or be split across multiple recv() calls.
// outOversize is set to true when Content-Length exceeds kMaxRequestBody.
// Returns the full request (headers + body) read so far.
std::string readHttpRequest(int clientFd, bool* outOversize) {
    char buf[8192];
    std::string request;
    *outOversize = false;

    // Issue-17: separate cap on header size (16KB). The total kMaxRequestBody cap
    // applies to headers+body; without a separate header cap a request could send
    // 64KB of headers and zero body, wasting parse time. Normal HTTP requests
    // have < 4KB of headers.
    static const size_t kMaxHeaderSize = 16384;

    // Phase 1: read headers until \r\n\r\n.
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        ssize_t n = recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0) break;  // timeout (SO_RCVTIMEO) or peer closed
        request.append(buf, (size_t)n);
        headerEnd = request.find("\r\n\r\n");
        // Issue-17: reject oversized headers immediately.
        if (headerEnd == std::string::npos && request.size() > kMaxHeaderSize) {
            *outOversize = true;
            return request;
        }
        // Hard cap on total buffered bytes (headers + partial body).
        if (request.size() > kMaxRequestBody) {
            *outOversize = true;
            return request;
        }
    }
    if (headerEnd == std::string::npos) return request;  // incomplete headers

    // Phase 2: parse Content-Length from the header block (case-insensitive).
    std::string headers = request.substr(0, headerEnd);
    std::string lowerHeaders = headers;
    std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(), [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
    // P3: detect DUPLICATE Content-Length headers. A request with two different
    // Content-Length values is a request-smuggling vector (different parsers pick
    // different values). Reject the request outright instead of trusting the first.
    {
        size_t dupProbe = 0;
        int clCount = 0;
        while ((dupProbe = lowerHeaders.find("content-length:", dupProbe)) != std::string::npos) {
            // ensure it is a header start (preceded by \r\n or at position 0)
            if (dupProbe == 0 || lowerHeaders[dupProbe-1] == '\n') clCount++;
            dupProbe += 15;
            if (clCount > 1) {
                *outOversize = true;  // treat as malformed; caller will 413/close
                return request;
            }
        }
    }
    size_t clPos = lowerHeaders.find("content-length:");
    size_t contentLength = 0;
    bool hasContentLength = false;
    // G8-011 (ROUND-001): reject any request that uses Transfer-Encoding. We do
    // not implement chunked framing; silently ignoring the header and trusting
    // Content-Length (or the initial recv buffer) is a smuggling-adjacent
    // foot-gun. Refuse with a clear 501 so clients know to fall back.
    if (lowerHeaders.find("transfer-encoding:") != std::string::npos) {
        *outOversize = true;  // caller will close; we treat as malformed
        return request;
    }
    if (clPos != std::string::npos) {
        size_t valStart = clPos + 15;  // strlen("content-length:")
        while (valStart < headers.size() && headers[valStart] == ' ') valStart++;
        size_t valEnd = headers.find("\r\n", valStart);
        if (valEnd == std::string::npos) valEnd = headers.size();
        std::string clStr = headers.substr(valStart, valEnd - valStart);
        try {
            size_t used = 0;
            unsigned long long v = std::stoull(clStr, &used);
            // P3: reject trailing garbage ("100abc" must not parse as 100).
            if (used == 0 || used != clStr.size()) throw std::invalid_argument("cl");
            contentLength = (size_t)v;
            hasContentLength = true;
        } catch (...) { hasContentLength = false; LOG_DEBUG("http: malformed Content-Length '%s' ignored", clStr.c_str()); }
    }

    if (hasContentLength) {
        if (contentLength > kMaxRequestBody) {
            *outOversize = true;
            return request;
        }
        size_t bodyStart = headerEnd + 4;
        size_t bodyRead = request.size() - bodyStart;  // body bytes already received with headers
        // Continue recv until the full body is present (handles TCP segmentation).
        while (bodyRead < contentLength) {
            size_t want = std::min(sizeof(buf), contentLength - bodyRead);
            ssize_t n = recv(clientFd, buf, want, 0);
            if (n <= 0) break;  // timeout or peer closed before full body
            request.append(buf, (size_t)n);
            bodyRead += (size_t)n;
            if (request.size() > kMaxRequestBody) {
                *outOversize = true;
                return request;
            }
        }
    }
    return request;
}

// v94-perf: shared, TTL-bounded cache for the aggregate state JSON.
// buildAggregateStateJson() walks every regular file in STATE_DIR (~183 files)
// and open()/read()'s each one on every call. Before this change each SSE
// connection rebuilt that JSON from disk every 500ms, so N open EventSource
// connections did Nx the directory walk + file reads on every push tick. Now all
// SSE pushes share one cache that is rebuilt at most once per TTL, regardless of
// how many clients are connected.
//
// G-TSan fix (round 2): this cache (mutex + string + time point) was a set of
// static globals. stopHttpServer() has an 8s join budget; under TSan (slowed
// ~10x) an in-flight SSE thread can still be inside getCachedAggregateStateJson()
// when the budget expires and it is detach()ed. That detached thread may keep
// running past process exit; when it then locks g_stateJsonCacheMutex and moves
// into g_stateJsonCache AFTER the static mutex/string have already been destroyed
// by atexit, ThreadSanitizer reports heap-use-after-free + data race (and on the
// integration TSan binary this manifests as a SEGV in std::string move-assign /
// char_traits::assign on an unmapped page). By holding the cache in an
// intentionally-leaked heap singleton (accessor below), the state outlives every
// thread — including late-exiting detached ones — exactly like SseAdmission.
struct StateJsonCache {
    std::mutex mu;
    std::string data;
    std::chrono::steady_clock::time_point time;
};
static StateJsonCache& stateJsonCache() {
    static StateJsonCache* c = new StateJsonCache();  // intentionally leaked, never destroyed
    return *c;
}
static const auto kStateJsonCacheTtl = std::chrono::milliseconds(1000);

std::string getCachedAggregateStateJson() {
    StateJsonCache& cache = stateJsonCache();
    std::lock_guard<std::mutex> lock(cache.mu);
    auto now = std::chrono::steady_clock::now();
    if (!cache.data.empty() &&
        (now - cache.time) < kStateJsonCacheTtl) {
        return cache.data;
    }
    cache.data = aatpe::webui_http::buildAggregateStateJson();
    cache.time = now;
    return cache.data;
}

// v94-realtime: SSE (Server-Sent Events) handler for /events endpoint.
// Keeps a long-lived connection open and pushes aggregate state JSON every 500ms.
// This replaces the 3s polling loop with true server-push: the frontend receives
// CPU freq/temp/mode updates within ~500ms of them changing in the daemon.
//
// Design notes:
//   - Uses a 1s recv timeout (SO_RCVTIMEO) so the loop wakes up 2x/sec to check
//     for client disconnect (recv returns 0/error when client goes away) without
//     burning CPU on blocking poll().
//   - Sends a SSE comment line (": heartbeat\n\n") every ~15s as a keep-alive
//     through any intermediate proxies/NAT and to detect dead peers early.
//   - The pushed payload is the SAME JSON object as /state/all — no new serialization
//     format, no schema drift. The frontend EventSource just does JSON.parse() on it.
//   - CORS: EventSource does not send custom headers in all browsers, so we echo
//     the Origin header (already validated by isCorsOriginAllowed) into the SSE
//     response headers.
void handleSseConnection(int clientFd, const std::string& origin) {
    // Disable the 2s recv timeout inherited from handleConnection setup.
    // We want a 1s timeout so we can check for client disconnect and push interval.
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOG_DEBUG("webui: setsockopt(SO_RCVTIMEO) failed for SSE client: %s", aatpeErrStr(errno));
    }
    // Half-open reclaim correctness: pin the SSE send buffer small instead of
    // inheriting the OS default. On a desktop host the default sndbuf is ~208KB
    // (net.core.wmem_default); a slow/non-reading WebView then absorbs every
    // small probe/heartbeat into that buffer and send() NEVER blocks, so the
    // EAGAIN stall detector below never fires and the slot leaks until the whole
    // buffer fills (minutes). A small sndbuf bounds the in-flight-but-undelivered
    // queue: once the peer stops reading, the next frame blocks within ~kSndbuf
    // bytes, SO_SNDTIMEO (sendTimeoutSec) fires, EAGAIN breaks the loop, and the
    // slot is reclaimed. We keep the sndbuf small (~4KB; the kernel floors the
    // actual value a little higher) so the bounded queue is short on ANY host:
    // a healthy WebView drains continuously so a small sndbuf never stalls live
    // pushes (send() copies straight into the reader), while a dead peer blocks
    // within a few KB of queued data instead of being absorbed by a multi-hundred-KB
    // OS-default buffer.
    {
        int sndbuf = 4096;
        if (setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
            LOG_DEBUG("webui: setsockopt(SO_SNDBUF) failed for SSE client: %s", aatpeErrStr(errno));
        }
    }

    // Build SSE response headers — no Content-Length, no "Connection: close".
    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n";
    hdr << "Content-Type: text/event-stream\r\n";
    hdr << "Cache-Control: no-cache\r\n";
    hdr << "Connection: keep-alive\r\n";
    hdr << "X-Accel-Buffering: no\r\n";
    hdr << "Server: AATPE-WebUI/1.0\r\n";
    // P3: use the same centralized CORS helper as short HTTP responses.
    appendCorsHeaders(hdr, origin);
    hdr << "\r\n";
    std::string hdrStr = hdr.str();
    // P0-2: loop-write the headers (sendAll handles partial/EAGAIN).
    if (!sendAll(clientFd, hdrStr.c_str(), hdrStr.size())) { close(clientFd); return; }

    // header sent -> the SSE stream is live. Count it so the
    // daemon main loop keeps the heavy threads/decisions/explain JSON writers on
    // the 2s cadence. RAII decrements on EVERY exit path (client disconnect,
    // send error, server shutdown) so the counter never leaks.
    struct SseCountGuard {
        SseCountGuard()  { g_sseClientCount.fetch_add(1, std::memory_order_acq_rel); }
        ~SseCountGuard() { g_sseClientCount.fetch_sub(1, std::memory_order_acq_rel); }
    };
    SseCountGuard sseCountGuard;

    int idleTicks = 0;  // count ticks since last data push
    int heartbeatTicks = 0; // count since last heartbeat comment
    // #19/#20: push ONLY when the state generation actually changes, instead of
    // flooding every 500ms. A 250ms tick keeps switch latency <250ms; when state
    // is stable we send at most a lightweight generation probe every ~2s plus the
    // 15s SSE keep-alive comment.
    uint64_t lastPushedGen = (uint64_t)-1;
    uint64_t lastChangePushTicks = 0;
    const int kNoChangeProbeTicks = 8;  // ~2s at 250ms/tick

    while (g_httpRunning.load()) {
        // Non-blocking recv to detect client disconnect.
        char drainBuf[256];
        ssize_t n = recv(clientFd, drainBuf, sizeof(drainBuf), MSG_DONTWAIT);
        if (n == 0) break;          // client closed
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;

        idleTicks++;
        heartbeatTicks++;

        // Push the full state JSON only when the generation changed.
        uint64_t curGen = stateCacheGeneration();
        if (curGen != lastPushedGen) {
            std::string payload = getCachedAggregateStateJson();
            std::string frame = "data: " + payload + "\n\n";
            // P0-2: loop-write (partial send / EAGAIN safe).
            if (!sendAll(clientFd, frame.c_str(), frame.size())) break;
            lastPushedGen = curGen;
            lastChangePushTicks = idleTicks;
        } else if (idleTicks - lastChangePushTicks >= kNoChangeProbeTicks) {
            // No change for ~2s: send a tiny generation-only probe so the client
            // can confirm liveness without the full ~183-key payload.
            std::string probe = "data: {\"state_generation\":" +
                                std::to_string(curGen) + "}\n\n";
            if (!sendAll(clientFd, probe.c_str(), probe.size())) break;
            lastChangePushTicks = idleTicks;
        }

    // Heartbeat comment every kSseKeepaliveSec (default 20s, env override for
    // tests). SSE comment lines are ignored by EventSource but keep the TCP
    // connection alive through NAT/proxy timeouts AND bound half-open
    // (silent, no FIN) connections: a dead peer makes sendAll fail -> break ->
    // SseSlotGuard releases the slot within ~keepalive+send_timeout.
    // A6: the env var is a process-level constant — read it ONCE (magic-static)
    // instead of calling getenv() on every incoming SSE connection.
    static const int kSseKeepaliveSec = []() {
        const char* envKp = getenv("AATPE_SSE_KEEPALIVE_SEC");
        int v = envKp ? atoi(envKp) : 20;
        return v < 1 ? 1 : v;
    }();
    const int kHeartbeatTicks = kSseKeepaliveSec * 4;  // 250ms ticks
        if (heartbeatTicks >= kHeartbeatTicks) {
            heartbeatTicks = 0;
            const char* hb = ": keepalive\n\n";
            if (!sendAll(clientFd, hb, strlen(hb))) break;
        }

        // Tick ~250ms: fast enough to react to a state change, cheap when idle.
        usleep(250000);
    }
    close(clientFd);
}

// v94 P2 问题23/24: Handle one accepted connection on its own thread.
// The accept loop only does accept(); all I/O + request processing happens here
// so a single slow client cannot block other connections.
// round-3 (P0-1): `self` is a shared_ptr so the ConnEntry stays alive for
// the lifetime of this thread regardless of what stopHttpServer() does to the
// registry. DoneGuard holds the shared_ptr by value; setting ->done in its
// destructor can never touch freed memory.
void handleConnection(int clientFd, std::shared_ptr<ConnEntry> self) {
    // P1-3: verify the peer is on 127.0.0.1 via getpeername(). The socket is
    // already bound to INADDR_LOOPBACK so this is defense-in-depth, but the
    // daemon_token endpoint treats it as a trusted signal alongside Origin.
    g_peerIsLoopback = false;
    {
        struct sockaddr_in peer;
        socklen_t peerLen = sizeof(peer);
        if (getpeername(clientFd, (struct sockaddr*)&peer, &peerLen) == 0) {
            if (peer.sin_family == AF_INET &&
                peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
                g_peerIsLoopback = true;
            }
        }
    }
    // fix-v96 #3: kernel-verified peer UID. SO_PEERCRED is valid only on AF_UNIX
    // sockets. On the TCP loopback listener the kernel either (a) fails the call
    // outright or (b) returns success with a zeroed/self credential struct
    // (verified on host Linux: r=0 but pid=0, uid=0 — garbage). We therefore
    // trust SO_PEERCRED ONLY when getpeername reports AF_UNIX; on AF_INET we
    // force g_peerUid=-1 (untrusted) regardless of the getsockopt result.
    g_peerUid = -1;
    {
        struct sockaddr_storage sa;
        socklen_t saLen = sizeof(sa);
        if (getpeername(clientFd, (struct sockaddr*)&sa, &saLen) == 0 &&
            sa.ss_family == AF_UNIX) {
            struct ucred cr;
            socklen_t crLen = sizeof(cr);
            if (getsockopt(clientFd, SOL_SOCKET, SO_PEERCRED, &cr, &crLen) == 0 &&
                crLen == sizeof(cr)) {
                g_peerUid = (int)cr.uid;
            }
        }
    }
    // G8-003 (ROUND-001): rate-limit bucket key.
    //  - AF_UNIX with verified UID: bucket by real UID (one bucket per app).
    //  - TCP loopback (g_peerUid == -1): we cannot distinguish local apps, so
    //    collapse ALL TCP clients into ONE shared strict bucket. Previously we
    //    keyed by ephemeral source port, which let one malicious app open 32
    //    connections and multiply its rate budget by 32 (3200 req/s total).
    //    The shared bucket is intentionally stricter (see kRateLimitPerSecTcp).
    g_rateBucketKey = g_peerUid;
    if (g_rateBucketKey < 0) {
        g_rateBucketKey = kSharedTcpBucketKey; // single shared bucket for all TCP
    }

    // Mark done on any exit path (normal return, early return). sequenced-before
    // the underlying thread terminates, so a reaper join() returns immediately.
    // The shared_ptr copy keeps self->ConnEntry alive until this guard destructs.
    // P0-1: ALSO clear self->clientFd here. After the handler (or the SSE callee)
    // close()s the fd, the fd number can be recycled by a later accept(). If
    // stopHttpServer() snapshots the ConnEntry and shutdown()s the stale fd
    // number, it would shutdown a DIFFERENT connection. Setting clientFd=-1 in
    // this RAII guard (which runs on EVERY exit path, including SSE's internal
    // close) means stopHttpServer sees -1 and skips shutdown on closed fds.
    struct DoneGuard {
        std::shared_ptr<ConnEntry> e;
        ~DoneGuard() {
            if (e) {
                e->done.store(true);
                e->clientFd.store(-1);
            }
        }
    } doneGuard{self};

    // Set read timeout (2 seconds) to avoid hanging on slow clients.
    struct timeval tv;
    tv.tv_sec = kRecvTimeoutSec;
    tv.tv_usec = 0;
    if (setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOG_DEBUG("webui: setsockopt(SO_RCVTIMEO) failed: %s", aatpeErrStr(errno));
    }
    // round-3 (P1-18): also bound send(). A client that never drains the
    // response otherwise pins the send() loop; the 5s write timeout guarantees
    // every handler thread terminates in bounded time so stopHttpServer() can
    // always join (no forced detach, no UAF).
    struct timeval tvw;
    tvw.tv_sec = sendTimeoutSec();
    tvw.tv_usec = 0;
    if (setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tvw, sizeof(tvw)) < 0) {
        LOG_DEBUG("webui: setsockopt(SO_SNDTIMEO) failed: %s", aatpeErrStr(errno));
    }

    // P0 keep-alive: serve up to kKeepAliveMaxRequests on one fd to kill the
    // per-request TIME_WAIT storm. readHttpRequest() already frames exactly one
    // request (headers + Content-Length body), so the next loop iteration reads
    // the next request cleanly. Safety:
    //  - idle close driven by the 2s recv timeout (recv<=0 -> empty -> loop end);
    //  - any "Connection: close" / oversize / SSE path closes as before;
    //  - EVERY request re-runs handleRequest -> checkDaemonTokenAuth per request,
    //    never once-per-connection.
    static const int kKeepAliveMaxRequests = 64;
    for (int kaIdx = 0; kaIdx < kKeepAliveMaxRequests; ++kaIdx) {
    bool oversize = false;
    std::string request = readHttpRequest(clientFd, &oversize);

    // Shutdown requested while blocked in recv: drop the connection, no reply.
    // P1-3: publish -1 before close() so stopHttpServer() cannot shutdown a
    // recycled fd.
    if (!g_httpRunning.load()) {
        if (self) self->clientFd.store(-1);
        close(clientFd); return;
    }
    if (request.empty()) break;  // idle recv timeout / peer closed -> end keep-alive

    if (oversize) {
        std::string resp = buildHttpResponse(413, "application/json",
            "{\"ok\":false,\"error\":\"request body too large (max 65536 bytes)\"}",
            g_currentOrigin);
        // P1-1/P3-14: use sendAll() (MSG_NOSIGNAL + EINTR/EAGAIN loop) instead of a
        // raw send(...,0) loop. A short send on a peer-RST socket previously raised
        // SIGPIPE and killed the root daemon; a single short write also silently
        // dropped the tail of the response.
        sendAll(clientFd, resp.data(), resp.size());
        break;  // P2-1: 413/oversize -> close fd, never reuse / drain-resync
    } else if (!request.empty()) {
        // Re-check before doing any work on a shutting-down server.
        if (g_httpRunning.load()) {
            // v94-realtime: Detect SSE /events request and route to long-lived
            // SSE handler. We parse the request line directly from the already-read
            // request buffer (no extra recv needed).
            std::string sseMethod, ssePath, sseVersion;
            {
                std::istringstream ss(request);
                ss >> sseMethod >> ssePath >> sseVersion;
            }
            size_t sseQ = ssePath.find('?');
            std::string sseRoute = (sseQ != std::string::npos) ? ssePath.substr(0, sseQ) : ssePath;
            if (sseMethod == "GET" && (sseRoute == "/events" || sseRoute == "/sse" || sseRoute == "/api/events")) {
                // SSE pushes the same sensitive aggregate as /state/all. EventSource
                // cannot set custom headers, so the WebUI appends ?stok=<5min ticket>
                // (F-01: never the long-lived daemon_token on TCP loopback). The
                // X-Daemon-Token header is also accepted; ?token= is accepted only
                // on the AF_UNIX trusted channel.
                if (!checkSseTicketAuth(request, ssePath)) {
                    std::string deny = buildHttpResponse(401, "application/json",
                        "{\"ok\":false,\"error\":\"daemon token required for /events\"}",
                        "");
                    // P3-13: sendAll (loop short-write) not a single send().
                    sendAll(clientFd, deny.data(), deny.size());
                    if (self) self->clientFd.store(-1);
                    close(clientFd);
                    return;
                }
                // round-4 (P2 #6): admit SSE connection under the total +
                // per-UID caps. The guard releases the slot when this scope exits
                // (i.e. when handleSseConnection returns / the client disconnects).
                {
                    SseSlotGuard slot = sseTryAcquire(g_peerUid);
                    if (!slot.acquired) {
                        // ROUND-003: machine-readable reason so the frontend can
                        // distinguish "SSE rate-limited, retry soon" from other
                        // errors and back off 1s instead of the fixed 3s.
                        std::string deny = buildHttpResponse(429, "application/json",
                            "{\"ok\":false,\"error\":\"sse_limit\",\"sse_limit\":true,\"retry_after\":1}",
                            "");
                        sendAll(clientFd, deny.data(), deny.size());
                        if (self) self->clientFd.store(-1);
                        close(clientFd);
                        return;
                    }
                    // Parse Origin for CORS echo (same logic as handleRequest).
                    std::string sseOrigin;
                    std::string lowerReq = request;
                    std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
                    size_t op = lowerReq.find("\r\norigin:");
                    if (op != std::string::npos) {
                        size_t os = op + 9;
                        size_t oe = request.find("\r\n", os);
                        if (oe != std::string::npos) {
                            sseOrigin = request.substr(os, oe - os);
                            // HTTP-07 (P3, audit 2026-10-26): trim BOTH leading
                            // and trailing header whitespace (RFC 7230 obs-fold /
                            // trailing SP-HTAB). Only leading was stripped before,
                            // so "https://trusted.example " with a trailing space
                            // failed the byte-exact allowlist match and dropped the
                            // CORS headers.
                            while (!sseOrigin.empty() && (sseOrigin.front()==' '||sseOrigin.front()=='\t')) sseOrigin.erase(sseOrigin.begin());
                            while (!sseOrigin.empty() && (sseOrigin.back()==' '||sseOrigin.back()=='\t'||sseOrigin.back()=='\r')) sseOrigin.pop_back();
                        }
                    }
                    // P3-11: isSse field removed (was write-only).
                    // P1-3: hand the fd to handleSseConnection() which closes it
                    // internally. Publish -1 now so stopHttpServer() does not
                    // shutdown() an fd that handleSseConnection has already closed.
                    if (self) self->clientFd.store(-1);
                    handleSseConnection(clientFd, sseOrigin);
                    return;  // handleSseConnection closes the fd; slot released here
                }
            }
            std::string response = handleRequest(request);
            // P1-1/P3-14: sendAll() (MSG_NOSIGNAL + EINTR/EAGAIN short-write loop)
            sendAll(clientFd, response.data(), response.size());
            // P2-1: on any 4xx/5xx (malformed, 400, 401, 403, 404, 413...) close the
            // fd — never reuse after an error, so no leftover body bytes can be
            // misread as the next request's header.
            if (response.size() >= 12 && response.compare(0,5,"HTTP/")==0) {
                size_t sp = response.find(' ');
                int st = atoi(response.c_str()+sp+1);
                if (st >= 400) break;
            }
            // Honour client "Connection: close"; otherwise loop for next request.
            // HTTP-06 (P3, audit 2026-10-26): search ONLY the header block (up to
            // the first \r\n\r\n), never the whole request body. Previously a POST
            // body that happened to contain the literal "connection: close" (e.g.
            // a command echo / state value round-tripped through us) made us close
            // a keep-alive connection the client expected to reuse. Same scoping
            // rule as G8-004's x-daemon-token scan (isCsrfSafePost).
            size_t hdrEnd = request.find("\r\n\r\n");
            std::string headersOnly = (hdrEnd == std::string::npos)
                                          ? request
                                          : request.substr(0, hdrEnd);
            std::string lowReq = headersOnly;
            std::transform(lowReq.begin(), lowReq.end(), lowReq.begin(),
                           [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
            if (lowReq.find("connection: close") != std::string::npos) break;
        }
    }
    }  // keep-alive loop
    // P1-3: clear self->clientFd BEFORE close(clientFd). There is a window
    // between close() and the DoneGuard dtor (which runs at function exit) during
    // which the kernel can recycle clientFd for a new accept(); if stopHttpServer()
    // snapshots the ConnEntry in that window it would shutdown() the NEW fd.
    // Publishing -1 first means stopHttpServer() sees -1 and skips shutdown.
    if (self) self->clientFd.store(-1);
    close(clientFd);
}

// Accept loop: accept one connection, register a joinable handler thread.
// v94 P2 问题23/24: Previously serial — a slow client blocked all other requests
// for up to 5s. Now each connection gets its own thread.
// v94 改进包B 任务2: threads are tracked in g_connThreads (bounded to
// kMaxConnections). Finished threads are reaped here opportunistically so the
// list does not grow; stopHttpServer() does the final bounded drain.
void acceptLoop(int listenFd) {
    while (g_httpRunning.load()) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(listenFd, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd < 0) {
            if (g_httpRunning.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }
        // v94 P0-bugfix: record that a client is still talking to us. This keeps
        // the gated JSON writers enabled through an SSE reconnect flap (the page
        // polls /state/all /health between SSE attempts, so accepts keep coming).
        g_lastClientAcceptMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_relaxed);
        std::lock_guard<std::mutex> connLock(g_connMutex);
        // Reap any finished handler threads before deciding on the new one.
        for (auto it = g_connThreads.begin(); it != g_connThreads.end(); ) {
            if ((*it)->done.load()) {
                (*it)->th.join();
                it = g_connThreads.erase(it);
            } else {
                ++it;
            }
        }
        // Enforce the connection cap: reject (close) instead of spawning.
        if (g_connThreads.size() >= kMaxConnections) {
            close(clientFd);
            continue;
        }
        auto entry = std::make_shared<ConnEntry>();
        entry->clientFd = clientFd;
        std::shared_ptr<ConnEntry> entryShared = entry;
        // P1-8: std::thread() can throw std::system_error when the process hits
        // RLIMIT_NTHREADS / RLIMIT_NPROC. Without the try/catch the exception
        // escapes acceptLoop() and tears down the whole HTTP server. On spawn
        // failure we close the fd and keep accepting.
        try {
            entry->th = std::thread(handleConnection, clientFd, entryShared);
        } catch (const std::exception& ex) {
            LOG_ERROR("webui: failed to spawn handler thread: %s (closing fd %d)",
                      ex.what(), clientFd);
            close(clientFd);
            entry->clientFd.store(-1);
            continue;
        }
        g_connThreads.push_back(entry);
    }
}

} // anonymous namespace

#ifdef AATPE_HOST_TEST
// F-A6-01/02[P2] host seams: handleControl/handleDiagBundle live in the anonymous
// namespace (internal linkage), so hermetic tests need a thin exported wrapper to
// drive the downgrade replay and diag-path logic without booting the TCP server.
// Guarded by AATPE_HOST_TEST -> absent from the release/NDK build (nm zero).
std::string aatpeHostHandleControl(const std::string& cmd) {
    return handleControl(cmd, false, false, nullptr);
}
std::string aatpeHostHandleDiagBundle() {
    return handleDiagBundle();
}
#endif

namespace aatpe {
namespace webui_http {

bool startHttpServer() {
    if (g_httpRunning.load()) return true;

    // P1-1: ignore SIGPIPE process-wide. A client that RSTs the loopback socket
    // while we send() a short response otherwise delivers SIGPIPE to the root
    // daemon and kills it (default disposition = terminate). sendAll() already
    // passes MSG_NOSIGNAL on every send(), but belt-and-suspenders: also install
    // a process-wide SIG_IGN handler so any other code path that send()s on a
    // dead peer (e.g. the SSE reject sends) cannot crash the daemon.
    signal(SIGPIPE, SIG_IGN);

    // v94-fix#14: fail closed on CSPRNG failure. The HTTP API is gated behind the
    // daemon token; if we cannot mint a cryptographically-strong token (/dev/
    // urandom unavailable or short read), do not bind the listener at all rather
    // than serving an authenticated surface with a weak/absent credential.
    std::string bootToken = getOrCreateDaemonToken();
    if (bootToken.empty()) {
        LOG_ERROR("startHttpServer: cannot mint a CSPRNG daemon token; HTTP WebUI not started (fail closed)");
        return false;
    }

    // P2: SOCK_CLOEXEC so the listen fd is not leaked across exec() of child
    // processes (watchdog/CLI/probe spawns).
    g_listenFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_listenFd < 0) return false;

    // Allow address reuse
    int opt = 1;
    if (setsockopt(g_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_DEBUG("webui: setsockopt(SO_REUSEADDR) failed: %s", aatpeErrStr(errno));
    }

    // Try ports 8888-8899
    bool bound = false;
    for (int port = 8888; port <= 8899; port++) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
        addr.sin_port = htons(port);

        if (bind(g_listenFd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            g_httpPort.store(port);
            bound = true;
            break;
        }
    }

    if (!bound) {
        close(g_listenFd);
        g_listenFd = -1;
        return false;
    }

    if (listen(g_listenFd, 8) < 0) {
        close(g_listenFd);
        g_listenFd = -1;
        return false;
    }

    g_httpRunning.store(true);
    g_serverStart = std::chrono::steady_clock::now();
    // v94 改进包B 任务2: keep g_httpThread joinable (previously detached).
    // stopHttpServer() closes the listen socket and joins it deterministically.
    g_httpThread = std::thread(acceptLoop, g_listenFd);
    return true;
}

void stopHttpServer() {
    g_httpRunning.store(false);

    // round-3 (P0-1): FIRST wake every in-flight handler thread out of any
    // blocking recv/send by shutdown()ing its client socket, BEFORE we try to
    // join. shutdown(SHUT_RDWR) makes a blocked recv() return EOF and a blocked
    // send() return EPIPE immediately, so threads exit on their own instead of
    // waiting out the full SO_RCVTIMEO/SO_SNDTIMEO budget. We snapshot the list
    // under the lock, then close() the fds outside the lock.
    std::vector<std::shared_ptr<ConnEntry>> snapshot;
    {
        std::lock_guard<std::mutex> connLock(g_connMutex);
        snapshot.assign(g_connThreads.begin(), g_connThreads.end());
    }
    for (auto& e : snapshot) {
        // P0-1: load the fd once under the atomic. DoneGuard clears it to -1 when
        // a handler thread exits, so we never shutdown() a closed/recycled fd.
        int fd = e ? e->clientFd.load() : -1;
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
        }
    }

    if (g_listenFd >= 0) {
        shutdown(g_listenFd, SHUT_RDWR);
        close(g_listenFd);
        g_listenFd = -1;
    }
    g_httpPort.store(0);

    // Drain in-flight connection threads with a bounded wait. With recv/send
    // timeouts (2s/5s) + the shutdown() above, in-flight work finishes quickly.
    // We join finished entries as they complete; the shared_ptr in each thread's
    // DoneGuard means erasing from the registry never frees ConnEntry under a
    // still-running thread.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        bool anyActive = false;
        {
            std::lock_guard<std::mutex> connLock(g_connMutex);
            for (auto it = g_connThreads.begin(); it != g_connThreads.end(); ) {
                if ((*it)->done.load()) {
                    (*it)->th.join();
                    it = g_connThreads.erase(it);
                } else {
                    anyActive = true;
                    ++it;
                }
            }
            if (g_connThreads.empty()) break;
        }
        if (!anyActive) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // P2: after the bounded 8s drain, any thread STILL not done is stuck (e.g. an
    // SSE client that ignores our shutdown and keeps its socket open). The old
    // code join()'ed these unconditionally — if such a thread never exits,
    // stopHttpServer() hangs forever, blocking daemon shutdown. We detach the
    // stragglers instead. This is memory-safe because each thread's DoneGuard
    // holds a shared_ptr<ConnEntry> by value: detaching does NOT free ConnEntry,
    // and g_connThreads.clear() only drops the registry copy. The detached thread
    // exits whenever its own recv/send timeouts fire (bounded at 2s/5s) and the
    // process is shutting down anyway.
    std::lock_guard<std::mutex> connLock(g_connMutex);
    for (auto& e : g_connThreads) {
        if (!e->th.joinable()) continue;
        if (e->done.load()) {
            e->th.join();
        } else {
            e->th.detach();
        }
    }
    g_connThreads.clear();

    // Join the accept-loop thread now that its listen socket is closed and
    // g_httpRunning is false (accept() returns immediately).
    if (g_httpThread.joinable()) g_httpThread.join();
}

int getHttpPort() {
    return g_httpPort.load();
}

// P2-1 fix: rough "is any WebUI client connected?" probe for the daemon loop.
// Returns true iff the HTTP server is running AND at least one connection
// handler thread has not finished (DoneGuard not set). SSE /events connections
// keep their handler thread alive for the whole browser session; short HTTP
// requests finish almost immediately. The daemon uses this to skip the expensive
// 2s full JSON serialization (threads.json over ~500 threads, decisions.json,
// explain.json) when no browser is attached — previously those writes ran every
// 2s regardless, burning CPU and I/O with no reader.
bool hasActiveClients() {
    if (!g_httpRunning.load()) return false;
    // Grace window (checked lock-free first): the page may be open and polling
    // /state/all while its SSE stream is briefly down (WebView recycle, tab
    // switch, CORS reconnect). Any accept within the last kClientGraceMs keeps the
    // gated writers enabled so threads/decisions/explain JSON does not go stale
    // out from under the open page. Once the page closes, accepts stop and the
    // writers spin down after the window elapses.
    long long lastAccept = g_lastClientAcceptMs.load(std::memory_order_relaxed);
    if (lastAccept != 0) {
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs - lastAccept < kClientGraceMs) return true;
    }
    std::lock_guard<std::mutex> connLock(g_connMutex);
    for (const auto& e : g_connThreads) {
        // SSE streams are long-lived; treat an explicitly-tagged SSE conn that has
        // not finished as active. Short HTTP requests finish (done=true) almost
        // immediately and are intentionally NOT counted here (they are covered by
        // the grace window above, which is refreshed on every accept).
        if (!e->done.load()) return true;
    }
    return false;
}

// v94 WebUI-fix: materialize daemon_token at init time (see header comment).
// getOrCreateDaemonToken() lives in the anonymous namespace above; calling it here
// forces the 0600 token file to exist so the WebUI's first `cat daemon_token`
// (via KSU exec) succeeds and the signed-command bootstrap is not stuck.
void ensureDaemonToken() {
    std::string tok = getOrCreateDaemonToken();
    (void)tok;
}

std::string buildAggregateStateJson() {
    // opt#1: build from the in-memory STATE_DIR cache instead of
    // opendir()+open()/read() on every file (~183) every 500ms. The cache is kept
    // byte-for-byte identical to disk by writeFile()/writeFileAtomic() (they call
    // stateCacheNoteWrite on every write); it is seeded once from the filesystem
    // on first read. Output JSON is identical in schema and content.
    if (!stateCacheIsSeeded()) {
        stateCacheSeedFromDir(STATE_DIR);
    }
    std::vector<std::pair<std::string,std::string>> snap = stateCacheSnapshot();
    std::ostringstream body;
    body << "{";
    // #20: embed the monotonic generation as the first key so SSE clients can
    // decide whether a payload carries any real change by comparing one number.
    body << "\"state_generation\":" << stateCacheGeneration();
    for (const auto& kv : snap) {
        if (kv.first == "state_generation") continue;  // never double-key
        body << ",";
        body << "\"" << jsonEscape(kv.first) << "\":\"" << jsonEscape(kv.second) << "\"";
    }
    body << "}";
    return body.str();
}

} // namespace webui_http
} // namespace aatpe

// P3-42: public audit entry point used by the CommandDispatcher (CLI / control-pipe
// / watchdog-originated sensitive commands). The anonymous-namespace auditCommand()
// does the actual file write; this wrapper makes it reachable from other TUs.
namespace aatpe {
namespace webui_http {
void auditSensitiveCommand(const std::string& cmd, const std::string& source,
                            bool ok, const std::string& reason) {
    // auditCommand lives in the anonymous namespace above; call it with peerUid=0
    // (root for CLI/pipe/watchdog, which are root-owned channels).
    auditCommand(cmd, 0, ok, source, reason);
}
} // namespace webui_http
} // namespace aatpe
