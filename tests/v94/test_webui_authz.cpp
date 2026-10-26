// test_webui_authz.cpp - F-01 authorization regression (v4.2.9).
//
// Pins down the F-01 [P1] authZ hardening on webui_http.cpp:
//   - the short-lived ?stok= session ticket is SSE-ONLY. It must NOT authorize
//     /control writes, /api/hmac_key, /api/diag_bundle, or /api/session mint.
//   - those endpoints accept ONLY the long-lived daemon token (X-Daemon-Token
//     header on TCP loopback; ?token= only on a trusted AF_UNIX channel).
//   - /control write auth = (X-Daemon-Token header) OR (per-command
//     .sig/.ts/.nonce HMAC). Whitelist runs BEFORE auth, so an out-of-enum
//     command is 400 even with valid credentials.
//   - diag_bundle packs only a fixed named whitelist; it never recurses the
//     state dir and never references *token*/*key*/*session*.
//
// Link set mirrors the webui_http target. The host stub verifyCommandHmac()
// returns true, so a well-formed .sig/.ts/.nonce exercises the real control
// path end to end without embedding crypto in the test.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctime>

#include "webui_http.h"
#include "aatpe_session_token.h"
#include "integrity.h"
#include "aatped.h"
#include "aatped_config.h"   // g_cfg, g_cfgMutex

