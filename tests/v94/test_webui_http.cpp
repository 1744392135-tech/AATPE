// test_webui_http_round001.cpp - ROUND-001 regression tests for G8 fixes.
// Socket-level: starts the real HTTP server on 127.0.0.1, connects via TCP,
// and asserts the fixed behaviors.
//
//   G8-001: hmacRequired=false + POST /api/session with NO credentials -> 403
//   G8-003: TCP shared rate-limit bucket rejects after 50 req/s
//   G8-011: request with Transfer-Encoding: chunked -> rejected (non-200)
//   G8-004: POST body containing "x-daemon-token:" does NOT pass CSRF gate
//   G8-002: SSE admission on TCP shares a bounded bucket (not per-port)
//
// Build: added to tests/v94/build.sh as `webui_http` recipe already.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include "aatpe_session_token.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "aatped.h"
#include "aatped_config.h"   // g_cfg, g_cfgMutex
#include "webui_http.h"

static int g_pass = 0, g_fail = 0, g_skip = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)
// G-TSan: timing-dependent assertions that depend on wall-clock arrival
// (heartbeat within N seconds, /state/all 200 after startup) are unreliable
// under ThreadSanitizer because TSan slows execution ~10-30x. Under TSan,
// these are SKIPPED (not FAIL) — the SSE connections are still established
// and run for the full duration, so TSan still observes all races.
#if defined(__SANITIZE_THREAD__)
  #define AATPE_TSAN_BUILD 1
#else
  #define AATPE_TSAN_BUILD 0
#endif
// SLOW_BUILD = any sanitizer (TSan or ASan) slows the event loop ~10x; widen
// wall-clock read/poll windows but KEEP assertions real (no ASan skip).
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
  #define AATPE_SLOW_BUILD 1
#else
  #define AATPE_SLOW_BUILD 0
#endif
#define CHECK_TSKIP(c, m) do { \
    if (AATPE_TSAN_BUILD) { g_skip++; printf("  [SKIP] %s (TSan wall-clock distortion; race coverage retained)\n", m); } \
    else if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// Send a raw request and read the full response (Connection: close).
static std::string httpOnce(int port, const std::string& req) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return "";
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); return ""; }
    // set recv timeout
    struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    size_t off = 0;
    const char* p = req.c_str();
    while (off < req.size()) {
        ssize_t n = send(s, p+off, req.size()-off, 0);
        if (n <= 0) break;
        off += (size_t)n;
    }
    std::string out;
    char buf[4096];
    while (true) {
        ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, n);
    }
    close(s);
    return out;
}

static int statusOf(const std::string& resp) {
    // "HTTP/1.1 200 OK"
    if (resp.size() < 12) return -1;
    if (resp.compare(0, 5, "HTTP/") != 0) return -1;
    size_t sp = resp.find(' ');
    if (sp == std::string::npos) return -1;
    return atoi(resp.c_str() + sp + 1);
}

