// aatpe_command_queue.cpp - 
// See aatpe_command_queue.h for the design.
#include "aatpe_command_queue.h"

#include "aatped.h"            // RUN_DIR, writeFileAtomic
#include "aatped_control.h"    // executeControlLine, ControlResult
#include "aatped_utils.h"      // mkdirIfNotExist, readFile
#include "aatped_logging.h"
#include "aatpe_command_dispatcher.h"  // aatpe::ERR_*

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

// ---------------------------------------------------------------------------
// Status registry (in-memory). Bounded to kMaxStatusEntries; oldest entries are
// evicted so a busy WebUI cannot grow this without limit.
// ---------------------------------------------------------------------------
struct StatusEntry {
    std::string command_id;
    std::string line;
    std::string state;           // queued | executing | success | failed
    int         error_code = 0;
    std::string message;
    std::string applied_value;
    long long   enqueuedMs = 0;
    long long   finishedMs = 0;
};

std::mutex                g_statusMutex;
std::vector<StatusEntry>  g_status;
// P1-5: ring buffer bound (drop oldest done entries past this size).
static constexpr int      kMaxStatusEntries = 256;
// R6-014: O(1) command_id -> vector index. g_status stays the ordered vector
// (deterministic JSON / oldest-first eviction); this index makes statusUpsert and
// lookup O(1) instead of a linear scan over up to 256 entries on every enqueue
// and every drain transition. Indices are rebuilt whenever the front is erased.
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

// JSON-escape a small string into an existing ostringstream-style buffer.
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
    // R6-014: O(1) indexed lookup instead of a linear scan over g_status.
    auto it = g_statusIndex.find(e.command_id);
    if (it != g_statusIndex.end()) {
        g_status[it->second] = e;
        return;
    }
    g_status.push_back(e);
    g_statusIndex[e.command_id] = g_status.size() - 1;
    // Evict oldest (front) when over capacity.
    while ((int)g_status.size() > kMaxStatusEntries) {
        g_status.erase(g_status.begin());
        // erase(begin()) shifts every index by one — rebuild the index.
        rebuildStatusIndex();
    }
}

// Count queued files on disk (cmd-NNNNNN, regular files only). Best effort.
int countQueuedFiles() {
    DIR* d = opendir(commandsDir().c_str());
    if (!d) return 0;
    int n = 0;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name.rfind("cmd-", 0) != 0) continue;
        // Only fully numeric suffix (skip .tmp staging files).
        bool numeric = name.size() > 4;
        for (size_t i = 4; i < name.size() && numeric; ++i) {
            if (name[i] < '0' || name[i] > '9') numeric = false;
        }
        if (numeric) ++n;
    }
    closedir(d);
    return n;
}

// Parse "cmd-NNNNNN" -> number; -1 if not a well-formed queue filename.
long long parseCmdNumber(const std::string& name) {
    if (name.rfind("cmd-", 0) != 0) return -1;
    long long v = 0;
    for (size_t i = 4; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        int digit = name[i] - '0';
        // P2-#14: overflow check before v*10+digit.
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

// Monotonic id allocator. The counter is seeded from the on-disk max at first
// use so we never reuse a number that still has a queued file after a restart.
std::mutex       g_idMutex;
std::atomic<long long> s_nextId{0};
std::atomic<bool>      s_idSeeded{false};

// R6-013: in-memory queue depth. Previously every enqueue() called
// countQueuedFiles() which opendir()+readdir()'d the whole commands dir just to
// compare against kMaxQueueDepth. Now we maintain the depth in memory: seeded
// from disk on first use, incremented on enqueue, decremented whenever the drain
// unlinks a queued file.
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

    // MON-11 fix: quarantine non-idempotent leftover commands left by a previous
    // daemon crash. If the previous daemon died between executeControlLine() and
    // unlink(), the file is still on disk. On restart we would re-execute it.
    // State-setting commands (set_*, migrate=, game_add=) are idempotent and
    // safe to re-run; one-shot triggers (restart, clear_log, manual_unlock_watchdog,
    // shadow_dump=1, restored_after_crash_window) are not — replaying them would
    // restart the daemon twice, double-unlock the watchdog, or emit a spurious
    // diagnostic dump. Move those to .quarantined so they are never re-drained.
    std::string dir = commandsDir();
    DIR* d2 = opendir(dir.c_str());
    if (d2) {
        struct dirent* de;
        while ((de = readdir(d2)) != nullptr) {
            std::string name = de->d_name;
            if (parseCmdNumber(name) < 0) continue;  // only numeric .seq files
            std::string path = dir + "/" + name;
            std::string content = readFile(path.c_str());
            // trim
            while (!content.empty() &&
                   (content.back()=='\n'||content.back()=='\r'||content.back()==' '||content.back()=='\t'))
                content.pop_back();
            if (!commandLineIsReplaySafe(content)) {
                std::string qpath = path + ".quarantined";
                if (::rename(path.c_str(), qpath.c_str()) == 0) {
                    LOG_WARN("cmdq: quarantined leftover non-idempotent command '%s' ('%s')",
                             name.c_str(), content.c_str());
                    --depth;  // not going to be drained
                }
            }
        }
        closedir(d2);
    }
    s_queuedDepth.store(depth);
}

} // namespace