// Provided by tests/stubs/webui_deps.cpp (same stub set as webui_http target).
extern const char* STATE_DIR;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// Send a raw request, read the full response (Connection: close).
static std::string httpOnce(int port, const std::string& req) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return "";
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); return ""; }
    struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    size_t off = 0; const char* p = req.c_str();
    while (off < req.size()) { ssize_t n = send(s, p+off, req.size()-off, 0); if (n <= 0) break; off += (size_t)n; }
    std::string out; char buf[4096];
    while (true) { ssize_t n = recv(s, buf, sizeof buf, 0); if (n <= 0) break; out.append(buf, n); }
    close(s);
    return out;
}
static int statusOf(const std::string& resp) {
    if (resp.size() < 12) return -1;
    if (resp.compare(0, 5, "HTTP/") != 0) return -1;
    size_t sp = resp.find(' '); if (sp == std::string::npos) return -1;
    return atoi(resp.c_str() + sp + 1);
}
static std::string readDaemonToken() {
    std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
    std::string tok((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
    while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
    return tok;
}

int main() {
    system("mkdir -p /tmp/aatpe_v94_state 2>/dev/null");  // RUN_DIR is per-proc mkdtemp from webui_deps.cpp
    aatpe::webui_http::ensureDaemonToken();
    if (!aatpe::webui_http::startHttpServer()) { printf("  [FAIL] startHttpServer\n"); return 1; }
    int port = aatpe::webui_http::getHttpPort();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::string tok = readDaemonToken();
    CHECK(!tok.empty(), "daemon_token readable");

    // HMAC enforcement ON so unsigned writes are rejected (loopback default).
    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.hmacRequired = true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Freshly minted short-lived SSE ticket (5-min bearer).
    std::string stok = aatpeSessionIssue();
    CHECK(!stok.empty(), "minted a valid stok for negative tests");

    printf("=== F-01: fail-closed read-only gate (integrity Failed) ===\n");
    {
        aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Failed);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Even with the master token header, a read-only daemon MUST reject writes.
        std::string r = httpOnce(port,
            "GET /control?cmd=set_mode=active HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok +
            "\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 403, ("[5] integrity=Failed, header write -> 403 (got " + std::to_string(st) + ")").c_str());
        CHECK(r.find("read_only") != std::string::npos, "[5] read_only:true in body");
        aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600)); // drain rate-limit bucket

    // Host stub defaults the daemon to HMAC-read-only (fail-closed, so the
    // existing webui_http ROUND-003 read-only gate holds). For the write-auth
    // matrix below we must take the daemon OUT of read-only mode so legitimate
    // writes can actually be applied; integrity is already Ok.
    {
        extern bool g_hmacReadonlyFlag;
        g_hmacReadonlyFlag = false;
    }

    printf("=== F-01: /control write auth matrix ===\n");
    // [1] stok-only write must be rejected (stok is NOT a write credential).
    {
        std::string r = httpOnce(port,
            "GET /control?cmd=set_mode=active&stok=" + stok +
            " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 403, ("[1] /control write with ONLY stok -> 403 (got " + std::to_string(st) + ")").c_str());
    }
    // [2] X-Daemon-Token header, no sig/stok, valid command -> 200 applied.
    {
        std::string r = httpOnce(port,
            "GET /control?cmd=set_mode=active HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok +
            "\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 200, ("[2] /control write with X-Daemon-Token header -> 200 (got " + std::to_string(st) + ")").c_str());
        CHECK(r.find("\"ok\":true") != std::string::npos, "[2] body ok:true (command applied)");
    }
    // [3] per-command HMAC .sig/.ts/.nonce (no header) -> 200.
    {
        long long ts = (long long)time(nullptr);
        static int nonceCtr = 0;
        // Wire order matters: handleControl strips .sig (last), then .ts=
        // (rfind, swallows the rest), then .nonce=. So .nonce must PRECEDE .ts.
        std::string cmd = "set_mode=passive.nonce=n" + std::to_string(++nonceCtr) +
                          ".ts=" + std::to_string(ts) + ".sig=deadbeefcafe";
        std::string r = httpOnce(port,
            "GET /control?cmd=" + cmd +
            " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 200, ("[3] /control write with valid .sig/.ts/.nonce -> 200 (got " + std::to_string(st) + ")").c_str());
    }
    // [4] out-of-enum command, even WITH credentials -> 400 (whitelist first).
    {
        std::string r = httpOnce(port,
            "GET /control?cmd=set_profile=bogus HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok +
            "\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 400, ("[4] set_profile=bogus even with header -> 400 (got " + std::to_string(st) + ")").c_str());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    printf("=== F-01: stok cannot reach secret endpoints; header can ===\n");
    // [6a] stok on /api/hmac_key -> reject.
    {
        std::string r = httpOnce(port,
            "GET /api/hmac_key?stok=" + stok + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 401 || st == 403, ("[6a] /api/hmac_key with stok -> 401/403 (got " + std::to_string(st) + ")").c_str());
    }
    // [6b] stok on /api/diag_bundle -> reject.
    {
        std::string r = httpOnce(port,
            "GET /api/diag_bundle?stok=" + stok + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 401 || st == 403, ("[6b] /api/diag_bundle with stok -> 401/403 (got " + std::to_string(st) + ")").c_str());
    }
    // [6c] X-Daemon-Token header on /api/hmac_key -> 200/204.
    {
        std::string r = httpOnce(port,
            "GET /api/hmac_key HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok +
            "\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 200 || st == 204, ("[6c] /api/hmac_key with header -> 200/204 (got " + std::to_string(st) + ")").c_str());
    }
    // [6d] header on /api/diag_bundle -> 200/204 + argv whitelist invariants.
    {
        extern std::vector<std::string> g_lastExecArgv;
        g_lastExecArgv.clear();
        std::string r = httpOnce(port,
            "GET /api/diag_bundle HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok +
            "\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 200 || st == 204, ("[6d] /api/diag_bundle with header -> 200/204 (got " + std::to_string(st) + ")").c_str());
        auto has = [&](const char* n){ for(auto& a:g_lastExecArgv) if(a.find(n)!=std::string::npos) return true; return false; };
        auto hasExact = [&](const char* n){ for(auto& a:g_lastExecArgv) if(a==n) return true; return false; };
        CHECK(has("/system/bin/tar"), "[6d] diag uses fixed /system/bin/tar (no shell)");
        CHECK(has("/dev/aatpe/state/frame_apps.json"), "[6d] whitelist packs frame_apps.json");
        CHECK(has("/dev/aatpe/state/audit.log"), "[6d] whitelist packs audit.log");
        CHECK(!hasExact("/dev/aatpe/state"), "[6d] diag does NOT recurse the state dir");
        CHECK(!has("daemon_token"), "[6d] diag never references daemon_token");
        CHECK(!has("hmac_key"), "[6d] diag never references hmac_key");
        CHECK(!has("session_token"), "[6d] diag never references session_token");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    printf("=== F-01: stok still works on SSE, cannot mint a new ticket ===\n");
    // [7] stok on /events (SSE) -> 200 (event stream stays open to short ticket).
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a,0,sizeof a);
        a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        bool sse200 = false;
        if (connect(s,(struct sockaddr*)&a,sizeof a)==0) {
            std::string req = "GET /events?stok=" + stok + " HTTP/1.1\r\nHost: x\r\n\r\n";
            send(s, req.c_str(), req.size(), 0);
            struct timeval tv; tv.tv_sec=2; tv.tv_usec=0; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
            char h[512]; int n = recv(s, h, sizeof h, 0);
            if (n > 0 && strstr(h, "200")) sse200 = true;
        }
        close(s);
        CHECK(sse200, "[7] /events(SSE) with valid stok -> 200 (short ticket still SSE-only OK)");
    }
    // [8] stok cannot mint a fresh ticket via /api/session.
    {
        std::string r = httpOnce(port,
            "GET /api/session?stok=" + stok + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 401 || st == 403, ("[8] /api/session renewal with stok -> 401/403 (got " + std::to_string(st) + ")").c_str());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(600)); // drain rate-limit bucket

    printf("=== F-01: HMAC/session wire-order contract (x.nonce=.ts=.sig=) ===\n");
    {
        // pull "field":"value" out of a JSON response body
        auto jsonStr = [](const std::string& resp, const char* field) -> std::string {
            std::string key = std::string("\"") + field + "\":\"";
            size_t q = resp.find(key);
            if (q == std::string::npos) return "";
            q += key.size();
            size_t e = resp.find('"', q);
            return (e == std::string::npos) ? "" : resp.substr(q, e - q);
        };
        static int wc = 0;
        long long ts = (long long)time(nullptr);

        // [1] session_issue in CORRECT wire order -> 200 + a fresh token.
        std::string s1 = "session_issue.nonce=w" + std::to_string(++wc) +
                         ".ts=" + std::to_string(ts) + ".sig=deadbeef";
        std::string r1 = httpOnce(port,
            "GET /api/session?cmd=" + s1 +
            " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        int st1 = statusOf(r1);
        std::string minted = jsonStr(r1, "token");
        CHECK(st1 == 200, ("[1] session_issue correct order -> 200 (got " + std::to_string(st1) + ")").c_str());
        CHECK(!minted.empty(), "[1] session_issue returns a non-empty token");

        // [2] session_issue in REVERSED order (ts before nonce) -> 403 (nonce lost).
        std::string s2 = "session_issue.ts=" + std::to_string(ts) +
                         ".nonce=w" + std::to_string(++wc) + ".sig=deadbeef";
        std::string r2 = httpOnce(port,
            "GET /api/session?cmd=" + s2 +
            " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r2) == 403, ("[2] session_issue reversed order -> 403 (got " + std::to_string(statusOf(r2)) + ")").c_str());

        // [3] control correct order -> 200 first; replay the identical proof -> 403.
        std::string s3 = "set_mode=active.nonce=c" + std::to_string(++wc) +
                         ".ts=" + std::to_string(ts) + ".sig=deadbeef";
        std::string req3 = "GET /control?cmd=" + s3 +
                           " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        std::string rc3a = httpOnce(port, req3);  // first use of the nonce
        std::string rc3b = httpOnce(port, req3);  // identical proof -> replay
        CHECK(statusOf(rc3a) == 200,
              ("[3] control correct order first -> 200 (got " + std::to_string(statusOf(rc3a)) + ")").c_str());
        CHECK(statusOf(rc3b) == 403,
              ("[3] replay the same nonce/timestamp/sig -> 403 (got " + std::to_string(statusOf(rc3b)) + ")").c_str());

        // [4] control in REVERSED order -> 403 (mandatory nonce field lost).
        std::string s4 = "set_mode=active.ts=" + std::to_string(ts) +
                         ".nonce=c" + std::to_string(++wc) + ".sig=deadbeef";
        std::string r4 = httpOnce(port,
            "GET /control?cmd=" + s4 +
            " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r4) == 403, ("[4] control reversed order -> 403 (got " + std::to_string(statusOf(r4)) + ")").c_str());

        // [5] the token minted in [1] opens SSE /events via ?stok= -> 200.
        if (!minted.empty()) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in a; memset(&a,0,sizeof a);
            a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            bool ok = false;
            if (connect(s,(struct sockaddr*)&a,sizeof a)==0) {
                std::string req="GET /events?stok="+minted+" HTTP/1.1\r\nHost: x\r\n\r\n";
                send(s,req.c_str(),req.size(),0);
                struct timeval tv; tv.tv_sec=2; tv.tv_usec=0;
                setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
                char h[512]; int n=recv(s,h,sizeof h,0);
                if (n>0 && strstr(h,"200")) ok=true;
            }
            close(s);
            CHECK(ok, "[5] minted stok (correct-order issue) opens /events SSE -> 200");
        } else {
            CHECK(false, "[5] cannot test SSE: no minted token from [1]");
        }
    }

    aatpe::webui_http::stopHttpServer();
    printf("\n========================================\n"
           "F-01 authz results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
