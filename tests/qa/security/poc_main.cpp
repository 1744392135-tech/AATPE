#include "webui_http.h"
#include "aatped.h"
#include "aatped_hmac.h"
#include "aatpe_session_token.h"
#include "aatpe_command_dispatcher.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

extern Config g_cfg;
extern std::mutex g_cfgMutex;

static int g_port = 0;

static std::string http(const std::string& req) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return "SOCKET_ERR"; }
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return "CONNECT_ERR"; }
    struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::send(fd, req.data(), req.size(), 0);
    char buf[16384]; std::string out; ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) { out.append(buf, n); }
    close(fd);
    return out;
}

static std::string statusLine(const std::string& resp) {
    size_t sp = resp.find("\r\n");
    return sp == std::string::npos ? resp.substr(0,40) : resp.substr(0, sp);
}
static std::string bodyOf(const std::string& resp) {
    size_t h = resp.find("\r\n\r\n");
    return h == std::string::npos ? "" : resp.substr(h+4);
}

static int g_pass = 0, g_fail = 0;
static const size_t npos = std::string::npos;
static void check(const char* name, bool cond, const std::string& evidence) {
    if (cond) { g_pass++; printf("  [PASS] %s\n", name); }
    else      { g_fail++; printf("  [FAIL] %s\n", name); }
    printf("         -> %s\n", evidence.substr(0,200).c_str());
}

