#include "webui_http.h"
#include "aatped.h"
#include "aatpe_session_token.h"
#include "aatpe_command_queue.h"
#include "aatpe_command_dispatcher.h"
#include "integrity.h"
#include "aatpe_config_schema.h"
#include "aatped_apklabel.h"

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

extern char g_hmacKey[128];
extern bool g_hmacActive;
extern std::atomic<int> g_sseClientCount;

std::string aatpe_buildFrameAppsJson();

namespace {

std::atomic<bool> g_httpRunning{false};
std::atomic<int> g_httpPort{0};
int g_listenFd = -1;
std::thread g_httpThread;
auto g_serverStart = std::chrono::steady_clock::now();
thread_local std::string g_currentOrigin;

thread_local bool g_peerIsLoopback = false;

thread_local int g_peerUid = -1;

thread_local int g_rateBucketKey = -1;

static constexpr int kSseMaxTotal = 16;
static constexpr int kSseMaxPerUid = 3;
static constexpr int kSseMaxPerSharedTcp = 6;
std::atomic<int> g_sseTotal{0};

struct SseAdmission {
    std::mutex mu;
    std::map<int,int> byUid;
};
static SseAdmission& sseAdmission() {
    static SseAdmission* s = new SseAdmission();
    return *s;
}

struct SseSlotGuard {
    int uid = 0;
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

static bool peerUidIsTrusted(int uid) {
    return uid == 0 || uid == 1000;
}

static const size_t kMaxRequestBody = 65536;
static const int kRecvTimeoutSec = 2;
static int sendTimeoutSec() {
    static const int kDefault = 5;
    const char* env = getenv("AATPE_SSE_HALFOPEN_TIMEOUT_SEC");
    if (!env) return kDefault;
    int v = atoi(env);
    return v < 1 ? kDefault : v;
}

static const size_t kMaxConnections = 32;
struct ConnEntry {
    std::thread th;
    std::atomic<int> clientFd{-1};
    std::atomic<bool> done{false};
};
std::mutex g_connMutex;
std::list<std::shared_ptr<ConnEntry>> g_connThreads;

std::atomic<long long> g_lastClientAcceptMs{0};
static const long long kClientGraceMs = 60000;

std::string readFileToString(const std::string& path) {
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

static bool sendAll(int fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    size_t off = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (off < len) {
        ssize_t w = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (w > 0) { off += (size_t)w; t0 = std::chrono::steady_clock::now(); continue; }
        if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (errno == EINTR) continue;
            auto now = std::chrono::steady_clock::now();
            auto stalled = std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();
            if (stalled >= sendTimeoutSec()) return false;
            struct timespec ts = {0, 1000};
            nanosleep(&ts, nullptr);
            continue;
        }
        return false;
    }
    return true;
}

bool constantTimeStringEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

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
            if (i + 2 < s.size()) {
                int hi = hexVal(s[i + 1]);
                int lo = hexVal(s[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out += (char)(hi * 16 + lo);
                    i += 2;
                    continue;
                }
            }
            out += c;
        } else {
            out += c;
        }
    }
    return out;
}

bool isCommandAllowed(const std::string& rawCmd);

static bool checkDaemonTokenAuth(const std::string& request, const std::string& path);
static bool checkSseTicketAuth(const std::string& request, const std::string& path);

bool isReadOnlyControlCommand(const std::string& rawCmd) {
    std::string cmd = rawCmd;
    while (!cmd.empty() && (cmd.back()=='\n'||cmd.back()=='\r'||cmd.back()==' '||cmd.back()=='\t')) cmd.pop_back();
    while (!cmd.empty() && (cmd.front()==' '||cmd.front()=='\t')) cmd.erase(cmd.begin());
    size_t eq = cmd.find('=');
    std::string key = (eq == std::string::npos) ? cmd : cmd.substr(0, eq);
    if (key == "get_status" || key == "state" || key == "get_state") return true;
    return false;
}

static const size_t kRateLimitPerSec = 100;
static const size_t kRateLimitPerSecTcpShared = 50;
static const int kSharedTcpBucketKey = -1;
std::mutex g_rateMutex;
std::map<int, std::list<std::chrono::steady_clock::time_point>> g_rateWindowByUid;

bool rateLimitAllowed() {
    auto now = std::chrono::steady_clock::now();
    int uid = g_rateBucketKey;
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

struct NonceEntry { std::string key; int64_t seenTs; };
std::mutex g_nonceMutex;
std::deque<NonceEntry> g_nonceCache;
static std::unordered_set<std::string> g_nonceSet;
static const size_t kNonceCacheMax = 32768;
bool nonceReplayOrInsert(const std::string& nonceKey, int64_t reqTs) {
    (void)reqTs;
    std::lock_guard<std::mutex> lk(g_nonceMutex);
    int64_t now = (int64_t)time(nullptr);
    while (!g_nonceCache.empty() && (now - g_nonceCache.front().seenTs) > 600) {
        g_nonceSet.erase(g_nonceCache.front().key);
        g_nonceCache.pop_front();
    }
    if (g_nonceSet.count(nonceKey)) return true;
    g_nonceSet.insert(nonceKey);
    g_nonceCache.push_back({nonceKey, now});
    while (g_nonceCache.size() > kNonceCacheMax) {
        g_nonceSet.erase(g_nonceCache.front().key);
        g_nonceCache.pop_front();
    }
    return false;
}

std::mutex g_auditMutex;
static const off_t kAuditMaxBytes = off_t(2) * 1024 * 1024;

void auditCommand(const std::string& actualCmd, int peerUid, bool ok,
                  const std::string& source, const std::string& reason) {
    std::lock_guard<std::mutex> lk(g_auditMutex);
    std::string path = std::string(STATE_DIR) + "/audit.log";
    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && st.st_size >= kAuditMaxBytes) {
        std::string oldPath = path + ".1";
        if (::rename(path.c_str(), oldPath.c_str()) != 0) {
            LOG_WARN("audit: rotate rename(%s -> %s) failed: %s",
                     path.c_str(), oldPath.c_str(), aatpeErrStr(errno));
        }
    }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    char tsbuf[32];
    time_t now = time(nullptr);
    struct tm tm_{};
    localtime_r(&now, &tm_);
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S", &tm_);
    std::string src = source.empty() ? "webui" : source;
    std::string rs = reason.empty() ? (ok ? "applied" : "rejected") : reason;
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
    {
        size_t off = 0, len = line.size();
        while (off < len) {
            ssize_t w = write(fd, line.c_str() + off, len - off);
            if (w < 0) { if (errno == EINTR) continue; break; }
            if (w == 0) break;
            off += (size_t)w;
        }
    }
    close(fd);
}

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

