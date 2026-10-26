// test_webui_p3_http.cpp — regression tests for the 2026-10-26 webui_http P3 audit
// fixes (HTTP-05 / HTTP-06 / HTTP-07). Socket-level: boots the real loopback HTTP
// server and drives raw bytes, exactly like test_webui_http.cpp.
//
//   HTTP-06: a POST/GET body containing the literal "connection: close" MUST NOT
//            terminate a keep-alive connection (the scan is header-block scoped).
//   HTTP-07: an Origin header with trailing whitespace still matches the byte-exact
//            allowlist, so Access-Control-Allow-Origin is emitted.
//   HTTP-05: unsigned cross-origin /control write with hmacRequired=false is
//            rejected 403 by the LED-158 gate (NOT 405) — pins the subsumption
//            proof that the retained 405 backstop is unreachable.
//
// Build: registered in tests/v94/build.sh as `webui_p3_http`.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "aatped.h"
#include "aatped_config.h"   // g_cfg, g_cfgMutex
#include "webui_http.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// Open a TCP connection to the loopback server.
static int dial(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); return -1; }
    return s;
}

static void sendAll(int s, const std::string& data) {
    size_t off = 0;
    const char* p = data.c_str();
    while (off < data.size()) {
        ssize_t n = send(s, p + off, data.size() - off, 0);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

// Read exactly one HTTP response off a (possibly keep-alive) socket: headers +
// Content-Length body. Returns "" on EOF/timeout.
static std::string readOneResponse(int s) {
    std::string in;
    size_t hdrEnd = std::string::npos;
    while (hdrEnd == std::string::npos) {
        char buf[4096];
        ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) return in.empty() ? std::string() : in;
        in.append(buf, (size_t)n);
        hdrEnd = in.find("\r\n\r\n");
    }
    // Parse Content-Length (case-insensitive).
    std::string lower = in.substr(0, hdrEnd);
    for (char& c : lower) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    size_t cl = 0;
    size_t cp = lower.find("content-length:");
    if (cp != std::string::npos) {
        size_t vs = cp + 15;
        while (vs < lower.size() && lower[vs] == ' ') vs++;
        size_t ve = lower.find("\r\n", vs);
        if (ve == std::string::npos) ve = lower.size();
        try { cl = (size_t)std::stoull(lower.substr(vs, ve - vs)); } catch (...) { cl = 0; }
    }
    size_t need = hdrEnd + 4 + cl;
    while (in.size() < need) {
        char buf[4096];
        ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        in.append(buf, (size_t)n);
    }
    return in;
}

static int statusOf(const std::string& resp) {
    if (resp.size() < 12 || resp.compare(0, 5, "HTTP/") != 0) return -1;
    size_t sp = resp.find(' ');
    if (sp == std::string::npos) return -1;
    return atoi(resp.c_str() + sp + 1);
}

// One-shot request over a fresh connection (Connection: close).
static std::string httpOnce(int port, const std::string& req) {
    int s = dial(port);
    if (s < 0) return "";
    sendAll(s, req);
    std::string out;
    char buf[4096];
    while (true) {
        ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    close(s);
    return out;
}

int main() {
    system("mkdir -p /tmp/aatpe_v94_state 2>/dev/null");
    aatpe::webui_http::ensureDaemonToken();

    printf("=== P3-webui: start HTTP server ===\n");
    if (!aatpe::webui_http::startHttpServer()) {
        printf("  [FAIL] startHttpServer returned false\n");
        return 1;
    }
    int port = aatpe::webui_http::getHttpPort();
    CHECK(port >= 8888 && port <= 8899, ("server bound on port " + std::to_string(port)).c_str());
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // ---- HTTP-07: Origin trailing whitespace still matches the allowlist ----
    printf("=== HTTP-07: Origin trailing whitespace trim ===\n");
    {
        // Sanity: clean trusted origin -> ACAO echoed.
        std::string r1 = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\n"
            "Origin: https://appassets.androidplatform.net\r\n"
            "Connection: close\r\n\r\n");
        CHECK(r1.find("Access-Control-Allow-Origin: https://appassets.androidplatform.net\r\n") != std::string::npos,
              "clean trusted origin -> ACAO echoed");

        // Trailing space on the SAME trusted origin: must still match after trim.
        std::string r2 = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\n"
            "Origin: https://appassets.androidplatform.net \r\n"
            "Connection: close\r\n\r\n");
        CHECK(r2.find("Access-Control-Allow-Origin: https://appassets.androidplatform.net\r\n") != std::string::npos,
              "trusted origin WITH trailing space -> ACAO still echoed (trimmed)");

        // Control: a junk origin (even trimmed) must NOT get ACAO. Guards against
        // the test passing because we echo everything.
        std::string r3 = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\n"
            "Origin: https://evil.example \r\n"
            "Connection: close\r\n\r\n");
        CHECK(r3.find("Access-Control-Allow-Origin:") == std::string::npos,
              "junk origin (even with trailing space) -> NO ACAO (allowlist not bypassed)");
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // ---- HTTP-06: body containing "connection: close" must not close keep-alive ----
    printf("=== HTTP-06: body 'connection: close' does not end keep-alive ===\n");
    {
        int s = dial(port);
        CHECK(s >= 0, "keep-alive socket dialed");
        if (s >= 0) {
            // GET /health with a body whose literal payload IS "connection: close".
            // Old code scanned the whole request -> found it in the body -> closed
            // the fd after the first response. Fixed code scans headers only.
            // Content-Length MUST equal the body byte count exactly, else the server
            // waits out its recv timeout (which itself desynchronizes the test).
            std::string body1 = "connection: close";  // 17 bytes
            std::string req1 =
                "GET /health HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                std::to_string(body1.size()) + "\r\n\r\n" + body1;
            sendAll(s, req1);
            std::string resp1 = readOneResponse(s);
            CHECK(statusOf(resp1) == 200,
                  ("first /health over keep-alive -> 200 (got " + std::to_string(statusOf(resp1)) + ")").c_str());

            // Second request on the SAME socket. If the server closed after the
            // first response, recv returns EOF ("") -> FAIL.
            std::string req2 = "GET /health HTTP/1.1\r\nHost: x\r\n\r\n";
            sendAll(s, req2);
            std::string resp2 = readOneResponse(s);
            CHECK(statusOf(resp2) == 200,
                  ("second /health over reused keep-alive socket -> 200 (got " +
                   std::to_string(statusOf(resp2)) + "; -1 = server closed, old bug)").c_str());
            close(s);
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // ---- HTTP-05: unsigned /control write (hmacRequired=false) -> 403 not 405 ----
    printf("=== HTTP-05: unsigned cross-origin /control write -> 403 (LED-158 gate) ===\n");
    {
        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.hmacRequired = false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Non-CSRF-safe content type, no token, no signature, whitelisted WRITE cmd.
        std::string body = "set_mode=active";
        std::string r = httpOnce(port,
            "POST /control HTTP/1.1\r\nHost: x\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body);
        int st = statusOf(r);
        CHECK(st == 403,
              ("unsigned /control write (hmacRequired=false) -> 403 (got " + std::to_string(st) + ")").c_str());
        CHECK(st != 405,
              ("unsigned /control write is NOT the 405 backstop firing (got " + std::to_string(st) + ")").c_str());
        { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.hmacRequired = true; }
    }

    aatpe::webui_http::stopHttpServer();

    printf("=== P3-webui summary: pass=%d fail=%d ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
