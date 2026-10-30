#include "aatpe_command_queue.h"

#include "aatped.h"
#include "aatped_control.h"
#include "aatped_utils.h"
#include "aatped_logging.h"
#include "aatpe_command_dispatcher.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>

namespace aatpe_cmdq {
namespace {

std::string commandsDir() { return std::string(RUN_DIR) + "/commands"; }

struct StatusEntry {
    std::string command_id;
    std::string line;
    std::string state;
    int         error_code = 0;
    std::string message;
    std::string applied_value;
    long long   enqueuedMs = 0;
    long long   finishedMs = 0;
};

std::mutex                g_statusMutex;
std::vector<StatusEntry>  g_status;
static constexpr int      kMaxStatusEntries = 256;
static std::unordered_map<std::string, size_t> g_statusIndex;

static void rebuildStatusIndex() {
    g_statusIndex.clear();
    g_statusIndex.reserve(g_status.size());
    for (size_t i = 0; i < g_status.size(); ++i)
        g_statusIndex[g_status[i].command_id] = i;
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void appendJsonEscaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void statusUpsert(const StatusEntry& e) {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    auto it = g_statusIndex.find(e.command_id);
    if (it != g_statusIndex.end()) {
        g_status[it->second] = e;
        return;
    }
    g_status.push_back(e);
    g_statusIndex[e.command_id] = g_status.size() - 1;
    while ((int)g_status.size() > kMaxStatusEntries) {
        g_status.erase(g_status.begin());
        rebuildStatusIndex();
    }
}

int countQueuedFiles() {
    DIR* d = opendir(commandsDir().c_str());
    if (!d) return 0;
    int n = 0;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name.rfind("cmd-", 0) != 0) continue;
        bool numeric = name.size() > 4;
        for (size_t i = 4; i < name.size() && numeric; ++i) {
            if (name[i] < '0' || name[i] > '9') numeric = false;
        }
        if (numeric) ++n;
    }
    closedir(d);
    return n;
}

long long parseCmdNumber(const std::string& name) {
    if (name.rfind("cmd-", 0) != 0) return -1;
    long long v = 0;
    for (size_t i = 4; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        int digit = name[i] - '0';
        if (v > (LLONG_MAX - digit) / 10) return -1;
        v = v * 10 + digit;
    }
    return name.size() > 4 ? v : -1;
}

std::string formatCmdId(long long n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "cmd-%06lld", n);
    return buf;
}

std::mutex       g_idMutex;
std::atomic<long long> s_nextId{0};
std::atomic<bool>      s_idSeeded{false};

std::atomic<int> s_queuedDepth{0};

void seedIdIfNeeded() {
    if (s_idSeeded.load()) return;
    long long maxN = 0;
    int depth = 0;
    DIR* d = opendir(commandsDir().c_str());
    if (d) {
        struct dirent* de;
        while ((de = readdir(d)) != nullptr) {
            long long n = parseCmdNumber(de->d_name);
            if (n > maxN) maxN = n;
            if (n >= 0) ++depth;
        }
        closedir(d);
    }
    s_nextId.store(maxN + 1);
    s_queuedDepth.store(depth);
    s_idSeeded.store(true);

    std::string dir = commandsDir();
    DIR* d2 = opendir(dir.c_str());
    if (d2) {
        struct dirent* de;
        while ((de = readdir(d2)) != nullptr) {
            std::string name = de->d_name;
            if (parseCmdNumber(name) < 0) continue;
            std::string path = dir + "/" + name;
            std::string content = readFile(path.c_str());
            while (!content.empty() &&
                   (content.back()=='\n'||content.back()=='\r'||content.back()==' '||content.back()=='\t'))
                content.pop_back();
            if (!commandLineIsReplaySafe(content)) {
                std::string qpath = path + ".quarantined";
                if (::rename(path.c_str(), qpath.c_str()) == 0) {
                    LOG_WARN("cmdq: quarantined leftover non-idempotent command '%s' ('%s')",
                             name.c_str(), content.c_str());
                    --depth;
                }
            }
        }
        closedir(d2);
    }
    s_queuedDepth.store(depth);
}

}

EnqueueResult commandQueueEnqueue(const std::string& line) {
    EnqueueResult res;

    std::string cmd = line;
    while (!cmd.empty() && (cmd.back()=='\n'||cmd.back()=='\r'||cmd.back()==' '||cmd.back()=='\t'))
        cmd.pop_back();
    while (!cmd.empty() && (cmd.front()==' '||cmd.front()=='\t'))
        cmd.erase(cmd.begin());
    if (cmd.empty()) {
        res.error_code = aatpe::ERR_INVALID_ARGUMENT;
        res.message = "empty command";
        return res;
    }
    if (cmd.size() > 256) {
        res.error_code = aatpe::ERR_INVALID_ARGUMENT;
        res.message = "command too long";
        return res;
    }
    if (!isValidControlChars(cmd)) {
        res.error_code = aatpe::ERR_INVALID_ARGUMENT;
        res.message = "command rejected: disallowed characters";
        LOG_WARN("cmdq: enqueue rejected line with disallowed characters");
        return res;
    }

    mkdirIfNotExist(commandsDir().c_str(), 0700);

    std::lock_guard<std::mutex> lk(g_idMutex);
    seedIdIfNeeded();

    if (s_queuedDepth.load(std::memory_order_relaxed) >= kMaxQueueDepth) {
        res.error_code = aatpe::ERR_BUSY;
        res.message = "command queue full (max " + std::to_string(kMaxQueueDepth) + ")";
        LOG_WARN("cmdq: enqueue rejected, queue depth >= %d", kMaxQueueDepth);
        return res;
    }

    long long n = s_nextId.fetch_add(1);
    std::string id = formatCmdId(n);

    std::string finalPath = commandsDir() + "/" + id;
    std::string tmpPath   = finalPath + ".tmp." + std::to_string((long long)getpid());

    if (!writeFileAtomic(tmpPath.c_str(), cmd)) {
        res.error_code = aatpe::ERR_APPLY_FAILED;
        res.message = "cannot write command file";
        return res;
    }
    s_queuedDepth.fetch_add(1, std::memory_order_release);
    if (::rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
        ::unlink(tmpPath.c_str());
        s_queuedDepth.fetch_sub(1, std::memory_order_release);
        res.error_code = aatpe::ERR_APPLY_FAILED;
        res.message = "cannot enqueue command (rename)";
        return res;
    }

    StatusEntry e;
    e.command_id  = id;
    e.line       = cmd;
    e.state      = "queued";
    e.enqueuedMs = nowMs();
    statusUpsert(e);

    res.ok = true;
    res.command_id = id;
    res.error_code = 0;
    res.message = "queued";
    return res;
}

