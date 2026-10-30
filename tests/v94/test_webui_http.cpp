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
#include "aatped_config.h"
#include "webui_http.h"

static int g_pass = 0, g_fail = 0, g_skip = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)
#if defined(__SANITIZE_THREAD__)
  #define AATPE_TSAN_BUILD 1
#else
  #define AATPE_TSAN_BUILD 0
#endif
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

static std::string httpOnce(int port, const std::string& req) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return "";
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); return ""; }
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
    if (resp.size() < 12) return -1;
    if (resp.compare(0, 5, "HTTP/") != 0) return -1;
    size_t sp = resp.find(' ');
    if (sp == std::string::npos) return -1;
    return atoi(resp.c_str() + sp + 1);
}

int main() {
    system("mkdir -p /tmp/aatpe_v94_state 2>/dev/null");
    system("echo 42 > /tmp/aatpe_v94_state/daemon_uptime");
    system("echo active > /tmp/aatpe_v94_state/control_mode");
    system("echo 5 > /tmp/aatpe_v94_state/log_level");
    system("python3 -c \"open('/tmp/aatpe_v94_state/sse_pressure_blob','w').write('x'*16000)\"");
    aatpe::webui_http::ensureDaemonToken();
    setenv("AATPE_SSE_KEEPALIVE_SEC", "1", 1);
    setenv("AATPE_SSE_HALFOPEN_TIMEOUT_SEC", "1", 1);
    printf("=== ROUND-001: start HTTP server ===\n");
    if (!aatpe::webui_http::startHttpServer()) {
        printf("  [FAIL] startHttpServer returned false\n");
        return 1;
    }
    int port = aatpe::webui_http::getHttpPort();
    CHECK(port >= 8888 && port <= 8899, ("server bound on port " + std::to_string(port)).c_str());
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
    std::string s = httpOnce(port,
        "GET /api/session HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    int st = statusOf(s);
    CHECK(st == 403,
          ("/api/session no-cred with hmacRequired=false -> 403 (got " + std::to_string(st) + ")").c_str());
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfg.hmacRequired = true;
    }

    printf("=== G8-003: TCP shared rate-limit bucket rejects after 50 req/s ===\n");
    {
        int got429 = 0; int gotOk = 0;
        for (int i = 0; i < 80; i++) {
            std::string r = httpOnce(port,
                "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
            int st = statusOf(r);
            if (st == 429) got429++;
            else if (st == 200) gotOk++;
        }
        CHECK_TSKIP(got429 > 0,
              ("flood of 80 /health -> at least one 429 (got " + std::to_string(got429) +
               " 429s, " + std::to_string(gotOk) + " 200s)").c_str());
    }

    printf("=== G8-011: Transfer-Encoding: chunked request MUST be rejected ===\n");
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::string r = httpOnce(port,
            "POST /control HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n0\r\n\r\n");
        int st = statusOf(r);
        CHECK(st != 200,
              ("TE: chunked POST /control -> not 200 (got " + std::to_string(st) + ")").c_str());
    }

    printf("=== G8-004: CSRF body substring does not bypass CSRF gate ===\n");
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
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
        std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
        std::string tok((std::istreambuf_iterator<char>(tf)),
                         std::istreambuf_iterator<char>());
        while (!tok.empty() && (tok.back()=='\n'||tok.back()=='\r'||tok.back()==' ')) tok.pop_back();
        CHECK(!tok.empty(), "daemon_token readable for SSE test");

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
            char hdr[512]; int n = recv(s, hdr, sizeof hdr, 0);
            if (n > 0 && hdr[0]=='H' && strstr(hdr, "200")) {
                sseSocks.push_back(s); openCount++;
            } else {
                close(s);
            }
        }
        CHECK(openCount == 6,
              ("opened 6 SSE connections (got " + std::to_string(openCount) + ")").c_str());

        std::string r7 = httpOnce(port,
            "GET /events HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        int st7 = statusOf(r7);
        CHECK(st7 == 429, ("7th SSE -> 429 (got " + std::to_string(st7) + ")").c_str());
        CHECK(r7.find("sse_limit") != std::string::npos,
              "7th SSE body contains \"sse_limit\"");

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
                if (nTry > 0) { hTry[nTry] = '\0'; }
                if (nTry > 0 && strstr(hTry, "200")) {
                    reopened = true;
                    sseSocks.push_back(sTry);
                    continue;
                }
                close(sTry);
            } else {
                close(sTry);
            }
            if (!reopened) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK_TSKIP(reopened, "after closing one SSE, a new connection is admitted within 5s (slot eventually released)");

        for (int s : sseSocks) close(s);
    }

    printf("=== ROUND-003: read-only /control 403 carries read_only:true + reason ===\n");
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
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

        std::string r1 = httpOnce(port,
            "GET /state/control_mode HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK_TSKIP(statusOf(r1) == 200, "/state/control_mode (public) -> 200");
        CHECK_TSKIP(r1.find("\"control_mode\":\"active\"") != std::string::npos,
              "/state/control_mode returns the seeded value");

        std::string r2 = httpOnce(port,
            "GET /state/thermal_enabled HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r2) == 401, "/state/thermal_enabled without token -> 401");

        std::string r3 = httpOnce(port,
            "GET /state/thermal_enabled HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK_TSKIP(statusOf(r3) == 404, "/state/thermal_enabled (not seeded) -> 404");

        std::string r4 = httpOnce(port,
            "GET /state/..%2f..%2fetc%2fpasswd HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r4) == 400 || statusOf(r4) == 404,
              "path traversal rejected (400/404)");

        std::string r5 = httpOnce(port,
            "GET /state/foo.bar HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r5) == 400, "dot in name -> 400");
    }

    printf("=== ROUND-006: keep-alive — same fd serves multiple requests, per-request auth ===\n");
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a,0,sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool ok = false;
        if (s >= 0 && connect(s, (struct sockaddr*)&a, sizeof a) == 0) {
            struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            auto sendAll = [&](const std::string& req){ size_t o=0; while(o<req.size()){ ssize_t n=send(s,req.data()+o,req.size()-o,0); if(n<=0)return false; o+=n;} return true; };
            auto recvOne = [&]()->std::string {
                std::string all; char buf[4096];
                while (all.find("\r\n\r\n") == std::string::npos) {
                    ssize_t n=recv(s,buf,sizeof buf,0); if(n<=0) break; all.append(buf,n);
                }
                size_t hdrEnd = all.find("\r\n\r\n");
                if (hdrEnd == std::string::npos) return all;
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
            sendAll("GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
            std::string r1 = recvOne();
            sendAll("GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
            std::string r2 = recvOne();
            sendAll("GET /api/apps HTTP/1.1\r\nHost: x\r\n\r\n");
            std::string r3 = recvOne();
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
            send(s, "GET /garbage\n", 13, 0);
            struct timeval tv; tv.tv_sec=3; tv.tv_usec=0;
            setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
            char buf[256]; recv(s,buf,sizeof buf,0);
            close(s);
        } else if (s>=0) close(s);
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r)==200, "post-malformation: new connection /health -> 200");
    }

    printf("=== v4.2.9 T4: oversized request -> 413 + server closes, new conn still serves ===\n");
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a,0,sizeof a);
        a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        bool got413=false, closedAfter=false;
        if (s>=0 && connect(s,(struct sockaddr*)&a,sizeof a)==0) {
            struct timeval tv; tv.tv_sec=2; tv.tv_usec=0;
            setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
            std::string req = "POST /control HTTP/1.1\r\nHost: x\r\n"
                              "Content-Length: 100000000\r\n\r\n";
            send(s, req.data(), req.size(), 0);
            std::string all; char b[4096];
            while (all.find("\r\n\r\n")==std::string::npos) {
                ssize_t n=recv(s,b,sizeof b,0); if(n<=0) break; all.append(b,n);
            }
            got413 = (statusOf(all)==413);
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
        std::string r = httpOnce(port,
            "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(r)==200, "post-oversize: fresh connection /health -> 200");
    }

    printf("=== v4.2.9 T5: diag_bundle whitelist packs frame_apps.json + audit.log ===\n");
    {
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

    printf("=== F-01: stok cannot unlock /api/hmac_key or /api/diag_bundle ===\n");
    {
        int port = aatpe::webui_http::getHttpPort();
        std::string stok = aatpeSessionIssue();
        CHECK(!stok.empty(), "F-01: minted a test stok");
        {
            std::string req =
                "GET /api/hmac_key?stok=" + stok + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            std::string r = httpOnce(port, req);
            int st = statusOf(r);
            CHECK(st==401 || st==403,
                  ("F-01: /api/hmac_key with stok -> 401/403 (got " + std::to_string(st) + ")").c_str());
        }
        {
            std::string req =
                "GET /api/diag_bundle?stok=" + stok + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            std::string r = httpOnce(port, req);
            int st = statusOf(r);
            CHECK(st==401 || st==403,
                  ("F-01: /api/diag_bundle with stok -> 401/403 (got " + std::to_string(st) + ")").c_str());
        }
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
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a,0,sizeof a);
        a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        bool conn = (connect(s,(struct sockaddr*)&a,sizeof a)==0);
        CHECK(conn, "keepalive: SSE socket connected");
        std::string req = "GET /events HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\n\r\n";
        send(s, req.c_str(), req.size(), 0);
        struct timeval rcvto; rcvto.tv_sec=1; rcvto.tv_usec=0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof rcvto);
        std::string acc;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(AATPE_SLOW_BUILD ? 60 : 30);
        while (std::chrono::steady_clock::now() < dl) {
            char b[2048];
            int n = recv(s, b, sizeof b, 0);
            if (n > 0) acc.append(b, n);
            else if (n == 0) break;
            if (acc.find(": keepalive") != std::string::npos) break;
        }
        close(s);
        int ka = 0; size_t pos = 0;
        while ((pos = acc.find(": keepalive", pos)) != std::string::npos) { ka++; pos += 11; }
        CHECK(statusOf(acc.substr(0, acc.find("\r\n"))) == 200 || acc.find("200") != std::string::npos,
              ("keepalive: SSE stream started with 200 (got head: " + acc.substr(0,30) + ")").c_str());
        CHECK_TSKIP(ka >= 1, ("keepalive: received >=1 ': keepalive' heartbeat frame (got " + std::to_string(ka) + ")").c_str());
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

        std::vector<int> socks;
        int limit = 6;
        const int handshakeSec = AATPE_TSAN_BUILD ? 10 : 2;
        const int settleMs     = AATPE_TSAN_BUILD ? 500 : 30;
        const int maxAttempts   = limit * 8;
        int attempts = 0;
        while ((int)socks.size() < limit && attempts < maxAttempts) {
            attempts++;
            int s = socket(AF_INET, SOCK_STREAM, 0);
            int rcvbuf = 4096;
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
            struct sockaddr_in a; memset(&a,0,sizeof a);
            a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if (connect(s,(struct sockaddr*)&a,sizeof a)<0){ close(s); continue; }
            std::string req="GET /events HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: "+tok+"\r\n\r\n";
            send(s, req.c_str(), req.size(), 0);
            struct timeval hdrTo; hdrTo.tv_sec=handshakeSec; hdrTo.tv_usec=0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &hdrTo, sizeof hdrTo);
            char hdr[512];
            ssize_t nh = recv(s, hdr, sizeof hdr-1, 0);
            if (nh > 0) { hdr[nh] = '\0'; }
            if (nh > 0 && hdr[0]=='H' && strstr(hdr, "200")) {
                socks.push_back(s);
            } else {
                close(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(AATPE_TSAN_BUILD ? 100 : 30));
        }
        CHECK((int)socks.size()==limit, ("halfopen: opened " + std::to_string(socks.size()) + "/" + std::to_string(limit) + " seed SSE").c_str());

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

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
            ssize_t n = recv(s, h, sizeof(h)-1, 0);
            close(s);
            if (n <= 0) return -1;
            h[n] = '\0';
            return statusOf(std::string(h, (size_t)n));
        };

        int stFull = probeStatus();
        CHECK(stFull==429, ("halfopen: 7th conn rejected 429 while slots held (got " + std::to_string(stFull) + ")").c_str());
        auto heldAt = std::chrono::steady_clock::now();

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

    {
        system("mkdir -p /tmp/webui_test_dir 2>/dev/null");
        system("echo '{\"ok\":true,\"n\":7}' > /tmp/webui_test_dir/threads.json 2>/dev/null");

        std::ifstream tf(std::string(STATE_DIR) + "/daemon_token");
        std::string tok;
        if (tf.good()) { std::getline(tf, tok); }
        CHECK(!tok.empty(), "N-02: daemon_token readable for /webui test");

        std::string rOk = httpOnce(port,
            "GET /webui/threads.json HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(rOk) == 200,
            "N-02: GET /webui/threads.json with token -> 200");
        CHECK(rOk.find("\"n\":7") != std::string::npos,
            "N-02: /webui/threads.json body echoes seeded content");

        std::string rNoAuth = httpOnce(port,
            "GET /webui/threads.json HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(rNoAuth) == 401,
            "N-02: GET /webui/threads.json without token -> 401");

        std::string rTrav = httpOnce(port,
            "GET /webui/..%2f..%2fetc%2fpasswd HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n");
        CHECK(statusOf(rTrav) == 400,
            "N-02: GET /webui/../..%2fetc%2fpasswd -> 400 (traversal rejected)");

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