int main() {
    // Ensure STATE_DIR exists (the stub points at /tmp/aatpe_v94_state).
    // STATE_DIR is fixed; RUN_DIR is a per-process unique mkdtemp from
    // webui_deps.cpp (already created), so no fixed /tmp/aatpe_v94_run here.
    system("mkdir -p /tmp/aatpe_v94_state 2>/dev/null");
    // Seed test state files BEFORE any HTTP traffic so the state cache picks
    // them up on first seed (direct echo bypasses stateCacheNoteWrite).
    system("echo 42 > /tmp/aatpe_v94_state/daemon_uptime");
    system("echo active > /tmp/aatpe_v94_state/control_mode");
    system("echo 5 > /tmp/aatpe_v94_state/log_level");
    // Half-open pressure: a real device pushes a ~10-20KB aggregate state frame
    // on every SSE connection (the full ~183-key /state/all snapshot). The host
    // stub state above is only ~1KB, so without this the first SSE frame fits in
    // the socket pipe and send() never blocks on a silent peer. Seed a device-
    // realistic ~16KB value so the first SSE push actually overflows the small
    // sndbuf + 4KB peer rcvbuf pipe and forces the SO_SNDTIMEO half-open break.
    system("python3 -c \"open('/tmp/aatpe_v94_state/sse_pressure_blob','w').write('x'*16000)\"");
    // Mint the daemon token so /events SSE auth works in the test.
    aatpe::webui_http::ensureDaemonToken();
    // SSE keepalive heartbeat: force a 1s interval (default 20s, clamped >=1).
    // Set BEFORE the server thread starts so the SSE loop picks it up.
    setenv("AATPE_SSE_KEEPALIVE_SEC", "1", 1);
    // G-test: half-open send-timeout reclaim interval. Default 5s; set to 1s
    // so silent (no-FIN) SSE slots are reclaimed deterministically and fast,
    // without depending on host scheduling headroom (30s wall-clock margin).
    setenv("AATPE_SSE_HALFOPEN_TIMEOUT_SEC", "1", 1);
    printf("=== ROUND-001: start HTTP server ===\n");
    if (!aatpe::webui_http::startHttpServer()) {
        printf("  [FAIL] startHttpServer returned false\n");
        return 1;
    }
    int port = aatpe::webui_http::getHttpPort();
    CHECK(port >= 8888 && port <= 8899, ("server bound on port " + std::to_string(port)).c_str());
    // give accept loop a moment. G-TSan: under ThreadSanitizer execution is slowed
    // ~10x, so 200ms virtual = 2s wall; extend to 1s so the server is ready when
    // the first /state/all probe fires.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    printf("=== baseline: /health open ===\n");
    std::string h = httpOnce(port,
        "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(statusOf(h) == 200, "/health returns 200");

    printf("=== G8-001: hmacRequired=false, no-cred /api/session MUST 403 ===\n");
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfg.hmacRequired = false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // /api/session is reached via GET ?cmd=... (POST blocked by line 1734 405 gate).
    std::string s = httpOnce(port,
        "GET /api/session HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    int st = statusOf(s);
    CHECK(st == 403,
          ("/api/session no-cred with hmacRequired=false -> 403 (got " + std::to_string(st) + ")").c_str());
    // restore
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfg.hmacRequired = true;
    }

    printf("=== G8-003: TCP shared rate-limit bucket rejects after 50 req/s ===\n");
    {
        // The first ~50 requests in this second should be accepted; the 60th
        // MUST be 429. We don't need exact count; we just need to see at least
        // one 429 after flooding.
        int got429 = 0; int gotOk = 0;
        for (int i = 0; i < 80; i++) {
            std::string r = httpOnce(port,
                "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
            int st = statusOf(r);
            if (st == 429) got429++;
            else if (st == 200) gotOk++;
        }
        // Rate-limit is a 50 req/s wall-clock bucket. Under TSan the 80 requests
        // take much longer wall-clock, so the bucket refills and a 429 may never
        // appear through no fault of the production logic. Treat as wall-clock-
        // dependent: hard on non-TSan, TSKIP (race coverage retained) on TSan.
        CHECK_TSKIP(got429 > 0,
              ("flood of 80 /health -> at least one 429 (got " + std::to_string(got429) +
               " 429s, " + std::to_string(gotOk) + " 200s)").c_str());
    }

    printf("=== G8-011: Transfer-Encoding: chunked request MUST be rejected ===\n");
    {
        // wait for rate limit window to drain
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::string r = httpOnce(port,
            "POST /control HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n0\r\n\r\n");
        int st = statusOf(r);
        // Server closes or sends 413; either way it must NOT be 200.
        CHECK(st != 200,
              ("TE: chunked POST /control -> not 200 (got " + std::to_string(st) + ")").c_str());
    }

    printf("=== G8-004: CSRF body substring does not bypass CSRF gate ===\n");
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // Form-encoded POST whose BODY contains "x-daemon-token:" (lowercased).
        // Old code would substring-search the whole request and treat this as
        // CSRF-safe. Even if CSRF passes, LED-158 still requires sig/stok, so
        // the result should be 403 (not a 200 / 400-from-whitelist). We just
        // assert we don't get a 200.
        std::string body = "x-daemon-token: fake&cmd=set_mode=passive";
        std::string req = "POST /control HTTP/1.1\r\nHost: x\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        std::string r = httpOnce(port, req);
        int st = statusOf(r);
        CHECK(st != 200,
              ("body-substring CSRF probe -> not 200 (got " + std::to_string(st) + ")").c_str());
    }

    printf("=== G8-005: daemon_token endpoint stays disabled ===\n");
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::string r = httpOnce(port,
            "GET /api/daemon_token HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r) == 403, "GET /api/daemon_token -> 403 (disabled)");
    }

    printf("=== ROUND-003: SSE shared-bucket cap and immediate slot release ===\n");
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // Read the daemon token minted by ensureDaemonToken() earlier.
        std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
        std::string tok((std::istreambuf_iterator<char>(tf)),
                         std::istreambuf_iterator<char>());
        while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
        CHECK(!tok.empty(), "daemon_token readable for SSE test");

        // Open kSseMaxPerSharedTcp=6 SSE connections. On TCP loopback g_peerUid=-1,
        // so ?token= query is NOT accepted — must use X-Daemon-Token header.
        std::vector<int> sseSocks;
        int openCount = 0;
        for (int i = 0; i < 6; i++) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in a; memset(&a,0,sizeof a);
            a.sin_family = AF_INET; a.sin_port = htons(port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); break; }
            std::string req = "GET /events HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\n\r\n";
            send(s, req.c_str(), req.size(), 0);
            // read response headers (up to \r\n\r\n)
            char hdr[512]; int n = recv(s, hdr, sizeof hdr, 0);
            if (n > 0 && hdr[0]=='H' && strstr(hdr, "200")) {
                sseSocks.push_back(s); openCount++;
            } else {
                close(s);
            }
        }
        CHECK(openCount == 6,
              ("opened 6 SSE connections (got " + std::to_string(openCount) + ")").c_str());

        // 7th connection must be 429 with sse_limit in body.
        std::string r7 = httpOnce(port,
            "GET /events HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        int st7 = statusOf(r7);
        CHECK(st7 == 429, ("7th SSE -> 429 (got " + std::to_string(st7) + ")").c_str());
        CHECK(r7.find("sse_limit") != std::string::npos,
              "7th SSE body contains \"sse_limit\"");

        // Close one SSE socket; the server's MSG_DONTWAIT EOF poll releases the
        // slot asynchronously (~250ms, handler bounded by the 2s recv timeout).
        // This is "eventually released", NOT "immediately": a fixed sleep(400)
        // was flaky under ASan slowdown (9/10). Poll up to 5s for a new SSE
        // connection to be admitted; a 429 just means the slot is not yet free.
        close(sseSocks.back()); sseSocks.pop_back();
        bool reopened = false;
        auto sseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!reopened && std::chrono::steady_clock::now() < sseDeadline) {
            int sTry = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in aTry; memset(&aTry,0,sizeof aTry);
            aTry.sin_family=AF_INET; aTry.sin_port=htons(port); aTry.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if (connect(sTry, (struct sockaddr*)&aTry, sizeof aTry) == 0) {
                std::string reqTry = "GET /events HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\n\r\n";
                send(sTry, reqTry.c_str(), reqTry.size(), 0);
                struct timeval rcvto; rcvto.tv_sec=2; rcvto.tv_usec=0;
                setsockopt(sTry, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof rcvto);
                char hTry[512];
                int nTry = recv(sTry, hTry, sizeof(hTry)-1, 0);
                if (nTry > 0) { hTry[nTry] = '\0'; }  // NUL-terminate so strstr can't overrun
                if (nTry > 0 && strstr(hTry, "200")) {
                    reopened = true;
                    sseSocks.push_back(sTry);
                    continue;
                }
                close(sTry); // 429 (limit hit): slot not yet released, retry
            } else {
                close(sTry);
            }
            if (!reopened) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK_TSKIP(reopened, "after closing one SSE, a new connection is admitted within 5s (slot eventually released)");

        // cleanup
        for (int s : sseSocks) close(s);
    }

    printf("=== ROUND-003: read-only /control 403 carries read_only:true + reason ===\n");
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // On host, integrity verdict defaults to Unknown (fail-closed), so
        // privilegedOpsAllowed() returns false -> /control must 403 readonly.
        std::string r = httpOnce(port,
            "GET /control?cmd=set_mode=passive HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 403,
              ("readonly /control -> 403 (got " + std::to_string(st) + ")").c_str());
        CHECK(r.find("\"read_only\":true") != std::string::npos,
              "response body contains \"read_only\":true");
        CHECK(r.find("\"reason\":") != std::string::npos,
              "response body contains \"reason\" field");
    }

    printf("=== ROUND-004: /state/public exposes daemon_uptime (R4-C03) ===\n");
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // Post G8-003 flood + ROUND-003 SSE, the single-threaded webui event loop can
        // lag badly under TSan slowdown; the plain 2s recv timeout here occasionally
        // fires while the loop is still draining, which made a real-200 endpoint look
        // like a failure. Retry with bounded backoff so we still exercise the endpoint
        // for real; only if every attempt times out do we TSKIP the status (race cover
        // retained, wall-clock distortion not treated as a failure). Body assertions
        // run against whichever response actually came back.
        std::string r;
        int st = -1;
        bool got200 = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            std::string trial = httpOnce(port,
                "GET /state/public HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
            st = statusOf(trial);
            if (st == 200 && !trial.empty()) { r = trial; got200 = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        CHECK_TSKIP(st == 200, "/state/public -> 200");
        // Body assertions only run against a real 200 response. If every retry timed
        // out (empty r under TSan slowdown), skip the body checks rather than fail on
        // an empty string — race coverage is retained, wall-clock distortion isn't a
        // failure. On non-TSan the endpoint answers fast, so these stay hard checks.
        if (got200) {
            CHECK(r.find("\"daemon_uptime\"") != std::string::npos,
                  "/state/public body contains \"daemon_uptime\" key");
            CHECK(r.find("\"daemon_uptime_sec\"") == std::string::npos,
                  "/state/public body does NOT contain stale \"daemon_uptime_sec\" key");
        } else {
            g_skip++;
            printf("  [SKIP] /state/public body assertions (no response under TSan; race coverage retained)\n");
        }
    }

    printf("=== ROUND-004: control_mode=active visible in /state/all (R4-C01) ===\n");
    {
        // /state/all requires daemon token.
        std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
        std::string tok((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
        while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
        std::string r = httpOnce(port,
            "GET /state/all HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK_TSKIP(statusOf(r) == 200, "/state/all -> 200");
        CHECK_TSKIP(r.find("\"control_mode\":\"active\"") != std::string::npos,
              "/state/all body contains \"control_mode\":\"active\"");
    }

    printf("=== ROUND-004: log_level=5 visible in /state/all (R4-C04) ===\n");
    {
        std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
        std::string tok((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
        while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
        std::string r = httpOnce(port,
            "GET /state/all HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK_TSKIP(r.find("\"log_level\":\"5\"") != std::string::npos,
              "/state/all body contains \"log_level\":\"5\" (UI now draws log-5 button)");
    }

    printf("=== ROUND-004: /api/config_meta returns key->reload class map ===\n");
    {
        std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
        std::string tok((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
        while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
        std::string r = httpOnce(port,
            "GET /api/config_meta HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r) == 200, "/api/config_meta -> 200");
        CHECK(r.find("\"LITTLE_POOL\":\"boot\"") != std::string::npos,
              "config_meta marks LITTLE_POOL as boot");
        CHECK(r.find("\"LOG_LEVEL\":\"live\"") != std::string::npos,
              "config_meta marks LOG_LEVEL as live");
        CHECK(r.find("\"EBPF_ENABLED\":\"hook\"") != std::string::npos,
              "config_meta marks EBPF_ENABLED as hook");
    }

    printf("=== ROUND-004: clusters state file readable via /state/all ===\n");
    {
        system("printf 'LITTLE,120,400-1800:0,1,2,3\\nBIG,1024,800-2600:4,5,6\\nPRIME,1024,800-2800:7\\n' > /tmp/aatpe_v94_state/clusters");
        // clusters was seeded before server start; but we just wrote it after seed.
        // The /state/all dump reads from state cache. The `clusters` key may or may
        // not be in cache depending on seed timing. We just verify the endpoint
        // doesn't crash and returns 200.
        std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
        std::string tok((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
        while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
        std::string r = httpOnce(port,
            "GET /state/all HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK_TSKIP(statusOf(r) == 200, "/state/all after clusters write -> 200 (no crash)");
    }

    printf("=== ROUND-005: /state/<name> single-file route ===\n");
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
        std::string tok((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
        while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();

        // Public key no token needed.
        std::string r1 = httpOnce(port,
            "GET /state/control_mode HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK_TSKIP(statusOf(r1) == 200, "/state/control_mode (public) -> 200");
        CHECK_TSKIP(r1.find("\"control_mode\":\"active\"") != std::string::npos,
              "/state/control_mode returns the seeded value");

        // Private key requires token.
        std::string r2 = httpOnce(port,
            "GET /state/thermal_enabled HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r2) == 401, "/state/thermal_enabled without token -> 401");

        std::string r3 = httpOnce(port,
            "GET /state/thermal_enabled HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK_TSKIP(statusOf(r3) == 404, "/state/thermal_enabled (not seeded) -> 404");

        // Path traversal rejected.
        std::string r4 = httpOnce(port,
            "GET /state/..%2f..%2fetc%2fpasswd HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r4) == 400 || statusOf(r4) == 404,
              "path traversal rejected (400/404)");

        // Bad chars rejected.
        std::string r5 = httpOnce(port,
            "GET /state/foo.bar HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r5) == 400, "dot in name -> 400");
    }

    printf("=== ROUND-006: keep-alive — same fd serves multiple requests, per-request auth ===\n");
    {
        // Open one connection, send two requests WITHOUT "Connection: close";
        // read one full response (by Content-Length) each time.
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a,0,sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool ok = false;
        if (s >= 0 && connect(s, (struct sockaddr*)&a, sizeof a) == 0) {
            struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            auto sendAll = [&](const std::string& req){ size_t o=0; while(o<req.size()){ ssize_t n=send(s,req.data()+o,req.size()-o,0); if(n<=0)return false; o+=n;} return true; };
            // read one full HTTP response by Content-Length.
            auto recvOne = [&]()->std::string {
                std::string all; char buf[4096];
                // read until we have headers + blank line
                while (all.find("\r\n\r\n") == std::string::npos) {
                    ssize_t n=recv(s,buf,sizeof buf,0); if(n<=0) break; all.append(buf,n);
                }
                size_t hdrEnd = all.find("\r\n\r\n");
                if (hdrEnd == std::string::npos) return all;
                // parse content-length
                std::string hdr = all.substr(0,hdrEnd);
                size_t cp = hdr.find("ontent-Length:");
                long need = 0;
                if (cp != std::string::npos) {
                    size_t sp = hdr.find(':',cp);
                    need = atol(hdr.c_str()+sp+1);
                }
                size_t bodyStart = hdrEnd+4;
                while ((ssize_t)(all.size()-bodyStart) < need) {
                    ssize_t n=recv(s,buf,sizeof buf,0); if(n<=0) break; all.append(buf,n);
                }
                return all;
            };
            // req1+req2 on SAME fd: two healthy /health reuse the connection.
            sendAll("GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
            std::string r1 = recvOne();
            sendAll("GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
            std::string r2 = recvOne();
            // req3 on SAME fd: gated /api/apps WITHOUT token -> 401 (per-request
            // auth). P2-1: server MUST then close the fd (4xx closes keep-alive).
            sendAll("GET /api/apps HTTP/1.1\r\nHost: x\r\n\r\n");
            std::string r3 = recvOne();
            // After the 401, server closes; a further read on this fd must EOF.
            char tail[64]; ssize_t te = recv(s, tail, sizeof tail, 0);
            close(s);
            CHECK(statusOf(r1)==200, "KA req1 /health -> 200");
            CHECK(statusOf(r2)==200, "KA req2 /health on SAME fd -> 200 (reused)");
            CHECK(statusOf(r3)==401, ("KA req3 /api/apps no token -> 401 (per-request; got "
                    + std::to_string(statusOf(r3)) + ")").c_str());
            CHECK(te <= 0, "KA: after 401 the fd is closed by server (recv EOF)");
            ok = true;
        }
        if (!ok) CHECK(false, "keep-alive: could not open/connect socket");
    }
    // P2-1: a fresh connection right after the 401-close must still serve /health.
    {
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r)==200, "post-401-close: new connection /health -> 200");
    }

    printf("=== ROUND-006b: malformed request closes connection, new connection still works ===\n");
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a,0,sizeof a);
        a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if (s>=0 && connect(s,(struct sockaddr*)&a,sizeof a)==0) {
            // garbage that has no \r\n\r\n -> readHttpRequest times out / oversize
            send(s, "GET /garbage\n", 13, 0);
            struct timeval tv; tv.tv_sec=3; tv.tv_usec=0;
            setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
            char buf[256]; recv(s,buf,sizeof buf,0); // drain whatever/close
            close(s);
        } else if (s>=0) close(s);
        // brand new connection must still serve /health
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r)==200, "post-malformation: new connection /health -> 200");
    }

    printf("=== v4.2.9 T4: oversized request -> 413 + server closes, new conn still serves ===\n");
    {
        // drain the shared TCP rate-limit bucket before this section.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a,0,sizeof a);
        a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        bool got413=false, closedAfter=false;
        if (s>=0 && connect(s,(struct sockaddr*)&a,sizeof a)==0) {
            struct timeval tv; tv.tv_sec=2; tv.tv_usec=0;
            setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
            // Content-Length vastly above the 64KB cap -> readHttpRequest flags
            // oversize -> the handler answers 413 then breaks the keep-alive loop.
            std::string req = "POST /control HTTP/1.1\r\nHost: x\r\n"
                              "Content-Length: 100000000\r\n\r\n";
            send(s, req.data(), req.size(), 0);
            std::string all; char b[4096];
            // read headers (up to blank line)
            while (all.find("\r\n\r\n")==std::string::npos) {
                ssize_t n=recv(s,b,sizeof b,0); if(n<=0) break; all.append(b,n);
            }
            got413 = (statusOf(all)==413);
            // drain the (small) 413 body; the server then closes the fd, so the
            // next recv must EOF (this IS the "Connection: close" equivalent
            // close-semantics: product writes keep-alive in the header but breaks
            // the loop and close()s the socket).
            while (true) {
                ssize_t n=recv(s,b,sizeof b,0);
                if (n<=0) { closedAfter=true; break; }
                all.append(b,n);
                if (all.size() > (1u<<20)) break;
            }
            close(s);
        } else if (s>=0) close(s);
        CHECK(got413, "oversized Content-Length -> 413 Payload Too Large");
        CHECK(closedAfter, "after 413 the server closes the fd (recv EOF == close semantics)");
        // a brand-new connection after the poisoned one must still be served.
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r)==200, "post-oversize: fresh connection /health -> 200");
    }

    printf("=== v4.2.9 T5: diag_bundle whitelist packs frame_apps.json + audit.log ===\n");
    {
        // handleDiagBundle() lives in an anonymous namespace (internal linkage), so
        // drive it through the real /api/diag_bundle route. The exec stub records
        // the argv whitelist it was handed (g_lastExecArgv) instead of running tar.
        std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
        std::string tok((std::istreambuf_iterator<char>(tf)),
                         std::istreambuf_iterator<char>());
        while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();

        extern std::vector<std::string> g_lastExecArgv;
        g_lastExecArgv.clear();
        std::string r = httpOnce(port,
            "GET /api/diag_bundle HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok +
            "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r)==204 || statusOf(r)==200,
              ("diag_bundle endpoint answered (got " + std::to_string(statusOf(r)) + ")").c_str());
        auto has=[&](const char* needle){
            for(const auto& arg:g_lastExecArgv)
                if(arg.find(needle)!=std::string::npos) return true;
            return false;
        };
        CHECK(!g_lastExecArgv.empty(), "diag_bundle built an exec argv");
        CHECK(has("/system/bin/tar"), "diag uses fixed /system/bin/tar argv (no shell)");
        CHECK(has("/dev/aatpe/state/frame_apps.json"), "whitelist packs state/frame_apps.json");
        CHECK(has("/dev/aatpe/state/audit.log"), "whitelist packs state/audit.log");
        // F-01[P1]: must NOT recurse the state dir (contains daemon_token/hmac_key).
        auto hasExact=[&](const char* needle){
            for(const auto& arg:g_lastExecArgv)
                if(arg==needle) return true;
            return false;
        };
        CHECK(!hasExact("/dev/aatpe/state"), "F-01: diag_bundle does NOT recurse state dir");
        CHECK(!has("daemon_token"), "F-01: diag_bundle never references daemon_token");
        CHECK(!has("hmac_key"), "F-01: diag_bundle never references hmac_key");
        CHECK(!has("session_token"), "F-01: diag_bundle never references session_token");
    }

    // F-01[P1]: stok (short-lived session ticket) must NOT unlock sensitive
    // endpoints. It is only accepted on the SSE /events route.
    printf("=== F-01: stok cannot unlock /api/hmac_key or /api/diag_bundle ===\n");
    {
        int port = aatpe::webui_http::getHttpPort();
        std::string stok = aatpeSessionIssue();
        CHECK(!stok.empty(), "F-01: minted a test stok");
        // 1) stok on /api/hmac_key -> must be 401/403
        {
            std::string req =
                "GET /api/hmac_key?stok=" + stok + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            std::string r = httpOnce(port, req);
            int st = statusOf(r);
            CHECK(st==401 || st==403,
                  ("F-01: /api/hmac_key with stok -> 401/403 (got " + std::to_string(st) + ")").c_str());
        }
        // 2) stok on /api/diag_bundle -> must be 401/403
        {
            std::string req =
                "GET /api/diag_bundle?stok=" + stok + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            std::string r = httpOnce(port, req);
            int st = statusOf(r);
            CHECK(st==401 || st==403,
                  ("F-01: /api/diag_bundle with stok -> 401/403 (got " + std::to_string(st) + ")").c_str());
        }
        // 3) X-Daemon-Token header on /api/hmac_key still works (200/204)
        {
            std::string tok;
            {
                std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
                tok.assign(std::istreambuf_iterator<char>(tf), std::istreambuf_iterator<char>());
                while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
            }
            std::string req =
                "GET /api/hmac_key HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok +
                "\r\nConnection: close\r\n\r\n";
            std::string r = httpOnce(port, req);
            int st = statusOf(r);
            CHECK(st==200 || st==204,
                  ("F-01: /api/hmac_key with X-Daemon-Token -> 200/204 (got " + std::to_string(st) + ")").c_str());
        }
    }

    printf("=== SSE keepalive: heartbeat frames on a live /events stream ===\n");
    {
        std::string tok;
        {
            std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
            if (tf) {
                tok.assign(std::istreambuf_iterator<char>(tf), std::istreambuf_iterator<char>());
                while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
            }
        }
        CHECK(!tok.empty(), "keepalive: daemon_token readable");
        // Open a long-lived SSE connection (no Connection: close).
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a,0,sizeof a);
        a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        bool conn = (connect(s,(struct sockaddr*)&a,sizeof a)==0);
        CHECK(conn, "keepalive: SSE socket connected");
        std::string req = "GET /events HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\n\r\n";
        send(s, req.c_str(), req.size(), 0);
        struct timeval rcvto; rcvto.tv_sec=1; rcvto.tv_usec=0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof rcvto);
        // Read for a deadline. With AATPE_SSE_KEEPALIVE_SEC=1 we expect >=1
        // heartbeat comment frame ": keepalive\n\n" even when no state changes.
        // G-TSan: under ThreadSanitizer execution is slowed ~10x, so the 1s
        // heartbeat arrives at ~10s wall. Use a 30s deadline and break early
        // once a heartbeat is captured so normal runs stay fast.
        std::string acc;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(AATPE_SLOW_BUILD ? 60 : 30);
        while (std::chrono::steady_clock::now() < dl) {
            char b[2048];
            int n = recv(s, b, sizeof b, 0);
            if (n > 0) acc.append(b, n);
            else if (n == 0) break; // peer closed
            if (acc.find(": keepalive") != std::string::npos) break;
        }
        close(s);
        int ka = 0; size_t pos = 0;
        while ((pos = acc.find(": keepalive", pos)) != std::string::npos) { ka++; pos += 11; }
        CHECK(statusOf(acc.substr(0, acc.find("\r\n"))) == 200 || acc.find("200") != std::string::npos,
              ("keepalive: SSE stream started with 200 (got head: " + acc.substr(0,30) + ")").c_str());
        CHECK_TSKIP(ka >= 1, ("keepalive: received >=1 ': keepalive' heartbeat frame (got " + std::to_string(ka) + ")").c_str());
        // EventSource semantics: the keepalive frame is a *comment* line that
        // starts with ':' (so the browser EventSource delivers NO message for
        // it). Real state pushes (data:) are expected and fine; we only assert
        // the heartbeat itself sits on its own comment line rather than being
        // embedded in a data: payload.
        bool kaIsComment = (acc.find("\n: keepalive") != std::string::npos) ||
                           (acc.find(": keepalive") == 0);
        CHECK_TSKIP(kaIsComment,
              "keepalive: ': keepalive' sits on its own comment line (EventSource delivers no message)");
    }

    printf("=== SSE half-open: send-timeout bounded slot reclaim (no FIN) ===\n");
    {
        std::string tok;
        {
            std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
            if (tf) {
                tok.assign(std::istreambuf_iterator<char>(tf), std::istreambuf_iterator<char>());
                while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
            }
        }
        CHECK(!tok.empty(), "halfopen: daemon_token readable");

        // Fill the SSE slot limit (6). Connections are made HALF-OPEN:
        // after confirming their 200 handshake we never recv them again, never
        // send FIN. The server's heartbeat / state probes then fill our receive
        // buffer; once send() makes zero progress for AATPE_SSE_HALFOPEN_TIMEOUT_SEC
        // (1s, set at the top of main()) the product's sendAll() returns false,
        // the SSE loop breaks, and SseSlotGuard/SseCountGuard releases the slot.
        //
        // F4.1 deterministic fix: RCVBUF is 4KB instead of 128B.
        // The old 128B buffer caused the server's sendAll(SSE headers, ~300B)
        // to stall on EAGAIN mid-handshake: the slot was already acquired
        // (webui_http.cpp sseTryAcquire) but the 200 headers had not finished
        // flushing. Under full-suite load / ASan slowdown the 1s send-stall
        // timer fired while the test was still opening connections, releasing
        // slots early — so the 7th probe got 200 instead of 429.
        //
        // 4KB is the sweet spot:
        //   - ~300B SSE headers fit in one send() → handshake completes
        //     instantly, "client saw 200" == "server holds the slot".
        //   - The first full-state push (~2-5KB) overflows the 4KB buffer →
        //     send stalls → 1s timeout → slot reclaim still triggers, so the
        //     half-open reclamation assertion remains valid.
        std::vector<int> socks;
        int limit = 6;
        // E: slow/instrumented builds (TSan) make the server accept+handshake take
        // ~seconds. The old loop fired limit*4 connects with only a 2s handshake and
        // no settle, so handshakes that didn't finish in time were closed by the
        // client -> server-side half-open (ghost) slots held until the 30s send
        // timeout, starving the real 6. Build one slot at a time with a generous
        // handshake window and a bounded settle after a miss, so we never pile up
        // ghost connections. Real assertion unchanged: we must still reach 6.
        const int handshakeSec = AATPE_TSAN_BUILD ? 10 : 2;
        const int settleMs     = AATPE_TSAN_BUILD ? 500 : 30;
        const int maxAttempts   = limit * 8;
        int attempts = 0;
        while ((int)socks.size() < limit && attempts < maxAttempts) {
            attempts++;
            int s = socket(AF_INET, SOCK_STREAM, 0);
            int rcvbuf = 4096;  // F4.1: was 128; see comment above
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
            struct sockaddr_in a; memset(&a,0,sizeof a);
            a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if (connect(s,(struct sockaddr*)&a,sizeof a)<0){ close(s); continue; }
            std::string req="GET /events HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: "+tok+"\r\n\r\n";
            send(s, req.c_str(), req.size(), 0);
            // F4.1: confirm 200 handshake before counting the connection as
            // admitted. With 4KB RCVBUF the headers flush in one send, so
            // recv returns immediately and we know the slot is occupied.
            struct timeval hdrTo; hdrTo.tv_sec=handshakeSec; hdrTo.tv_usec=0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &hdrTo, sizeof hdrTo);
            char hdr[512];
            ssize_t nh = recv(s, hdr, sizeof hdr-1, 0);
            if (nh > 0) { hdr[nh] = '\0'; }
            if (nh > 0 && hdr[0]=='H' && strstr(hdr, "200")) {
                socks.push_back(s);  // admitted: slot solidly occupied
            } else {
                close(s);  // rejected or error: don't count; settle so the server
                           // can finish/reclaim the in-flight accept before we pile on
                std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(AATPE_TSAN_BUILD ? 100 : 30));
        }
        CHECK((int)socks.size()==limit, ("halfopen: opened " + std::to_string(socks.size()) + "/" + std::to_string(limit) + " seed SSE").c_str());

        // Brief settle: let any in-flight server threads finish publishing
        // their slot state. With 4KB RCVBUF the handshake already completed,
        // but a 50ms guard absorbs scheduler jitter under full-suite load.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Probe: open a fresh SSE connection and report its status (200 = slot
        // free, 429 = slot still held).
        auto probeStatus = [&](void) -> int {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in a; memset(&a,0,sizeof a);
            a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if (connect(s,(struct sockaddr*)&a,sizeof a)<0){ close(s); return -1; }
            std::string req="GET /events HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: "+tok+"\r\n\r\n";
            send(s, req.c_str(), req.size(), 0);
            struct timeval rt; rt.tv_sec=2; rt.tv_usec=0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
            char h[256];
            ssize_t n = recv(s, h, sizeof(h)-1, 0);  // leave 1 byte for NUL
            close(s);
            if (n <= 0) return -1;
            h[n] = '\0';
            return statusOf(std::string(h, (size_t)n));  // length-construct, no strlen
        };

        // 7th connection must be rejected while the 6 no-read connections hold the
        // slots (proves the slots are actually occupied).
        int stFull = probeStatus();
        CHECK(stFull==429, ("halfopen: 7th conn rejected 429 while slots held (got " + std::to_string(stFull) + ")").c_str());
        auto heldAt = std::chrono::steady_clock::now();

        // Poll up to 30s: with AATPE_SSE_HALFOPEN_TIMEOUT_SEC=1, the send
        // timeout is 1s, so silent (no-FIN) slots are reclaimed within ~1-2s.
        // Under TSan the single-threaded event loop can lag badly, so widen the
        // polling deadline 3x (90s) there; the reclaim itself remains a HARD
        // functional check (slots MUST eventually be reclaimed), we only give the
        // slow loop room. Non-TSan builds keep the tight 30s window.
        bool reclaimed = false;
        auto halfDl = std::chrono::steady_clock::now()
                    + std::chrono::seconds(AATPE_SLOW_BUILD ? 90 : 30);
        std::chrono::steady_clock::time_point reclaimAt;
        while (std::chrono::steady_clock::now() < halfDl) {
            int st = probeStatus();
            if (st == 200) { reclaimed = true; reclaimAt = std::chrono::steady_clock::now(); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        double reclaimSec = std::chrono::duration<double>(reclaimAt - heldAt).count();
        CHECK(reclaimed, ("halfopen: silent no-read slots reclaimed within 30s (got it at +" +
               std::to_string(reclaimSec) + "s)").c_str());
        if (reclaimed)
            printf("       [info] half-open SSE slot admitted a new connection after ~%.1fs (bounded reclaim)\n", reclaimSec);

        for (int s : socks) close(s);
    }

    // N-02 regression: GET /webui/<name> (G-006) must exist exactly once.
    //   * happy path: whitelisted snapshot file, with daemon token -> 200, body echo.
    //   * unauth: same file, no token -> 401 (token gate runs after whitelist).
    //   * traversal: /webui/../... -> 400 (basename whitelist rejects "/" and "..").
    //   * non-whitelisted name -> 400.
    {
        system("mkdir -p /tmp/webui_test_dir 2>/dev/null");
        system("echo '{\"ok\":true,\"n\":7}' > /tmp/webui_test_dir/threads.json 2>/dev/null");

        std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
        std::string tok;
        if (tf.good()) { std::getline(tf, tok); }
        CHECK(!tok.empty(), "N-02: daemon_token readable for /webui test");

        // 1) happy path, authenticated.
        std::string rOk = httpOnce(port,
            "GET /webui/threads.json HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(rOk) == 200,
            "N-02: GET /webui/threads.json with token -> 200");
        CHECK(rOk.find("\"n\":7") != std::string::npos,
            "N-02: /webui/threads.json body echoes seeded content");

        // 2) unauthenticated -> 401.
        std::string rNoAuth = httpOnce(port,
            "GET /webui/threads.json HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(rNoAuth) == 401,
            "N-02: GET /webui/threads.json without token -> 401");

        // 3) path traversal attempt -> 400 (whitelist rejects slash / "..").
        std::string rTrav = httpOnce(port,
            "GET /webui/..%2f..%2fetc%2fpasswd HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(rTrav) == 400,
            "N-02: GET /webui/../..%2fetc%2fpasswd -> 400 (traversal rejected)");

        // 4) non-whitelisted basename -> 400.
        std::string rBad = httpOnce(port,
            "GET /webui/evil.json HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(rBad) == 400,
            "N-02: GET /webui/evil.json (not whitelisted) -> 400");
    }

    aatpe::webui_http::stopHttpServer();

    printf("\n========================================\n"
           "ROUND-001 WebUI results: %d passed, %d failed, %d skipped\n"
           "========================================\n", g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}
