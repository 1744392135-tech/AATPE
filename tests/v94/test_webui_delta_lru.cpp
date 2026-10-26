// test_webui_delta_lru.cpp — HTTP-01 regression: delta baseline LRU eviction.
//
// Before the fix, g_deltaBaselines.erase(begin()) removed the lexicographically
// smallest cid (std::map key order), NOT the least-recently-used client. This
// test proves that after the table fills and a new cid arrives, the LRU client
// (not the lexicographically smallest) is evicted.
//
// Build: linked into the webui_http recipe.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "aatped.h"
#include "aatped_config.h"
#include "webui_http.h"

static int g_pass = 0, g_fail = 0;
static const char* asStr(const std::string& s) { return s.c_str(); }
static const char* asStr(const char* s) { return s; }
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", asStr(m)); } \
    else   { g_fail++; printf("  [FAIL] %s\n", asStr(m)); } \
} while(0)

static std::string httpOnce(int port, const std::string& req) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return "";
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) < 0) { close(s); return ""; }
    struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    size_t off = 0;
    const char* p = req.c_str();
    while (off < req.size()) {
        ssize_t n = send(s, p+off, req.size()-off, 0);
        if (n <= 0) break;
        off += (size_t)n;
    }
    std::string out;
    char buf[8192];
    while (true) {
        ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, n);
    }
    close(s);
    return out;
}

// Extract the body (after \r\n\r\n).
static std::string bodyOf(const std::string& resp) {
    size_t p = resp.find("\r\n\r\n");
    return (p == std::string::npos) ? "" : resp.substr(p + 4);
}

int main() {
    system("mkdir -p /tmp/aatpe_v94_state 2>/dev/null");
    system("echo 42 > /tmp/aatpe_v94_state/daemon_uptime");
    system("echo active > /tmp/aatpe_v94_state/control_mode");
    aatpe::webui_http::ensureDaemonToken();

    printf("=== HTTP-01: delta LRU eviction test ===\n");
    if (!aatpe::webui_http::startHttpServer()) {
        printf("  [FAIL] startHttpServer returned false\n");
        return 1;
    }
    int port = aatpe::webui_http::getHttpPort();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Read daemon token.
    std::string tok;
    {
        std::string r = httpOnce(port, "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        // We need the token; read it via the file (the test runs as root in CI,
        // or the stub STATE_DIR is readable).
        FILE* f = fopen("/tmp/aatpe_v94_state/daemon_token", "r");
        if (f) { char buf[256]; if (fgets(buf, sizeof(buf), f)) tok = buf; tok.erase(tok.find_last_not_of("\r\n")+1); fclose(f); }
    }
    CHECK(!tok.empty(), "daemon_token readable");

    auto deltaReq = [&](const std::string& cid) -> std::string {
        std::string path = "/state/all?delta=1&cid=" + cid;
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\nConnection: close\r\n\r\n";
        return bodyOf(httpOnce(port, req));
    };

    // Step 1: Prime cid "aaa" (first request → baseline established).
    std::string r1 = deltaReq("aaa");
    CHECK(r1.find("\"changed_keys\":[") != std::string::npos, "prime cid=aaa returns changed_keys array");

    // Step 2: Fill 31 more cids (total 32 = kMaxDeltaClients).
    // Use cids "n00".."n30" — lexicographically these are all AFTER "aaa" (since 'n' > 'a').
    for (int i = 0; i < 31; i++) {
        char cid[8]; snprintf(cid, sizeof(cid), "n%02d", i);
        deltaReq(cid);
    }

    // Step 3: Re-access "aaa" to make it the MOST recently used (MRU).
    std::string r2 = deltaReq("aaa");
    // Since nothing changed, changed_keys should be empty (baseline still alive).
    bool aaaHasBaseline = (r2.find("\"changed_keys\":[]") != std::string::npos);
    CHECK(aaaHasBaseline, "cid=aaa baseline survives (empty changed_keys after re-access)");

    // Step 4: Add a 33rd cid "zzz" → triggers eviction.
    // Under lexicographic eviction, "aaa" would survive (it's not the smallest —
    // "n00" is smaller than "aaa"? No: 'n' > 'a', so "aaa" < "n00". So "aaa" IS
    // the lexicographically smallest and WOULD be evicted by the old bug).
    // Under LRU eviction, "aaa" was just accessed (MRU) and should survive.
    deltaReq("zzz");

    // Step 5: Re-access "aaa". If LRU works, baseline survives → empty changed_keys.
    // If old lex bug: "aaa" was evicted → full resync (non-empty changed_keys).
    std::string r3 = deltaReq("aaa");
    bool aaaSurvived = (r3.find("\"changed_keys\":[]") != std::string::npos);
    CHECK(aaaSurvived, "LRU: MRU cid=aaa survives eviction (empty changed_keys, got: " +
          r3.substr(0, 80) + "...)");

    aatpe::webui_http::stopHttpServer();

    printf("\n=== HTTP-01 delta LRU results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