static bool isTrustedManagerOrigin(const std::string& origin) {
    static const char* kManagerOrigins[] = {
        "https://appassets.androidplatform.net",
        "https://kernelsu.org",
        "https://www.kernelsu.org",
        "https://ps.kernelui.github.io",
        "https://topjohnwu.github.io",
        "https://127.0.0.1",
        "https://localhost"
    };
    for (const char* o : kManagerOrigins) {
        if (origin == o) return true;
    }
    return false;
}

static bool isCsrfTrustedOrigin(const std::string& origin) {
    if (isTrustedManagerOrigin(origin)) return true;
    if (loopbackHttpOrigin(origin)) return true;
    return false;
}

static bool isCorsOriginAllowed(const std::string& origin) {
    if (origin.empty()) return false;
    if (isTrustedManagerOrigin(origin)) return true;
    if (loopbackHttpOrigin(origin)) return true;
    if (origin == "null") return true;
    return false;
}

static void appendCorsHeaders(std::ostringstream& resp, const std::string& origin) {
    if (origin.empty() || !isCorsOriginAllowed(origin)) return;
    resp << "Access-Control-Allow-Origin: " << origin << "\r\n";
    resp << "Vary: Origin\r\n";
    resp << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    resp << "Access-Control-Allow-Headers: Content-Type, X-Daemon-Token\r\n";
    resp << "Access-Control-Max-Age: 86400\r\n";
    if (origin != "null") {
        resp << "Access-Control-Allow-Credentials: true\r\n";
    }
}

static bool isCsrfSafePost(const std::string& request) {
    size_t headerEnd = request.find("\r\n\r\n");
    std::string headers = (headerEnd == std::string::npos) ? request
                                                           : request.substr(0, headerEnd);
    {
        std::string lower = headers;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
        if (lower.find("x-daemon-token:") != std::string::npos) return true;
    }
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
    size_t ct = lower.find("content-type:");
    if (ct != std::string::npos) {
        size_t eol = lower.find("\r\n", ct);
        std::string ctLine = lower.substr(ct, (eol==std::string::npos)?eol:eol-ct);
        if (ctLine.find("application/json") != std::string::npos) return true;
    }
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
    resp << "Connection: keep-alive\r\n";
    resp << "Server: AATPE-WebUI/1.0\r\n";
    resp << "X-Content-Type-Options: nosniff\r\n";
    resp << "X-Frame-Options: DENY\r\n";
    resp << "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'; base-uri 'none'\r\n";
    appendCorsHeaders(resp, origin);
    resp << "\r\n";
    resp << body;
    return resp.str();
}

std::string handleHealth() {
    auto now = std::chrono::steady_clock::now();
    long long uptimeSec = std::chrono::duration_cast<std::chrono::seconds>(
        now - g_serverStart).count();
    std::ostringstream body;
    body << "{\"status\":\"ok\",\"uptime_s\":" << uptimeSec
         << ",\"port\":" << g_httpPort.load() << "}";
    return buildHttpResponse(200, "application/json", body.str(), g_currentOrigin);
}

static std::mutex g_deltaMutex;
struct DeltaBaseline {
    std::map<std::string,std::string> state;
    long long lastAccessMs = 0;
};
static std::map<std::string, DeltaBaseline> g_deltaBaselines;
static const size_t kMaxDeltaClients = 32;

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

std::string handleStateAll(const std::string& query) {
    std::string deltaVal;
    bool wantDelta = queryParamEq(query, "delta", deltaVal) && (deltaVal == "1");
    std::string cid;
    queryParamEq(query, "cid", cid);
    if (cid.size() > 64) cid.resize(64);
    if (cid.empty()) cid = "anon";

    if (!stateCacheIsSeeded()) stateCacheSeedFromDir(STATE_DIR);
    std::vector<std::pair<std::string,std::string>> snap = stateCacheSnapshot();
    uint64_t gen = stateCacheGeneration();

    if (!wantDelta) {
        std::string body = aatpe::webui_http::buildAggregateStateJson();
        return buildHttpResponse(200, "application/json", body, g_currentOrigin);
    }

    std::map<std::string,std::string> cur(snap.begin(), snap.end());
    std::lock_guard<std::mutex> lk(g_deltaMutex);
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_deltaBaselines.size() >= kMaxDeltaClients &&
        g_deltaBaselines.find(cid) == g_deltaBaselines.end()) {
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
    for (const auto& kv : cur) {
        auto bit = baseline.state.find(kv.first);
        bool changed = (bit == baseline.state.end()) || (bit->second != kv.second);
        if (!changed) continue;
        if (!first) body << ",";
        first = false;
        body << "\"" << jsonEscape(kv.first) << "\"";
    }
    body << "],";
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
    baseline.state = std::move(cur);
    return buildHttpResponse(200, "application/json", body.str(), g_currentOrigin);
}

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
    "fg_package",
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