EnqueueResult commandQueueEnqueue(const std::string& line) {
    EnqueueResult res;

    // Sanitize the line the same way the legacy control pipe does.
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
    // F02[P3] defense-in-depth: the queue dir is 0700 root-only, so only root can
    // enqueue — but the drain path used to trust the file content and call
    // executeControlLine() directly, bypassing the HTTP dispatcher's auth gate.
    // We now apply the SAME character whitelist at ENQUEUE time that drain applies
    // (isValidControlChars): a line carrying shell metacharacters, NULs, or other
    // non-control-table bytes is rejected before it is ever written to the queue.
    // Trust boundary (documented, not enforced here): only the HMAC/session-authed
    // HTTP handler or root itself may call commandQueueEnqueue; a non-root writer
    // cannot, because the directory is 0700.
    if (!isValidControlChars(cmd)) {
        res.error_code = aatpe::ERR_INVALID_ARGUMENT;
        res.message = "command rejected: disallowed characters";
        LOG_WARN("cmdq: enqueue rejected line with disallowed characters");
        return res;
    }

    // P1-fix (issue 13): command dir must be 0700 (root-only), matching the
    // file_perm::kDirMode used everywhere else. The previous 0750 allowed
    // group read/execute, exposing the queued command names (which include
    // privileged operations like set_control_mode=active) to non-root group.
    mkdirIfNotExist(commandsDir().c_str(), 0700);

    std::lock_guard<std::mutex> lk(g_idMutex);
    seedIdIfNeeded();

    // R6-013: use the in-memory depth instead of opendir/readdir'ing the whole
    // commands directory on every enqueue.
    if (s_queuedDepth.load(std::memory_order_relaxed) >= kMaxQueueDepth) {
        res.error_code = aatpe::ERR_BUSY;
        res.message = "command queue full (max " + std::to_string(kMaxQueueDepth) + ")";
        LOG_WARN("cmdq: enqueue rejected, queue depth >= %d", kMaxQueueDepth);
        return res;
    }

    long long n = s_nextId.fetch_add(1);
    std::string id = formatCmdId(n);

    // Write to a staging file then atomic-rename into place. The staging name is
    // unique per (id,pid) so two producers never collide on the temp file.
    std::string finalPath = commandsDir() + "/" + id;
    std::string tmpPath   = finalPath + ".tmp." + std::to_string((long long)getpid());

    if (!writeFileAtomic(tmpPath.c_str(), cmd)) {
        res.error_code = aatpe::ERR_APPLY_FAILED;
        res.message = "cannot write command file";
        return res;
    }
    // P1-fix (issue 14): reserve the depth slot BEFORE the rename so that a
    // concurrent drain cannot read the file and decrement depth before we
    // increment it (which caused depth to drift negative). If rename fails,
    // roll back the reservation.
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

// A2-007: explicit non-blocking try-push. commandQueueEnqueue already short-circuits
// to ERR_BUSY immediately when s_queuedDepth >= kMaxQueueDepth (it never waits on a
// condition variable for a free slot), so this is a thin, self-documenting wrapper
// that callers on the HTTP I/O thread can use to guarantee "never block".
bool tryPush(const std::string& line, EnqueueResult& out) {
    out = commandQueueEnqueue(line);
    return out.ok;
}

int commandQueueDrain() {
    std::string dir = commandsDir();
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;

    // Collect + sort queued filenames so we execute in FIFO order.
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
        // Trim trailing newline/whitespace.
        while (!content.empty() &&
               (content.back()=='\n'||content.back()=='\r'||content.back()==' '||content.back()=='\t'))
            content.pop_back();
        if (content.empty()) {
            ::unlink(path.c_str());
            s_queuedDepth.fetch_sub(1, std::memory_order_relaxed);  // R6-013
            continue;
        }

        // Mark executing.
        {
            std::lock_guard<std::mutex> lk(g_statusMutex);
            auto idx = g_statusIndex.find(name);
            if (idx != g_statusIndex.end()) g_status[idx->second].state = "executing";
        }

        // P1-1: the queued bytes come from a file (root-writable dir, but we must
        // not trust them any more than a live control pipe). Re-run the SAME
        // character whitelist [a-z0-9_=.-] before executeControlLine so a crafted
        // command file cannot smuggle shell/JSON metacharacters through the queue.
        if (!isValidControlChars(content)) {
            LOG_WARN("cmdq: rejecting '%s' (invalid control chars)", name.c_str());
            StatusEntry e;
            e.command_id    = name;
            e.line          = content;
            e.state         = "rejected";
            // P2-15: use the unified error code instead of a hardcoded 400.
            e.error_code     = aatpe::ERR_INVALID_ARGUMENT;
            e.message       = "invalid characters (only [a-z0-9_=.-], <=256 bytes)";
            e.applied_value = "";
            e.finishedMs    = nowMs();
            statusUpsert(e);
            ::unlink(path.c_str());
            s_queuedDepth.fetch_sub(1, std::memory_order_relaxed);  // R6-013
            continue;
        }

        // Execute through the production command table.
        ControlResult cr = executeControlLine(content);
        LOG_INFO("cmdq: executed '%s' -> ok=%d err=%d", content.c_str(),
                 cr.success ? 1 : 0, cr.error_code);

        // Record result.
        StatusEntry e;
        e.command_id   = name;
        e.line         = content;
        e.state        = cr.success ? "success" : "failed";
        e.error_code   = cr.success ? 0 : cr.error_code;
        e.message      = cr.message;
        e.applied_value = cr.applied_value;
        e.finishedMs   = nowMs();
        statusUpsert(e);

        // Consume: delete the queue file.
        ::unlink(path.c_str());
        s_queuedDepth.fetch_sub(1, std::memory_order_relaxed);  // R6-013
        ++drained;
    }
    return drained;
}

