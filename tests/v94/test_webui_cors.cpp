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

#include "webui_http.h"
#include "aatpe_session_token.h"
#include "integrity.h"
#include "aatped.h"
#include "aatped_config.h"

extern const char* STATE_DIR;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static std::string httpOnce(int port, const std::string& req) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return "";
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); return ""; }
    struct timeval tv; tv.tv_sec = 15; tv.tv_usec = 0;
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
static std::string headerVal(const std::string& resp, const char* name) {
    std::string lowerName; for (const char* c = name; *c; ++c) lowerName += (char)((*c>='A'&&*c<='Z')?*c+32:*c);
    size_t pos = 0;
    while (pos < resp.size()) {
        size_t eol = resp.find("\r\n", pos);
        if (eol == std::string::npos) break;
        std::string line = resp.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string h = line.substr(0, colon);
            for (auto& ch : h) ch = (ch>='A'&&ch<='Z')?ch+32:ch;
            if (h == lowerName) {
                std::string v = line.substr(colon+1);
                while (!v.empty() && (v.front()==' '||v.front()=='\t')) v.erase(v.begin());
                while (!v.empty() && (v.back()==' '||v.back()=='\t'||v.back()=='\r'||v.back()=='\n')) v.pop_back();
                return v;
            }
        }
        pos = eol + 2;
    }
    return "";
}
static bool hasHeader(const std::string& resp, const char* name) {
    return !headerVal(resp, name).empty();
}
static int countCorsHeaders(const std::string& resp) {
    int n = 0;
    const char* names[] = {"Access-Control-Allow-Origin","Access-Control-Allow-Methods",
                           "Access-Control-Allow-Headers","Access-Control-Max-Age",
                           "Access-Control-Allow-Credentials"};
    for (auto nm : names) if (hasHeader(resp, nm)) ++n;
    return n;
}
static std::string readDaemonToken() {
    std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
    std::string tok((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
    while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
    return tok;
}

int main() {
    system("mkdir -p /tmp/aatpe_v94_state 2>/dev/null");
    aatpe::webui_http::ensureDaemonToken();
    if (!aatpe::webui_http::startHttpServer()) { printf("  [FAIL] startHttpServer\n"); return 1; }
    int port = aatpe::webui_http::getHttpPort();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::string tok = readDaemonToken();
    CHECK(!tok.empty(), "daemon_token readable");

    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.hmacRequired = true; }
    { extern bool g_hmacReadonlyFlag; g_hmacReadonlyFlag = false; }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    printf("=== C: whitelisted manager origin gets exact ACAO echo ===\n");
    {
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nOrigin: https://appassets.androidplatform.net\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r) == 200, "[C1] /health 200 with manager origin");
        CHECK(headerVal(r,"Access-Control-Allow-Origin") == "https://appassets.androidplatform.net",
              "[C1] ACAO echoes manager origin byte-for-byte");
        CHECK(hasHeader(r,"Vary"), "[C1] Vary: Origin present");
        CHECK(headerVal(r,"Access-Control-Allow-Origin") != "*",
              "[C1] no bare ACAO:*");
    }

    printf("=== C: real-device origins (kernelsu.org / null) now served ===\n");
    {
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nOrigin: https://kernelsu.org\r\nConnection: close\r\n\r\n");
        CHECK(headerVal(r,"Access-Control-Allow-Origin") == "https://kernelsu.org",
              ("[C2] ACAO echoes https://kernelsu.org (got: '" + headerVal(r,"Access-Control-Allow-Origin") + "')").c_str());
        CHECK(hasHeader(r,"Vary"), "[C2] Vary present for kernelsu.org");
    }
    {
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nOrigin: null\r\nConnection: close\r\n\r\n");
        CHECK(headerVal(r,"Access-Control-Allow-Origin") == "null",
              ("[C3] ACAO:null served for sandboxed webview (got: '" + headerVal(r,"Access-Control-Allow-Origin") + "')").c_str());
        CHECK(hasHeader(r,"Vary"), "[C3] Vary present for null origin");
        CHECK(!hasHeader(r,"Access-Control-Allow-Credentials"),
              "[C3] null origin does NOT get Access-Control-Allow-Credentials:true");
    }
    {
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nOrigin: https://appassets.androidplatform.net\r\nConnection: close\r\n\r\n");
        CHECK(hasHeader(r,"Access-Control-Allow-Credentials"),
              "[C3b] explicit manager origin keeps Access-Control-Allow-Credentials:true");
    }

    printf("=== C: NON-whitelist origin gets NO CORS headers ===\n");
    {
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nOrigin: https://evil.example.com\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r) == 200, "[C4] /health still 200 for evil origin (liveness)");
        CHECK(countCorsHeaders(r) == 0,
              "[C4] evil origin response has zero Access-Control-Allow-* headers");
        CHECK(!hasHeader(r,"Access-Control-Allow-Origin"), "[C4] no ACAO for evil origin");
    }
    {
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(countCorsHeaders(r) == 0, "[C5] request with NO Origin gets no CORS headers");
    }

    printf("=== C: OPTIONS preflight returns Methods/Headers/Max-Age ===\n");
    {
        std::string r = httpOnce(port,
            "OPTIONS /health HTTP/1.1\r\nHost: x\r\nOrigin: https://appassets.androidplatform.net\r\n"
            "Access-Control-Request-Method: GET\r\n"
            "Access-Control-Request-Headers: x-daemon-token,content-type\r\n"
            "Connection: close\r\n\r\n");
        CHECK(statusOf(r) == 200, "[C6] OPTIONS preflight 200");
        CHECK(headerVal(r,"Access-Control-Allow-Origin") == "https://appassets.androidplatform.net",
              "[C6] preflight ACAO echoes origin");
        CHECK(headerVal(r,"Access-Control-Allow-Methods").find("GET") != std::string::npos &&
              headerVal(r,"Access-Control-Allow-Methods").find("POST") != std::string::npos &&
              headerVal(r,"Access-Control-Allow-Methods").find("OPTIONS") != std::string::npos,
              "[C6] Allow-Methods = GET, POST, OPTIONS");
        {
            std::string ah = headerVal(r,"Access-Control-Allow-Headers");
            std::string ahl = ah; for (auto& c: ahl) c=(c>='A'&&c<='Z')?c+32:c;
            CHECK(ahl.find("x-daemon-token") != std::string::npos &&
                  ahl.find("content-type") != std::string::npos,
                  "[C6] Allow-Headers contains X-Daemon-Token + Content-Type");
        }
        CHECK(hasHeader(r,"Access-Control-Max-Age"), "[C6] Max-Age present");
    }
    {
        std::string r = httpOnce(port,
            "OPTIONS /health HTTP/1.1\r\nHost: x\r\nOrigin: https://evil.example.com\r\nConnection: close\r\n\r\n");
        CHECK(countCorsHeaders(r) == 0, "[C7] preflight from non-whitelist origin has no CORS headers");
    }
    {
        std::string r = httpOnce(port,
            "OPTIONS /state/all HTTP/1.1\r\nHost: x\r\nOrigin: null\r\n"
            "Access-Control-Request-Headers: x-daemon-token\r\nConnection: close\r\n\r\n");
        CHECK(headerVal(r,"Access-Control-Allow-Origin") == "null",
              "[C7b] preflight ACAO:null served (webview custom-header path)");
    }

    printf("=== C: token-protected API unaffected; CSP not relaxed ===\n");
    {
        std::string r = httpOnce(port,
            "GET /state/all HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok +
            "\r\nConnection: close\r\n\r\n");
        int stC8 = statusOf(r);
        CHECK(stC8 == 200, ("[C8] /state/all with daemon token -> 200 (got " + std::to_string(stC8) + ")").c_str());
    }
    {
        std::string r = httpOnce(port,
            "GET /state/all HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r) == 401, "[C8b] /state/all WITHOUT token -> 401 (fail-closed unchanged)");
    }
    {
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nOrigin: https://appassets.androidplatform.net\r\nConnection: close\r\n\r\n");
        CHECK(headerVal(r,"Content-Security-Policy").find("default-src 'none'") != std::string::npos,
              "[C9] CSP unchanged (default-src 'none')");
        CHECK(headerVal(r,"X-Frame-Options") == "DENY", "[C9] X-Frame-Options: DENY unchanged");
        CHECK(headerVal(r,"X-Content-Type-Options") == "nosniff", "[C9] nosniff unchanged");
    }

    printf("=== C: null-origin unsigned /control write still rejected (no CSRF widening) ===\n");
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        std::string r = httpOnce(port,
            "POST /control HTTP/1.1\r\nHost: x\r\nOrigin: null\r\nContent-Type: text/plain\r\n"
            "Connection: close\r\n\r\nset_mode=active");
        int st = statusOf(r);
        CHECK(st == 403, ("[C10] null-origin unsigned POST /control -> 403 (got " + std::to_string(st) + ")").c_str());
    }
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        std::string r = httpOnce(port,
            "GET /control?cmd=set_mode=active HTTP/1.1\r\nHost: x\r\nOrigin: https://appassets.androidplatform.net\r\nConnection: close\r\n\r\n");
        int st = statusOf(r);
        CHECK(st == 403, ("[C11] manager-origin unsigned GET /control -> 403 (got " + std::to_string(st) + ")").c_str());
    }

    aatpe::webui_http::stopHttpServer();
    printf("\n========================================\n"
           "CORS results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