int main() {
    system("rm -rf /tmp/aatpe_poc_state /tmp/aatpe_poc_run /tmp/aatpe_poc_module");
    mkdir("/tmp/aatpe_poc_state", 0755);
    mkdir("/tmp/aatpe_poc_run", 0755);
    mkdir("/tmp/aatpe_poc_module", 0755);

    {
        std::lock_guard<std::mutex> lk(g_hmacMutex);
        memset(g_hmacKey, 0, sizeof(g_hmacKey));
        for (int i = 0; i < 127; i++) g_hmacKey[i] = "0123456789abcdef"[i % 16];
        g_hmacKey[127] = 0;
        g_hmacActive = true;
        g_hmacReadonly = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfg.hmacRequired = true;
    }

    if (!aatpe::webui_http::startHttpServer()) {
        printf("FATAL: server did not start\n"); return 2;
    }
    g_port = aatpe::webui_http::getHttpPort();
    printf("== AATPE WebUI PoC: server on 127.0.0.1:%d ==\n\n", g_port);

    std::string daemonToken;
    {
        int fd = open("/tmp/aatpe_poc_state/daemon_token", O_RDONLY);
        if (fd >= 0) { char b[256]; ssize_t n = read(fd, b, sizeof(b)-1); close(fd);
            if (n>0) { b[n]=0; daemonToken = b; } }
    }
    while (!daemonToken.empty() && (daemonToken.back()=='\n'||daemonToken.back()=='\r'||daemonToken.back()==' ')) daemonToken.pop_back();
    printf("daemon_token minted length=%zu (first8=%.8s...)\n\n", daemonToken.size(), daemonToken.c_str());

    std::string r;
    r = http("GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
    check("OPEN GET /health (expect 200)", statusLine(r).find("200")!=npos, statusLine(r)+" | "+bodyOf(r));

    r = http("GET /version HTTP/1.1\r\nHost: x\r\n\r\n");
    check("OPEN GET /version (expect 200)", statusLine(r).find("200")!=npos, statusLine(r)+" | "+bodyOf(r));

    r = http("GET /state/public HTTP/1.1\r\nHost: x\r\n\r\n");
    check("OPEN GET /state/public (expect 200, no token)", statusLine(r).find("200")!=npos, statusLine(r)+" | "+bodyOf(r));

    r = http("GET /api/fusion HTTP/1.1\r\nHost: x\r\n\r\n");
    check("GET /api/fusion NO token -> 401 (token-gated, not open)", statusLine(r).find("401")!=npos, statusLine(r)+" | "+bodyOf(r));

    r = http("GET /api/frame_time HTTP/1.1\r\nHost: x\r\n\r\n");
    check("GET /api/frame_time NO token -> 401 (token-gated, not open)", statusLine(r).find("401")!=npos, statusLine(r)+" | "+bodyOf(r));

    r = http("GET /state/all HTTP/1.1\r\nHost: x\r\n\r\n");
    check("PROTECTED GET /state/all NO token -> 401", statusLine(r).find("401")!=npos, statusLine(r));

    r = http("GET /state/all?token=wrongguess HTTP/1.1\r\nHost: x\r\n\r\n");
    check("PROTECTED GET /state/all ?token=wrong -> 401 (?token= dead on TCP)", statusLine(r).find("401")!=npos, statusLine(r));

    r = http("GET /state/all HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: wrongguess\r\n\r\n");
    check("PROTECTED GET /state/all bad X-Daemon-Token -> 401", statusLine(r).find("401")!=npos, statusLine(r));

    {
        std::string req = "GET /state/all HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + daemonToken + "\r\n\r\n";
        r = http(req);
    }
    check("PROTECTED GET /state/all correct X-Daemon-Token -> 200", statusLine(r).find("200")!=npos, statusLine(r)+" | "+bodyOf(r).substr(0,80));

    r = http("GET /api/daemon_token HTTP/1.1\r\nHost: x\r\n\r\n");
    check("GET /api/daemon_token over TCP -> 403 (not leaked to local apps)", statusLine(r).find("403")!=npos, statusLine(r)+" | "+bodyOf(r));

    r = http("POST /control HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\nContent-Length: 25\r\n\r\nset_profile=gaming");
    check("CSRF cross-origin text/plain POST /control unsigned -> blocked (403/405)",
          statusLine(r).find("403")!=npos || statusLine(r).find("405")!=npos, statusLine(r)+" | "+bodyOf(r));

    r = http("GET /control?cmd=set_profile=gaming HTTP/1.1\r\nHost: x\r\n\r\n");
    check("CSRF cross-origin GET /control?cmd=... unsigned -> blocked (403/405)",
          statusLine(r).find("403")!=npos || statusLine(r).find("405")!=npos, statusLine(r)+" | "+bodyOf(r));

    r = http("POST /control HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 19\r\n\r\nset_profile=gaming");
    check("POST application/json unsigned (no sig) -> 403 (HMAC still required)",
          statusLine(r).find("403")!=npos, statusLine(r)+" | "+bodyOf(r));

    r = http("POST /control HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 60\r\n\r\nset_profile=gaming.ts=9999999999.sig=deadbeefdeadbeef");
    check("POST /control with FORGED .sig -> 403", statusLine(r).find("403")!=npos, statusLine(r)+" | "+bodyOf(r));

    {
        std::string actualCmd = "set_profile=gaming";
        int64_t ts = (int64_t)time(nullptr);
        std::string nonce = "a1b2c3d4e5f60718";
        std::string signedData = actualCmd + ".ts=" + std::to_string(ts) + ".nonce=" + nonce;
        std::string sig = computeCommandHmac(signedData.c_str());
        std::string body = actualCmd + ".nonce=" + nonce + ".ts=" + std::to_string(ts) + ".sig=" + sig;
        std::string req = "POST /control HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: "
                        + std::to_string(body.size()) + "\r\n\r\n" + body;
        r = http(req);
    }
    check("POST /control with VALID HMAC+nonce -> 200 queued", statusLine(r).find("200")!=npos, statusLine(r)+" | "+bodyOf(r).substr(0,120));

    {
        std::string actualCmd = "set_mode=passive";
        int64_t ts = (int64_t)time(nullptr);
        std::string nonce = "0badc0de0badc0de";
        std::string signedData = actualCmd + ".ts=" + std::to_string(ts) + ".nonce=" + nonce;
        std::string sig = computeCommandHmac(signedData.c_str());
        std::string body = actualCmd + ".nonce=" + nonce + ".ts=" + std::to_string(ts) + ".sig=" + sig;
        std::string req = "POST /control HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: "
                        + std::to_string(body.size()) + "\r\n\r\n" + body;
        http(req);
        r = http(req);
    }
    check("REPLAY verbatim (cmd,nonce,ts,sig) -> 403 (nonce replay cache)", statusLine(r).find("403")!=npos,
          statusLine(r)+" | "+bodyOf(r).substr(0,120));

    r = http("GET /api/command/..%2f..%2f..%2fetc%2fpasswd HTTP/1.1\r\nHost: x\r\n\r\n");
    check("PATH traversal /api/command/<traversal> -> 400/401 (not file read)",
          statusLine(r).find("400")!=npos || statusLine(r).find("401")!=npos, statusLine(r));

    {
        std::string big(70000, 'a');
        std::string req = "POST /control HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: "
                        + std::to_string(big.size()) + "\r\n\r\n" + big;
        r = http(req);
    }
    check("OVERSIZE body (>64KB) -> 413", statusLine(r).find("413")!=npos, statusLine(r));

    r = http("DELETE /health HTTP/1.1\r\nHost: x\r\n\r\n");
    check("DELETE /health (non-GET) -> 405", statusLine(r).find("405")!=npos, statusLine(r));

    r = http("GET /api/hmac_key HTTP/1.1\r\nHost: x\r\n\r\n");
    check("GET /api/hmac_key NO token -> 401 (HMAC key not leaked)", statusLine(r).find("401")!=npos, statusLine(r));
    {
        std::string req = "GET /api/hmac_key HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + daemonToken + "\r\n\r\n";
        r = http(req);
    }
    check("GET /api/hmac_key WITH token -> 200 returns key", statusLine(r).find("200")!=npos && bodyOf(r).find("hmac_key")!=npos, statusLine(r)+" | "+bodyOf(r).substr(0,80));

    printf("\n== PoC summary: %d passed, %d failed ==\n", g_pass, g_fail);
    aatpe::webui_http::stopHttpServer();
    return g_fail > 0 ? 1 : 0;
}