std::string handleControl(const std::string& cmd, bool viaGet = false, bool csrfSafe = false, const std::string* requestForAuth = nullptr) {
    (void)viaGet;
    std::string command = cmd;
    while (!command.empty() && (command.back()=='\n'||command.back()=='\r'||command.back()==' '||command.back()=='\t')) command.pop_back();
    while (!command.empty() && (command.front()==' '||command.front()=='\t')) command.erase(command.begin());
    if (command.empty()) return buildHttpResponse(400,"application/json","{\"ok\":false,\"error\":\"empty command\"}",g_currentOrigin);
    if (command.size() > 512) {
        return buildHttpResponse(400,"application/json","{\"ok\":false,\"error\":\"command too long\"}",g_currentOrigin);
    }

    std::string actualCmd = command;
    std::string sig;
    int64_t reqTs = 0;
    bool hasSig = false;
    bool hasTs = false;
    size_t sigPos = command.rfind(".sig=");
    if (sigPos != std::string::npos && sigPos > 0) {
        sig = command.substr(sigPos + 5);
        actualCmd = command.substr(0, sigPos);
        hasSig = true;
    }
    size_t tsPos = actualCmd.rfind(".ts=");
    if (tsPos != std::string::npos && tsPos > 0) {
        std::string tsStr = actualCmd.substr(tsPos + 4);
        try { reqTs = std::stoll(tsStr); hasTs = true; }
        catch (...) { hasTs = false; LOG_DEBUG("http: bad .ts= '%s', ignoring", tsStr.c_str()); }
        actualCmd = actualCmd.substr(0, tsPos);
    }

    std::string nonce;
    size_t noncePos = actualCmd.rfind(".nonce=");
    if (noncePos != std::string::npos && noncePos > 0) {
        nonce = actualCmd.substr(noncePos + 7);
        actualCmd = actualCmd.substr(0, noncePos);
    }

    bool hmacRequired;
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        hmacRequired = g_cfg.hmacRequired;
    }

    if (isHmacReadonly() || !aatpe::integrity::privilegedOpsAllowed()) {
        const char* reason = !aatpe::integrity::privilegedOpsAllowed()
            ? "integrity_failed" : "hmac_unavailable";
        std::string body = std::string("{\"ok\":false,\"read_only\":true,\"reason\":\"") + reason +
            "\",\"error\":\"daemon in read-only mode (" + reason + ")\"}";
        return buildHttpResponse(403, "application/json", body, g_currentOrigin);
    }

    if (!isCommandAllowed(actualCmd)) {
        return buildHttpResponse(400,"application/json",
            "{\"ok\":false,\"error\":\"command not in whitelist\"}",g_currentOrigin);
    }

    bool sessionAuthenticated = false;
    bool sigAuthenticated = false;
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
        if (nonce.empty()) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"request nonce (.nonce=<hex>) required for replay protection\"}",g_currentOrigin);
        }
        const int64_t HMAC_REPLAY_WINDOW_SEC = 300;
        int64_t now = (int64_t)time(nullptr);
        int64_t age = now - reqTs;
        if (age > HMAC_REPLAY_WINDOW_SEC || age < -HMAC_REPLAY_WINDOW_SEC) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"timestamp expired or out of window (replay protection)\"}",g_currentOrigin);
        }
        std::string signedData = actualCmd + ".ts=" + std::to_string(reqTs);
        if (!nonce.empty()) signedData += ".nonce=" + nonce;
        if (!verifyCommandHmac(signedData.c_str(), sig.c_str())) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"HMAC signature verification failed\"}",g_currentOrigin);
        }
        sigAuthenticated = true;
        if (!nonce.empty() && nonceReplayOrInsert(signedData, reqTs)) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"replay detected (nonce already used)\"}",g_currentOrigin);
        }
    } else if (hasSig && hasTs) {
        std::string signedData = actualCmd + ".ts=" + std::to_string(reqTs);
        if (!nonce.empty()) signedData += ".nonce=" + nonce;
        if (!verifyCommandHmac(signedData.c_str(), sig.c_str())) {
            auditCommand(actualCmd, g_peerUid, false, "webui",
                         "hmac_required=false but presented signature failed verification");
            return buildHttpResponse(403, "application/json",
                "{\"ok\":false,\"error\":\"presented signature failed verification\"}",
                g_currentOrigin);
        }
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

    if (!hmacRequired && !sessionAuthenticated && !sigAuthenticated &&
        !isReadOnlyControlCommand(actualCmd)) {
        return buildHttpResponse(403, "application/json",
            "{\"ok\":false,\"error\":\"hmac_required=false: state-changing /control requires a verified signature or session token (open control plane disallowed)\"}",
            g_currentOrigin);
    }

    if (!csrfSafe && !sessionAuthenticated && !sigAuthenticated &&
        !isReadOnlyControlCommand(actualCmd)) {
        return buildHttpResponse(405, "application/json",
            "{\"ok\":false,\"error\":\"state-changing command requires a CSRF-safe request (valid HMAC signature / session token, or application/json / X-Daemon-Token / allowlisted Origin)\"}",
            g_currentOrigin);
    }

    aatpe_cmdq::EnqueueResult eq = aatpe_cmdq::commandQueueEnqueue(actualCmd);
    auditCommand(actualCmd, g_peerUid, eq.ok, "webui",
                 eq.ok ? std::string() : eq.message);
    if (!eq.ok) {
        int httpStatus = (eq.error_code == aatpe::ERR_BUSY) ? 503 : 500;
        std::string body = "{\"ok\":false,\"status\":\"rejected\",\"error_code\":" +
                           std::to_string(eq.error_code) + ",\"error\":\"" +
                           jsonEscape(eq.message) + "\"}";
        return buildHttpResponse(httpStatus, "application/json", body, g_currentOrigin);
    }
    std::string resp = "{\"ok\":true,\"status\":\"queued\",\"command_id\":\"" + eq.command_id +
                        "\",\"command\":\"" + jsonEscape(actualCmd) +
                        "\",\"hmac_required\":" + (hmacRequired ? "1" : "0") +
                        ",\"session_auth\":" + (sessionAuthenticated ? "1" : "0") + "}";
    return buildHttpResponse(200,"application/json",resp,g_currentOrigin);
}