std::string commandStatusJson(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    // R6-014: O(1) indexed lookup instead of scanning up to 256 entries.
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
    // P2-#15: escape id in the error JSON path too (the success path above
    // already uses appendJsonEscaped; this fallback was raw string concat).
    std::string out = "{";
    out += "\"command_id\":"; appendJsonEscaped(out, id); out += ",";
    out += "\"status\":\"unknown\",";
    out += "\"result\":{\"success\":false,\"error_code\":";
    out += std::to_string(aatpe::ERR_UNKNOWN_COMMAND);
    out += ",\"message\":\"unknown or expired command id\"}}";
    return out;
}

int commandQueueDepth() {
    // P1-fix (issue 13): 0700 to match the enqueue path.
    mkdirIfNotExist(commandsDir().c_str(), 0700);
    return countQueuedFiles();
}

// MON-11: classify a command line as safe to replay after a crash.
//
// A command is "replay-safe" iff executing it twice has the same net effect as
// executing it once. State-setting commands (set_*, migrate=, game_add=,
// reload_config, model_save=, model_reset=) fall in this bucket: they write a
// value or trigger an idempotent save/reset.
//
// One-shot triggers are NOT replay-safe: they cause an external side effect that
// cannot be undone and would be doubled on replay (restart the daemon twice,
// double-unlock the watchdog, emit a spurious diagnostic dump, re-mark the crash
// recovery window). We quarantine those at startup instead of re-draining.
bool commandLineIsReplaySafe(const std::string& line) {
    // Trim copy.
    std::string s = line;
    while (!s.empty() &&
           (s.back()=='\n'||s.back()=='\r'||s.back()==' '||s.back()=='\t'))
        s.pop_back();
    while (!s.empty() &&
           (s.front()==' '||s.front()=='\t'))
        s.erase(s.begin());
    if (s.empty()) return true;  // empty is a no-op, safe

    // Exact-match one-shot commands.
    // P3-3: "stop" sets g_shouldExit. If the daemon enqueued "stop" then crashed
    // before draining it, the next start would re-execute the leftover "stop" and
    // immediately kill the freshly-started daemon. Quarantine it on startup.
    // Normal-runtime "stop" (CLI -> enqueue -> drain -> executeControlLine) is
    // unaffected: this predicate is ONLY consulted by seedIdIfNeeded() at startup.
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

    // Prefix one-shot triggers (with '=' or alone).
    static const char* kNonReplayablePrefix[] = {
        "shadow_dump",       // shadow_dump=1 — triggers a diagnostic drain
        nullptr
    };
    for (int i = 0; kNonReplayablePrefix[i]; ++i) {
        const char* p = kNonReplayablePrefix[i];
        size_t plen = strlen(p);
        if (s.size() < plen) continue;
        if (s.compare(0, plen, p) != 0) continue;
        // Require end-of-string or '=' so "shadow_dump_foo" doesn't false-match.
        if (s.size() == plen || s[plen] == '=') return false;
    }

    // Everything else (set_*, migrate=, game_add=, game_remove=, reload_config,
    // model_reset=, model_save=, explain_tid=, etc.) is replay-safe.
    return true;
}

} // namespace aatpe_cmdq