bool tryPush(const std::string& line, EnqueueResult& out) {
    out = commandQueueEnqueue(line);
    return out.ok;
}

int commandQueueDrain() {
    std::string dir = commandsDir();
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;

    std::set<std::string> names;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (parseCmdNumber(name) >= 0) names.insert(name);
    }
    closedir(d);

    int drained = 0;
    for (const std::string& name : names) {
        std::string path = dir + "/" + name;
        std::string content = readFile(path.c_str());
        while (!content.empty() &&
               (content.back()=='\n'||content.back()=='\r'||content.back()==' '||content.back()=='\t'))
            content.pop_back();
        if (content.empty()) {
            ::unlink(path.c_str());
            s_queuedDepth.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(g_statusMutex);
            auto idx = g_statusIndex.find(name);
            if (idx != g_statusIndex.end()) g_status[idx->second].state = "executing";
        }

        if (!isValidControlChars(content)) {
            LOG_WARN("cmdq: rejecting '%s' (invalid control chars)", name.c_str());
            StatusEntry e;
            e.command_id    = name;
            e.line          = content;
            e.state         = "rejected";
            e.error_code     = aatpe::ERR_INVALID_ARGUMENT;
            e.message       = "invalid characters (only [a-z0-9_=.-], <=256 bytes)";
            e.applied_value = "";
            e.finishedMs    = nowMs();
            statusUpsert(e);
            ::unlink(path.c_str());
            s_queuedDepth.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }

        ControlResult cr = executeControlLine(content);
        LOG_INFO("cmdq: executed '%s' -> ok=%d err=%d", content.c_str(),
                 cr.success ? 1 : 0, cr.error_code);

        StatusEntry e;
        e.command_id   = name;
        e.line         = content;
        e.state        = cr.success ? "success" : "failed";
        e.error_code   = cr.success ? 0 : cr.error_code;
        e.message      = cr.message;
        e.applied_value = cr.applied_value;
        e.finishedMs   = nowMs();
        statusUpsert(e);

        ::unlink(path.c_str());
        s_queuedDepth.fetch_sub(1, std::memory_order_relaxed);
        ++drained;
    }
    return drained;
}

std::string commandStatusJson(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    auto idx = g_statusIndex.find(id);
    if (idx != g_statusIndex.end()) {
        const StatusEntry& s = g_status[idx->second];
        std::string out = "{";
        out += "\"command_id\":"; appendJsonEscaped(out, s.command_id); out += ",";
        out += "\"command\":";    appendJsonEscaped(out, s.line); out += ",";
        out += "\"status\":";      appendJsonEscaped(out, s.state); out += ",";
        out += "\"result\":{";
        out += "\"success\":";   out += s.state=="success" ? "true" : "false"; out += ",";
        out += "\"error_code\":"; out += std::to_string(s.error_code); out += ",";
        out += "\"message\":";    appendJsonEscaped(out, s.message); out += ",";
        out += "\"applied_value\":"; appendJsonEscaped(out, s.applied_value);
        out += "}}";
        return out;
    }
    std::string out = "{";
    out += "\"command_id\":"; appendJsonEscaped(out, id); out += ",";
    out += "\"status\":\"unknown\",";
    out += "\"result\":{\"success\":false,\"error_code\":";
    out += std::to_string(aatpe::ERR_UNKNOWN_COMMAND);
    out += ",\"message\":\"unknown or expired command id\"}}";
    return out;
}

int commandQueueDepth() {
    mkdirIfNotExist(commandsDir().c_str(), 0700);
    return countQueuedFiles();
}

bool commandLineIsReplaySafe(const std::string& line) {
    std::string s = line;
    while (!s.empty() &&
           (s.back()=='\n'||s.back()=='\r'||s.back()==' '||s.back()=='\t'))
        s.pop_back();
    while (!s.empty() &&
           (s.front()==' '||s.front()=='\t'))
        s.erase(s.begin());
    if (s.empty()) return true;

    static const char* kNonReplayable[] = {
        "restart",
        "stop",
        "clear_log",
        "manual_unlock_watchdog",
        "restored_after_crash_window",
        nullptr
    };
    for (int i = 0; kNonReplayable[i]; ++i) {
        if (s == kNonReplayable[i]) return false;
    }

    static const char* kNonReplayablePrefix[] = {
        "shadow_dump",
        nullptr
    };
    for (int i = 0; kNonReplayablePrefix[i]; ++i) {
        const char* p = kNonReplayablePrefix[i];
        size_t plen = strlen(p);
        if (s.size() < plen) continue;
        if (s.compare(0, plen, p) != 0) continue;
        if (s.size() == plen || s[plen] == '=') return false;
    }

    return true;
}

}