std::string handleSession(const std::string& request, const std::string& fullPath) {
    std::string cmd;
    size_t qpos = fullPath.find('?');
    if (qpos != std::string::npos) {
        std::string qs = fullPath.substr(qpos + 1);
        queryParamEq(qs, "cmd", cmd);
    }
    cmd = urlDecode(cmd);
    while (!cmd.empty() && (cmd.back()=='\n'||cmd.back()=='\r'||cmd.back()==' '||cmd.back()=='\t')) cmd.pop_back();

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
    if (!hmacRequired) {
    } else if (hasSig && hasTs && actualCmd == "session_issue") {
        if (nonce.empty()) {
            return buildHttpResponse(403,"application/json",
                "{\"ok\":false,\"error\":\"session_issue requires .nonce=<hex> replay protection\"}",
                g_currentOrigin);
        }
        int64_t now = (int64_t)time(nullptr);
        int64_t age = now - reqTs;
        if (age <= 300 && age >= -300) {
            std::string signedData = actualCmd + ".ts=" + std::to_string(reqTs) + ".nonce=" + nonce;
            authed = verifyCommandHmac(signedData.c_str(), sig.c_str());
            if (authed && nonceReplayOrInsert(signedData, reqTs)) {
                return buildHttpResponse(403,"application/json",
                    "{\"ok\":false,\"error\":\"replay detected (session proof already used)\"}",
                    g_currentOrigin);
            }
        }
    }
    if (!authed && checkDaemonTokenAuth(request, fullPath)) authed = true;
    if (!authed) {
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
    std::string body = "{\"ok\":true,\"token\":\"" + tok + "\",\"expires_in\":300}";
    return buildHttpResponse(200,"application/json",body,g_currentOrigin);
}

bool isCommandAllowed(const std::string& rawCmd) {
    std::string cmd = rawCmd;
    while (!cmd.empty() && (cmd.back()=='\n'||cmd.back()=='\r'||cmd.back()==' '||cmd.back()=='\t')) cmd.pop_back();
    while (!cmd.empty() && (cmd.front()==' '||cmd.front()=='\t')) cmd.erase(cmd.begin());
    if (cmd.empty()) return false;

    size_t eq = cmd.find('=');
    std::string key = (eq == std::string::npos) ? cmd : cmd.substr(0, eq);
    std::string value = (eq == std::string::npos) ? std::string() : cmd.substr(eq + 1);

    if (eq == std::string::npos) {
        if (key == "restart" || key == "stop" || key == "clear_log" ||
            key == "reload_config" || key == "unlock_watchdog") return true;
        return false;
    }

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
        return value == "1" || value == "0";
    }
    if (key == "explain_tid") {
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
        return inRange(value, 10, 5000);
    }
    if (key == "set_confidence_threshold") {
        return inRange(value, 1, 100);
    }
    if (key == "set_migration_cost_threshold") {
        return inRange(value, 1, 100);
    }
    if (key == "set_max_migrations") {
        return inRange(value, 1, 1000);
    }
    if (key == "set_thermal_limit_cpu") {
        return inRange(value, 0, 200);
    }
    if (key == "set_thermal_limit_skin") {
        return inRange(value, 0, 200);
    }
    if (key == "set_thermal_limit_battery") {
        return inRange(value, 0, 200);
    }
    if (key == "set_thermal_emergency") {
        return inRange(value, 0, 200);
    }
    if (key == "game_add" || key == "game_remove") {
        if (value.empty() || value.size() > 256) return false;
        if (!isalpha((unsigned char)value[0])) return false;
        if (value.front() == '.' || value.back() == '.') return false;
        if (value.find("..") != std::string::npos) return false;
        for (char c : value) {
            if (isalnum((unsigned char)c)) continue;
            if (c == '.' || c == '_') continue;
            return false;
        }
        return true;
    }
    return false;
}

static std::string stateSnapshotLookup(const std::string& key) {
    if (!stateCacheIsSeeded()) stateCacheSeedFromDir(STATE_DIR);
    std::vector<std::pair<std::string,std::string>> snap = stateCacheSnapshot();
    for (const auto& kv : snap) {
        if (kv.first == key) return kv.second;
    }
    return std::string();
}

std::string handleFusion() {
    std::string value = stateSnapshotLookup("fusion_weights");
    if (value.empty()) {
        value = "{\"heuristic_weight\":0,\"ml_weight\":0,\"hybrid_weight\":0,\"history_weight\":0,\"total_decisions\":0}";
    }
    return buildHttpResponse(200, "application/json", value, g_currentOrigin);
}

std::string handleApps() {
    std::vector<std::string> pkgs = aatpe::apklabel::listInstalledPackagesAllUsers();
    if (pkgs.empty()) pkgs = aatpe::apps::listInstalledPackages();
    std::string body = "{\"apps\":[";
    for (size_t i = 0; i < pkgs.size(); ++i) {
        if (i) body += ",";
        std::string lab = aatpe::apklabel::peekCachedLabel(pkgs[i]);
        if (lab.empty()) lab = pkgs[i];
        body += "{\"pkg\":\"" + jsonEscape(pkgs[i]) + "\",\"label\":\"" + jsonEscape(lab) + "\"}";
    }
    body += "],\"manual\":[";
    std::string manual = aatpe::gamelist::currentManualGameList();
    std::istringstream ss(manual);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        size_t a = line.find_first_not_of(" \t\r\n");
        size_t b = line.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        line = line.substr(a, b - a + 1);
        if (!first) body += ",";
        body += "\"" + jsonEscape(line) + "\"";
        first = false;
    }
    body += "]}";
    if (pkgs.empty()) {
        std::string note = "empty: pm/cmd/and /data/app scan all returned no packages (check root, SELinux)";
        body.insert(body.size() - 1, ",\"note\":\"" + note + "\"");
    }
    return buildHttpResponse(200, "application/json", body, g_currentOrigin);
}

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

std::string handleFrameApps() {
    std::string body = aatpe_buildFrameAppsJson();
    return buildHttpResponse(200, "application/json", body, g_currentOrigin);
}

static std::string diagBundleOutPath() {
    return std::string(STATE_DIR) + "/aatpe-diag.tgz";
}
static bool diagBundlePrepareOutput(const std::string& path) {
    (void)::unlink(path.c_str());
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}
std::string handleDiagBundle() {
    std::string outPath = diagBundleOutPath();
    if (!diagBundlePrepareOutput(outPath)) {
        return buildHttpResponse(204, "application/json", "", g_currentOrigin);
    }
    std::vector<std::string> argv = {
        "/system/bin/tar", "-czf", outPath,
        "/data/adb/modules/aatpe/aatped.log",
        "/data/adb/modules/aatpe/service.log",
        "/dev/aatpe/state/frame_apps.json",
        "/dev/aatpe/state/audit.log",
        "/data/tombstones/tombstone_00",
    };
    int st = -1;
    (void)aatpeExecArgv(argv, "DIAG_BUNDLE", &st, true);
    std::string bytes = readFileToString(outPath);
    (void)::unlink(outPath.c_str());
    const size_t kMaxDiag = size_t(2u) * 1024u * 1024u;
    if (bytes.empty() || bytes.size() > kMaxDiag) {
        return buildHttpResponse(204, "application/json", "", g_currentOrigin);
    }
    return buildHttpResponse(200, "application/gzip", bytes, g_currentOrigin);
}

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

static std::mutex g_tokenCacheMutex;
static std::string g_cachedDaemonToken;
static long long g_cachedTokenMtimeSec = 0;

static std::string getOrCreateDaemonToken() {
    std::string tokenPath = std::string(STATE_DIR) + "/daemon_token";

    {
        std::lock_guard<std::mutex> lk(g_tokenCacheMutex);
        if (!g_cachedDaemonToken.empty()) {
            struct stat st;
            if (stat(tokenPath.c_str(), &st) == 0 &&
                (long long)st.st_mtime == g_cachedTokenMtimeSec) {
                return g_cachedDaemonToken;
            }
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
    int fd = open(tokenPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        size_t off = 0;
        size_t len = token.size();
        bool ok = true;
        while (off < len) {
            ssize_t w = write(fd, token.c_str() + off, len - off);
            if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
            if (w == 0) { ok = false; break; }
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
    bool trustedChannel = (g_peerUid == 0 || g_peerUid == 1000);
    if (trustedChannel) {
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
    std::string lowerReq = request;
    std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
    size_t hdrEnd = lowerReq.find("\r\n\r\n");
    if (hdrEnd != std::string::npos) lowerReq.resize(hdrEnd);
    size_t hp = lowerReq.find("\r\nx-daemon-token:");
    if (hp != std::string::npos) {
        size_t hs = hp + 17;
        size_t he = request.find("\r\n", hs);
        std::string val = (he != std::string::npos) ? request.substr(hs, he - hs) : request.substr(hs);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        while (!val.empty() && (val.back()=='\r'||val.back()==' ')) val.pop_back();
        if (constantTimeStringEqual(val, daemonToken)) return true;
    }
    return false;
}

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

std::string handleHmacKey(const std::string& request, const std::string& fullPath) {
    if (!checkDaemonTokenAuth(request, fullPath)) {
        return buildHttpResponse(401, "application/json",
            "{\"ok\":false,\"error\":\"daemon token authentication required\"}", g_currentOrigin);
    }
    std::string key;
    bool hmacActive;
    {
        std::lock_guard<std::mutex> hmacLk(g_hmacMutex);
        key = g_hmacKey;
        hmacActive = g_hmacActive;
    }
    while (!key.empty() && (key.back()=='\n'||key.back()=='\r')) key.pop_back();
    bool hmacRequired;
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        hmacRequired = g_cfg.hmacRequired;
    }
    std::string resp = "{\"hmac_key\":\"" + jsonEscape(key) + "\",\"required\":" +
                        (hmacRequired ? "1" : "0") +
                        ",\"active\":" + (hmacActive ? "1" : "0") + "}";
    return buildHttpResponse(200,"application/json",resp,g_currentOrigin);
}

[[maybe_unused]]
std::string handleDaemonToken() {
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

std::string handleRequest(const std::string& request) {
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version;

    size_t qpos = path.find('?');
    std::string routePath = (qpos != std::string::npos) ? path.substr(0, qpos) : path;

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
            while (!g_currentOrigin.empty() && (g_currentOrigin.back()==' '||g_currentOrigin.back()=='\t'||g_currentOrigin.back()=='\r')) g_currentOrigin.pop_back();
        }
    }
    if (method == "OPTIONS") return buildHttpResponse(200,"text/plain","",g_currentOrigin);

    if (!rateLimitAllowed()) {
        return buildHttpResponse(429, "application/json",
            "{\"ok\":false,\"error\":\"rate limit exceeded (max 100 req/s)\"}",
            g_currentOrigin);
    }
    if (routePath == "/control" || routePath == "/api/control") {
        std::string cmd;
        if (method == "POST") {
            size_t bp = request.find("\r\n\r\n");
            cmd = (bp!=std::string::npos) ? request.substr(bp+4) : "";
            if (cmd.size() > kMaxRequestBody) {
                return buildHttpResponse(413,"application/json",
                    "{\"ok\":false,\"error\":\"request body too large (max 65536 bytes)\"}",g_currentOrigin);
            }
        } else if (method == "GET") {
            size_t qp = path.find('?');
            if (qp != std::string::npos) {
                std::string query = path.substr(qp + 1);
                std::string cmdVal;
                if (queryParamEq(query, "cmd", cmdVal)) {
                    cmd = urlDecode(cmdVal);
                }
            }
        } else {
            return buildHttpResponse(405,"text/plain","Method Not Allowed",g_currentOrigin);
        }
        bool postCsrfSafe = (method == "POST") && isCsrfSafePost(request);
        return handleControl(cmd, method == "GET", postCsrfSafe, &request);
    }
    if (method != "GET") return buildHttpResponse(405,"text/plain","Method Not Allowed",g_currentOrigin);

    if (routePath == "/health" || routePath == "/healthz") {
        return handleHealth();
    }
    if (routePath == "/state/public" || routePath == "/public") {
        return handleStatePublic();
    }
    if (routePath == "/state/all" || routePath == "/state" || routePath == "/" || routePath == "/api/state" ||
        routePath == "/status" || routePath == "/api/status") {
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required for /state/all\"}",
                g_currentOrigin);
        }
        {
            std::string stateQuery = (qpos != std::string::npos) ? path.substr(qpos + 1) : "";
            return handleStateAll(stateQuery);
        }
    }
    if (routePath == "/version") {
        std::string body = std::string("{\"version\":\"") + AATPE_VERSION + "\"}";
        return buildHttpResponse(200, "application/json", body, g_currentOrigin);
    }
    {
        const std::string prefix = "/state/";
        if (routePath.rfind(prefix, 0) == 0 && routePath.size() > prefix.size()) {
            std::string name = routePath.substr(prefix.size());
            bool valid = !name.empty() && name.size() <= 64;
            for (char c : name) {
                if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_')) { valid = false; break; }
            }
            if (!valid) {
                return buildHttpResponse(400, "application/json",
                    "{\"ok\":false,\"error\":\"invalid state name\"}", g_currentOrigin);
            }
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
    if (routePath == "/api/session" || routePath == "/session") return handleSession(request, path);
    if (routePath == "/api/daemon_token" || routePath == "/daemon_token") {
        return buildHttpResponse(403, "application/json",
            "{\"ok\":false,\"error\":\"daemon_token unavailable over HTTP; read /dev/aatpe/state/daemon_token as root\"}",
            g_currentOrigin);
    }
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
    if (routePath == "/api/frame_apps" || routePath == "/frame_apps") {
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required for /api/frame_apps\"}",
                g_currentOrigin);
        }
        return handleFrameApps();
    }
    if (routePath == "/api/diag_bundle" || routePath == "/diag_bundle") {
        if (!checkDaemonTokenAuth(request, path)) {
            return buildHttpResponse(401, "application/json",
                "{\"ok\":false,\"error\":\"daemon token required for /api/diag_bundle\"}",
                g_currentOrigin);
        }
        return handleDiagBundle();
    }
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

    {
        const std::string cmdPrefix = "/api/command/";
        if (routePath.rfind(cmdPrefix, 0) == 0) {
            if (!checkDaemonTokenAuth(request, path)) {
                return buildHttpResponse(401, "application/json",
                    "{\"ok\":false,\"error\":\"daemon token required\"}", g_currentOrigin);
            }
            std::string id = routePath.substr(cmdPrefix.size());
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

std::string readHttpRequest(int clientFd, bool* outOversize) {
    char buf[8192];
    std::string request;
    *outOversize = false;

    static const size_t kMaxHeaderSize = 16384;

    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        ssize_t n = recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        request.append(buf, (size_t)n);
        headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos && request.size() > kMaxHeaderSize) {
            *outOversize = true;
            return request;
        }
        if (request.size() > kMaxRequestBody) {
            *outOversize = true;
            return request;
        }
    }
    if (headerEnd == std::string::npos) return request;

    std::string headers = request.substr(0, headerEnd);
    std::string lowerHeaders = headers;
    std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(), [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
    {
        size_t dupProbe = 0;
        int clCount = 0;
        while ((dupProbe = lowerHeaders.find("content-length:", dupProbe)) != std::string::npos) {
            if (dupProbe == 0 || lowerHeaders[dupProbe-1] == '\n') clCount++;
            dupProbe += 15;
            if (clCount > 1) {
                *outOversize = true;
                return request;
            }
        }
    }
    size_t clPos = lowerHeaders.find("content-length:");
    size_t contentLength = 0;
    bool hasContentLength = false;
    if (lowerHeaders.find("transfer-encoding:") != std::string::npos) {
        *outOversize = true;
        return request;
    }
    if (clPos != std::string::npos) {
        size_t valStart = clPos + 15;
        while (valStart < headers.size() && headers[valStart] == ' ') valStart++;
        size_t valEnd = headers.find("\r\n", valStart);
        if (valEnd == std::string::npos) valEnd = headers.size();
        std::string clStr = headers.substr(valStart, valEnd - valStart);
        try {
            size_t used = 0;
            unsigned long long v = std::stoull(clStr, &used);
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
        size_t bodyRead = request.size() - bodyStart;
        while (bodyRead < contentLength) {
            size_t want = std::min(sizeof(buf), contentLength - bodyRead);
            ssize_t n = recv(clientFd, buf, want, 0);
            if (n <= 0) break;
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

struct StateJsonCache {
    std::mutex mu;
    std::string data;
    std::chrono::steady_clock::time_point time;
};
static StateJsonCache& stateJsonCache() {
    static StateJsonCache* c = new StateJsonCache();
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

void handleSseConnection(int clientFd, const std::string& origin) {
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOG_DEBUG("webui: setsockopt(SO_RCVTIMEO) failed for SSE client: %s", aatpeErrStr(errno));
    }
    {
        int sndbuf = 4096;
        if (setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
            LOG_DEBUG("webui: setsockopt(SO_SNDBUF) failed for SSE client: %s", aatpeErrStr(errno));
        }
    }

    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n";
    hdr << "Content-Type: text/event-stream\r\n";
    hdr << "Cache-Control: no-cache\r\n";
    hdr << "Connection: keep-alive\r\n";
    hdr << "X-Accel-Buffering: no\r\n";
    hdr << "Server: AATPE-WebUI/1.0\r\n";
    appendCorsHeaders(hdr, origin);
    hdr << "\r\n";
    std::string hdrStr = hdr.str();
    if (!sendAll(clientFd, hdrStr.c_str(), hdrStr.size())) { close(clientFd); return; }

    struct SseCountGuard {
        SseCountGuard()  { g_sseClientCount.fetch_add(1, std::memory_order_acq_rel); }
        ~SseCountGuard() { g_sseClientCount.fetch_sub(1, std::memory_order_acq_rel); }
    };
    SseCountGuard sseCountGuard;

    int idleTicks = 0;
    int heartbeatTicks = 0;
    uint64_t lastPushedGen = (uint64_t)-1;
    uint64_t lastChangePushTicks = 0;
    const int kNoChangeProbeTicks = 8;

    while (g_httpRunning.load()) {
        char drainBuf[256];
        ssize_t n = recv(clientFd, drainBuf, sizeof(drainBuf), MSG_DONTWAIT);
        if (n == 0) break;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;

        idleTicks++;
        heartbeatTicks++;

        uint64_t curGen = stateCacheGeneration();
        if (curGen != lastPushedGen) {
            std::string payload = getCachedAggregateStateJson();
            std::string frame = "data: " + payload + "\n\n";
            if (!sendAll(clientFd, frame.c_str(), frame.size())) break;
            lastPushedGen = curGen;
            lastChangePushTicks = idleTicks;
        } else if (idleTicks - lastChangePushTicks >= kNoChangeProbeTicks) {
            std::string probe = "data: {\"state_generation\":" +
                                std::to_string(curGen) + "}\n\n";
            if (!sendAll(clientFd, probe.c_str(), probe.size())) break;
            lastChangePushTicks = idleTicks;
        }

    static const int kSseKeepaliveSec = []() {
        const char* envKp = getenv("AATPE_SSE_KEEPALIVE_SEC");
        int v = envKp ? atoi(envKp) : 20;
        return v < 1 ? 1 : v;
    }();
    const int kHeartbeatTicks = kSseKeepaliveSec * 4;
        if (heartbeatTicks >= kHeartbeatTicks) {
            heartbeatTicks = 0;
            const char* hb = ": keepalive\n\n";
            if (!sendAll(clientFd, hb, strlen(hb))) break;
        }

        usleep(250000);
    }
    close(clientFd);
}

void handleConnection(int clientFd, std::shared_ptr<ConnEntry> self) {
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
    g_rateBucketKey = g_peerUid;
    if (g_rateBucketKey < 0) {
        g_rateBucketKey = kSharedTcpBucketKey;
    }

    struct DoneGuard {
        std::shared_ptr<ConnEntry> e;
        ~DoneGuard() {
            if (e) {
                e->done.store(true);
                e->clientFd.store(-1);
            }
        }
    } doneGuard{self};

    struct timeval tv;
    tv.tv_sec = kRecvTimeoutSec;
    tv.tv_usec = 0;
    if (setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOG_DEBUG("webui: setsockopt(SO_RCVTIMEO) failed: %s", aatpeErrStr(errno));
    }
    struct timeval tvw;
    tvw.tv_sec = sendTimeoutSec();
    tvw.tv_usec = 0;
    if (setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tvw, sizeof(tvw)) < 0) {
        LOG_DEBUG("webui: setsockopt(SO_SNDTIMEO) failed: %s", aatpeErrStr(errno));
    }

    static const int kKeepAliveMaxRequests = 64;
    for (int kaIdx = 0; kaIdx < kKeepAliveMaxRequests; ++kaIdx) {
    bool oversize = false;
    std::string request = readHttpRequest(clientFd, &oversize);

    if (!g_httpRunning.load()) {
        if (self) self->clientFd.store(-1);
        close(clientFd); return;
    }
    if (request.empty()) break;

    if (oversize) {
        std::string resp = buildHttpResponse(413, "application/json",
            "{\"ok\":false,\"error\":\"request body too large (max 65536 bytes)\"}",
            g_currentOrigin);
        sendAll(clientFd, resp.data(), resp.size());
        break;
    } else if (!request.empty()) {
        if (g_httpRunning.load()) {
            std::string sseMethod, ssePath, sseVersion;
            {
                std::istringstream ss(request);
                ss >> sseMethod >> ssePath >> sseVersion;
            }
            size_t sseQ = ssePath.find('?');
            std::string sseRoute = (sseQ != std::string::npos) ? ssePath.substr(0, sseQ) : ssePath;
            if (sseMethod == "GET" && (sseRoute == "/events" || sseRoute == "/sse" || sseRoute == "/api/events")) {
                if (!checkSseTicketAuth(request, ssePath)) {
                    std::string deny = buildHttpResponse(401, "application/json",
                        "{\"ok\":false,\"error\":\"daemon token required for /events\"}",
                        "");
                    sendAll(clientFd, deny.data(), deny.size());
                    if (self) self->clientFd.store(-1);
                    close(clientFd);
                    return;
                }
                {
                    SseSlotGuard slot = sseTryAcquire(g_peerUid);
                    if (!slot.acquired) {
                        std::string deny = buildHttpResponse(429, "application/json",
                            "{\"ok\":false,\"error\":\"sse_limit\",\"sse_limit\":true,\"retry_after\":1}",
                            "");
                        sendAll(clientFd, deny.data(), deny.size());
                        if (self) self->clientFd.store(-1);
                        close(clientFd);
                        return;
                    }
                    std::string sseOrigin;
                    std::string lowerReq = request;
                    std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), [](unsigned char c){ return (char)(c>='A'&&c<='Z'? c+32 : c); });
                    size_t op = lowerReq.find("\r\norigin:");
                    if (op != std::string::npos) {
                        size_t os = op + 9;
                        size_t oe = request.find("\r\n", os);
                        if (oe != std::string::npos) {
                            sseOrigin = request.substr(os, oe - os);
                            while (!sseOrigin.empty() && (sseOrigin.front()==' '||sseOrigin.front()=='\t')) sseOrigin.erase(sseOrigin.begin());
                            while (!sseOrigin.empty() && (sseOrigin.back()==' '||sseOrigin.back()=='\t'||sseOrigin.back()=='\r')) sseOrigin.pop_back();
                        }
                    }
                    if (self) self->clientFd.store(-1);
                    handleSseConnection(clientFd, sseOrigin);
                    return;
                }
            }
            std::string response = handleRequest(request);
            sendAll(clientFd, response.data(), response.size());
            if (response.size() >= 12 && response.compare(0,5,"HTTP/")==0) {
                size_t sp = response.find(' ');
                int st = atoi(response.c_str()+sp+1);
                if (st >= 400) break;
            }
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
    }
    if (self) self->clientFd.store(-1);
    close(clientFd);
}

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
        g_lastClientAcceptMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_relaxed);
        std::lock_guard<std::mutex> connLock(g_connMutex);
        for (auto it = g_connThreads.begin(); it != g_connThreads.end(); ) {
            if ((*it)->done.load()) {
                (*it)->th.join();
                it = g_connThreads.erase(it);
            } else {
                ++it;
            }
        }
        if (g_connThreads.size() >= kMaxConnections) {
            close(clientFd);
            continue;
        }
        auto entry = std::make_shared<ConnEntry>();
        entry->clientFd = clientFd;
        std::shared_ptr<ConnEntry> entryShared = entry;
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

}

#ifdef AATPE_HOST_TEST
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

    signal(SIGPIPE, SIG_IGN);

    std::string bootToken = getOrCreateDaemonToken();
    if (bootToken.empty()) {
        LOG_ERROR("startHttpServer: cannot mint a CSPRNG daemon token; HTTP WebUI not started (fail closed)");
        return false;
    }

    g_listenFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_listenFd < 0) return false;

    int opt = 1;
    if (setsockopt(g_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_DEBUG("webui: setsockopt(SO_REUSEADDR) failed: %s", aatpeErrStr(errno));
    }

    bool bound = false;
    for (int port = 8888; port <= 8899; port++) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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
    g_httpThread = std::thread(acceptLoop, g_listenFd);
    return true;
}

void stopHttpServer() {
    g_httpRunning.store(false);

    std::vector<std::shared_ptr<ConnEntry>> snapshot;
    {
        std::lock_guard<std::mutex> connLock(g_connMutex);
        snapshot.assign(g_connThreads.begin(), g_connThreads.end());
    }
    for (auto& e : snapshot) {
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

    if (g_httpThread.joinable()) g_httpThread.join();
}

int getHttpPort() {
    return g_httpPort.load();
}

bool hasActiveClients() {
    if (!g_httpRunning.load()) return false;
    long long lastAccept = g_lastClientAcceptMs.load(std::memory_order_relaxed);
    if (lastAccept != 0) {
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs - lastAccept < kClientGraceMs) return true;
    }
    std::lock_guard<std::mutex> connLock(g_connMutex);
    for (const auto& e : g_connThreads) {
        if (!e->done.load()) return true;
    }
    return false;
}

void ensureDaemonToken() {
    std::string tok = getOrCreateDaemonToken();
    (void)tok;
}

std::string buildAggregateStateJson() {
    if (!stateCacheIsSeeded()) {
        stateCacheSeedFromDir(STATE_DIR);
    }
    std::vector<std::pair<std::string,std::string>> snap = stateCacheSnapshot();
    std::ostringstream body;
    body << "{";
    body << "\"state_generation\":" << stateCacheGeneration();
    for (const auto& kv : snap) {
        if (kv.first == "state_generation") continue;
        body << ",";
        body << "\"" << jsonEscape(kv.first) << "\":\"" << jsonEscape(kv.second) << "\"";
    }
    body << "}";
    return body.str();
}

}
}

namespace aatpe {
namespace webui_http {
void auditSensitiveCommand(const std::string& cmd, const std::string& source,
                            bool ok, const std::string& reason) {
    auditCommand(cmd, 0, ok, source, reason);
}
}
}
