#include "integrity.h"
#include "aatped.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <vector>
#include <atomic>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

namespace aatpe {
namespace integrity {

void sha256Init(Sha256Ctx* ctx) {
    aatpe_crypto::sha256Init(&ctx->inner);
}

void sha256Update(Sha256Ctx* ctx, const uint8_t* data, size_t len) {
    aatpe_crypto::sha256Update(&ctx->inner, data, len);
}

void sha256Final(Sha256Ctx* ctx, uint8_t out[32]) {
    aatpe_crypto::sha256Final(&ctx->inner, out);
}

void sha256OneShot(const uint8_t* data, size_t len, uint8_t out[32]) {
    aatpe_crypto::sha256(data, len, out);
}

std::string toHexStr(const uint8_t* digest, size_t len) {
    static const char hexChars[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        result += hexChars[(digest[i] >> 4) & 0xF];
        result += hexChars[digest[i] & 0xF];
    }
    return result;
}

bool hexEqual(const std::string& a, const std::string& b) {
    return aatpe_crypto::constantTimeHexEqual(a, b);
}

static const char* kCriticalRelPaths[] = {
    "system/bin/aatped",
    "system/bin/aatpe-cli",
    "system/bin/aatpe-watchdog",
    "system/bin/aatpe-probe",
    "service.sh",
    "post-fs-data.sh",
    "uninstall.sh",
    "module.prop",
    "webroot/index.html",
    "system/etc/aatpe/webui/index.html",
    "system/etc/aatpe/profiles/balanced.conf",
    "system/etc/aatpe/profiles/gaming.conf",
    "system/etc/aatpe/profiles/battery.conf",
    "system/etc/aatpe/profiles/thermal.conf",
    "system/etc/aatpe/ebpf/sched_events_embedded.h",
    "system/etc/aatpe/ebpf/sched_events.bpf.o",
    "system/etc/aatpe/rules/thread_roles.conf",
    nullptr
};

static const char* kUserConfigRelPaths[] = {
    "system/etc/aatpe/aatpe.conf",
    nullptr
};

static std::string missingFileSentinel() {
    static std::string s;
    if (s.empty()) {
        const char* sentinel = "AATPE_MISSING_FILE_v1";
        uint8_t d[32];
        sha256OneShot(reinterpret_cast<const uint8_t*>(sentinel),
                      strlen(sentinel), d);
        s = toHexStr(d, 32);
    }
    return s;
}

static std::atomic<int> g_lastVerdict{-1};
static std::string g_lastReport;
static std::mutex g_reportMutex;

static std::mutex g_rootMutex;
static std::string g_moduleRootOverride;

static IntegrityFailHook g_failHook = nullptr;

static IntegrityRestoreHook g_restoreHook = nullptr;
static std::atomic<int> g_consecutiveTamper{0};
static constexpr int kTamperLatchStrikes = 3;

static std::string g_injectTransientPath;
#ifdef AATPE_HOST_TEST
void integrityTestInjectTransient(const std::string& absPath) { g_injectTransientPath = absPath; }
void integrityTestClearInject() { g_injectTransientPath.clear(); }
#endif

static std::atomic<long> g_hashFileCalls{0};
#ifdef AATPE_HOST_TEST
long integrityTestHashFileCount() { return g_hashFileCalls.load(std::memory_order_relaxed); }
void integrityTestResetHashFileCount() { g_hashFileCalls.store(0, std::memory_order_relaxed); }
#endif

static std::string g_manifestPathOverride;
#ifdef AATPE_HOST_TEST
void setIntegrityManifestPathForTest(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_rootMutex);
    g_manifestPathOverride = path;
}
#endif

static void fireFailHook(const char* reason) {
    IntegrityFailHook h;
    {
        std::lock_guard<std::mutex> lk(g_reportMutex);
        h = g_failHook;
    }
    if (h) h(reason);
}

static void fireRestoreHook(const char* reason) {
    IntegrityRestoreHook h;
    {
        std::lock_guard<std::mutex> lk(g_reportMutex);
        h = g_restoreHook;
    }
    if (h) h(reason);
}

static bool readFileToString(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' '  || out.back() == '\t')) {
        out.pop_back();
    }
    return true;
}

static bool writeStringToFile(const std::string& path, const std::string& content) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t off = 0;
    size_t len = content.size();
    bool complete = true;
    while (off < len) {
        ssize_t w = write(fd, content.data() + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            complete = false;
            break;
        }
        if (w == 0) { complete = false; break; }
        off += (size_t)w;
    }
    if (complete && fsync(fd) != 0) complete = false;
    close(fd);
    return complete;
}

std::string resolveModuleRoot() {
    {
        std::lock_guard<std::mutex> lk(g_rootMutex);
        if (!g_moduleRootOverride.empty()) {
            std::string binPath = g_moduleRootOverride + "/system/bin";
            struct stat st;
            if (stat(binPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                return g_moduleRootOverride;
            }
            return "";
        }
    }
    const char* candidates[] = {
        "/data/adb/modules/aatpe",
        "/data/adb/ksu/modules/aatpe",
        "/data/adb/ap/modules/aatpe",
        "/data/adb/modules_update/aatpe",
        nullptr
    };
    for (int i = 0; candidates[i]; i++) {
        std::string binPath = std::string(candidates[i]) + "/system/bin";
        struct stat st;
        if (stat(binPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            return candidates[i];
        }
    }
    return "";
}

void setModuleRootOverride(const std::string& root) {
    std::lock_guard<std::mutex> lk(g_rootMutex);
    g_moduleRootOverride = root;
}

std::string sha256FileHex(const std::string& absPath) {
    int err;
    return hashFileHex(absPath, &err);
}

std::string hashFileHex(const std::string& absPath, int* errOut) {
    auto fail = [&](int e) -> std::string { if (errOut) *errOut = e; return std::string(); };
    g_hashFileCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_injectTransientPath.empty() && absPath == g_injectTransientPath) {
        return fail(EIO);
    }
    int fd = open(absPath.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return fail(errno);
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return fail(EIO);
    }

    Sha256Ctx ctx;
    sha256Init(&ctx);

    std::vector<uint8_t> buf(65536);
    ssize_t n;
    while (true) {
        n = read(fd, buf.data(), buf.size());
        if (n > 0) {
            sha256Update(&ctx, buf.data(), static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    int readErr = (n < 0) ? errno : 0;
    close(fd);
    if (n < 0) return fail(readErr);

    uint8_t digest[32];
    sha256Final(&ctx, digest);
    if (errOut) *errOut = 0;
    return toHexStr(digest, 32);
}

std::vector<FileEntry> enumerateCriticalFiles(const std::string& moduleRoot) {
    std::vector<FileEntry> out;
    if (moduleRoot.empty()) return out;

    const std::string sentinel = missingFileSentinel();

    for (int i = 0; kCriticalRelPaths[i] != nullptr; i++) {
        FileEntry e;
        e.relPath = kCriticalRelPaths[i];
        e.absPath = moduleRoot + "/" + e.relPath;

        struct stat st;
        int lstatRc = lstat(e.absPath.c_str(), &st);
        if (lstatRc != 0) {
            e.exists = false;
            e.readState = (errno == ENOENT) ? ReadState::DefiniteTamper
                                           : ReadState::Transient;
        } else if (!S_ISREG(st.st_mode)) {
            e.exists = false;
            e.readState = ReadState::DefiniteTamper;
        } else {
            int herr = 0;
            e.hexDigest = hashFileHex(e.absPath, &herr);
            if (!e.hexDigest.empty()) {
                e.exists = true;
                e.readState = ReadState::Clean;
            } else {
                e.exists = false;
                switch (herr) {
                    case EIO: case EINTR: case EAGAIN: case EBUSY:
                    case ENOMEM: case EACCES: case EPERM: case ETIMEDOUT:
                        e.readState = ReadState::Transient;
                        break;
                    default:
                        e.readState = ReadState::DefiniteTamper;
                        break;
                }
            }
        }

        if (e.readState != ReadState::Clean) {
            e.hexDigest = sentinel;
        }
        out.push_back(std::move(e));
    }

    for (int i = 0; kUserConfigRelPaths[i] != nullptr; i++) {
        std::string absPath = moduleRoot + "/" + kUserConfigRelPaths[i];
        struct stat st;
        bool exists = (lstat(absPath.c_str(), &st) == 0 && S_ISREG(st.st_mode));
        if (exists) {
#ifdef AATPE_DEBUG
            if (g_logLevel.load(std::memory_order_relaxed) >= 3) {
                std::string digest = sha256FileHex(absPath);
                LOG_DEBUG("integrity: user-config '%s' digest=%s (diagnostics only, not chained)",
                          kUserConfigRelPaths[i], digest.c_str());
            }
#else
            (void)absPath;
#endif
        } else {
#ifdef AATPE_DEBUG
            LOG_DEBUG("integrity: user-config '%s' absent (diagnostics only)", kUserConfigRelPaths[i]);
#endif
        }
    }
    return out;
}

std::string computeChainedManifest(const std::vector<FileEntry>& entries) {
    uint8_t cv[32];
    memset(cv, 0, sizeof(cv));

    for (const auto& e : entries) {
        uint8_t block[96];
        memcpy(block, cv, 32);
        if (e.hexDigest.size() == 64) {
            memcpy(block + 32, e.hexDigest.data(), 64);
        } else {
            memset(block + 32, 0, 64);
        }

        uint8_t next[32];
        sha256OneShot(block, sizeof(block), next);
        memcpy(cv, next, 32);
    }
    return toHexStr(cv, 32);
}

std::string manifestPath() {
    std::lock_guard<std::mutex> lk(g_rootMutex);
    if (!g_manifestPathOverride.empty()) return g_manifestPathOverride;
    return "/data/adb/aatpe/manifest.sha256";
}

std::string readStoredManifest() {
    std::string m;
    if (readFileToString(manifestPath(), m)) {
        return m;
    }
    return "";
}

std::vector<std::string> enumerateCriticalRelPaths() {
    std::vector<std::string> out;
    for (int i = 0; kCriticalRelPaths[i]; i++) out.emplace_back(kCriticalRelPaths[i]);
    return out;
}

bool criticalSetComplete(const std::string& moduleRoot, std::string& firstMissing) {
    auto entries = enumerateCriticalFiles(moduleRoot);
    for (const auto& e : entries) {
        if (!e.exists) {
            firstMissing = e.relPath;
            return false;
        }
    }
    return true;
}

bool generateAndStoreManifest() {
    std::string root = resolveModuleRoot();
    if (root.empty()) {
        LOG_ERROR("integrity: cannot resolve module root for manifest generation");
        return false;
    }
    return generateAndStoreManifestAt(root);
}

bool generateAndStoreManifestAt(const std::string& root) {
    if (root.empty()) {
        LOG_ERROR("integrity: cannot resolve module root for manifest generation");
        return false;
    }
    auto entries = enumerateCriticalFiles(root);
    if (entries.empty()) {
        LOG_ERROR("integrity: no critical files enumerated under %s", root.c_str());
        return false;
    }
    for (const auto& e : entries) {
        if (!e.exists) {
            LOG_ERROR("integrity: refusing to generate baseline — critical entry "
                      "is missing or not a regular file: %s (would otherwise bake the "
                      "missing-file sentinel into the trusted chain)", e.relPath.c_str());
            return false;
        }
    }
    std::string manifest = computeChainedManifest(entries);

    if (mkdir("/data/adb/aatpe", 0700) != 0 && errno != EEXIST) {
        LOG_ERROR("integrity: mkdir(/data/adb/aatpe, 0700) failed: errno=%d (%s); "
                  "manifest write below will likely fail",
                  errno, strerror(errno));
    }
    std::string mpath = manifestPath();
    if (!writeStringToFile(mpath, manifest)) {
        LOG_ERROR("integrity: failed to write manifest to %s", mpath.c_str());
        return false;
    }
    LOG_INFO("integrity: manifest generated (%zu files, chain=%s...)",
             entries.size(), manifest.substr(0, 16).c_str());
    return true;
}

static bool verifyCore(const std::string& root,
                       const std::string& storedManifest,
                       const std::vector<FileEntry>& entries);

bool verifyModuleIntegrity() {
    std::string root = resolveModuleRoot();
    if (root.empty()) {
        g_lastVerdict = 0;
        {
            std::lock_guard<std::mutex> lk(g_reportMutex);
            g_lastReport = "FATAL: cannot resolve module root";
        }
        writeFile((std::string(STATE_DIR) + "/integrity_status").c_str(),
                  std::string("FAILED"));
        LOG_ERROR("integrity: cannot resolve module root");
        fireFailHook("integrity: cannot resolve module root");
        return false;
    }

    std::string storedManifest = readStoredManifest();
    if (storedManifest.empty()) {
        g_lastVerdict = 0;
        {
            std::lock_guard<std::mutex> lk(g_reportMutex);
            g_lastReport = "FAILED: no stored integrity manifest at " + manifestPath() +
                           " (expected from trusted install; refusing to auto-bootstrap)";
        }
        writeFile((std::string(STATE_DIR) + "/integrity_status").c_str(),
                  std::string("FAILED"));
        LOG_ERROR("integrity: no stored integrity manifest; refusing to auto-bootstrap");
        fireFailHook("integrity: no stored manifest (refusing to auto-bootstrap)");
        return false;
    }

    auto entries = enumerateCriticalFiles(root);
    return verifyCore(root, storedManifest, std::move(entries));
}

bool verifyAgainstRootAndManifest(const std::string& root, const std::string& storedManifest) {
    auto entries = enumerateCriticalFiles(root);
    return verifyCore(root, storedManifest, entries);
}

static bool verifyCore(const std::string& root,
                       const std::string& storedManifest,
                       const std::vector<FileEntry>& entries) {
    bool anyTransient = false;
    bool anyDefiniteTamper = false;
    for (const auto& e : entries) {
        if (e.readState == ReadState::Transient) anyTransient = true;
        else if (e.readState == ReadState::DefiniteTamper) anyDefiniteTamper = true;
    }
    if (anyTransient) {
        LOG_WARN("integrity: transient read error on >=1 critical file; keeping "
                 "previous verdict (no latch, no hook)");
        return g_lastVerdict.load(std::memory_order_acquire) == 1;
    }

    std::string currentManifest = computeChainedManifest(entries);
    bool chainMatch = hexEqual(currentManifest, storedManifest);
    bool tamper = anyDefiniteTamper || !chainMatch;

    if (!tamper) {
        int prev = g_lastVerdict.exchange(1, std::memory_order_acq_rel);
        g_consecutiveTamper.store(0, std::memory_order_release);
        writeFile((std::string(STATE_DIR) + "/integrity_status").c_str(),
                  std::string("OK"));
        {
            std::lock_guard<std::mutex> lk(g_reportMutex);
            g_lastReport.clear();
        }
        if (prev == 0) {
            LOG_INFO("integrity: recovered from FAILED -> OK; releasing read-only latch");
            fireRestoreHook("integrity: recovered to OK");
        }
        LOG_INFO("integrity: OK (%zu files, chain=%s...)",
                 entries.size(), currentManifest.substr(0, 16).c_str());
        return true;
    }

    int strikes = g_consecutiveTamper.fetch_add(1, std::memory_order_acq_rel) + 1;
    bool latch = (strikes >= kTamperLatchStrikes);

    g_lastVerdict.store(0, std::memory_order_release);
    writeFile((std::string(STATE_DIR) + "/integrity_status").c_str(),
              std::string("FAILED"));

    if (!latch) {
        LOG_WARN("integrity: definite-tamper notice strike %d/%d "
                 "(privileged ops revoked; irreversible read-only latch will engage after %d strikes)",
                 strikes, kTamperLatchStrikes, kTamperLatchStrikes);
        return false;
    }

    std::ostringstream rep;
    rep << "INTEGRITY TAMPER DETECTED\n";
    rep << "module_root=" << root << "\n";
    rep << "stored_chain="  << storedManifest << "\n";
    rep << "current_chain=" << currentManifest << "\n";
    rep << "files_scanned="  << entries.size() << "\n";
    rep << "--- per-file digest ---\n";
    int tamperedCount = 0;
    for (const auto& e : entries) {
        bool fileTampered = (e.readState != ReadState::Clean);
        rep << e.relPath << " | "
            << (e.readState == ReadState::Clean ? "present" : "MISSING") << " | "
            << e.hexDigest;
        if (fileTampered) {
            rep << " | TAMPERED";
            tamperedCount++;
        }
        rep << "\n";
    }
    rep << "tampered_files=" << tamperedCount << "\n";
    {
        std::lock_guard<std::mutex> lk(g_reportMutex);
        g_lastReport = rep.str();
    }

    LOG_ERROR("integrity: TAMPER DETECTED stored=%s current=%s tampered=%d",
              storedManifest.substr(0, 16).c_str(),
              currentManifest.substr(0, 16).c_str(),
              tamperedCount);
    for (const auto& e : entries) {
        if (e.readState != ReadState::Clean) {
            LOG_ERROR("integrity: MISSING file: %s", e.relPath.c_str());
        }
    }
    fireFailHook("integrity: TAMPER DETECTED (chain mismatch, latched)");
    return false;
}

IntegrityVerdict verdict() {
    int v = g_lastVerdict.load(std::memory_order_acquire);
    if (v == 1) return IntegrityVerdict::Ok;
    if (v == 0) return IntegrityVerdict::Failed;
    return IntegrityVerdict::Unknown;
}

bool privilegedOpsAllowed() {
    return g_lastVerdict.load(std::memory_order_acquire) == 1;
}

void setIntegrityFailHook(IntegrityFailHook hook) {
    std::lock_guard<std::mutex> lk(g_reportMutex);
    g_failHook = hook;
}

void setIntegrityRestoreHook(IntegrityRestoreHook hook) {
    std::lock_guard<std::mutex> lk(g_reportMutex);
    g_restoreHook = hook;
}

bool runBootGate() {
    bool ok = verifyModuleIntegrity();
    return ok;
}

#ifdef AATPE_HOST_TEST
void setIntegrityVerdictForTest(IntegrityVerdict v) {
    g_lastVerdict.store(static_cast<int>(v), std::memory_order_release);
}
#endif

std::string getIntegrityReport() {
    std::lock_guard<std::mutex> lk(g_reportMutex);
    return g_lastReport;
}

int lastVerdict() {
    return g_lastVerdict.load(std::memory_order_relaxed);
}

static void stripTrailingCr(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                          s.back() == ' '  || s.back() == '\t')) {
        s.pop_back();
    }
}

static bool parseSnapshotKv(const std::string& kvFilePath,
                             std::string& outValueB64,
                             std::string& outStoredSha) {
    std::ifstream ifs(kvFilePath, std::ios::binary);
    if (!ifs.is_open()) return false;
    std::string line;
    outValueB64.clear();
    outStoredSha.clear();
    while (std::getline(ifs, line)) {
        if (line.rfind("value_b64=", 0) == 0) {
            outValueB64 = line.substr(10);
        } else if (line.rfind("sha256=", 0) == 0) {
            outStoredSha = line.substr(7);
        }
    }
    stripTrailingCr(outValueB64);
    stripTrailingCr(outStoredSha);
    return true;
}

std::string snapshotValueSha256(const std::string& kvFilePath) {
    std::string b64Value, storedSha;
    if (!parseSnapshotKv(kvFilePath, b64Value, storedSha)) return "";
    if (b64Value.empty()) return "";

    uint8_t d[32];
    sha256OneShot(reinterpret_cast<const uint8_t*>(b64Value.data()),
                  b64Value.size(), d);
    return toHexStr(d, 32);
}

bool verifySnapshotFile(const std::string& kvFilePath) {
    std::string b64Value, storedSha;
    if (!parseSnapshotKv(kvFilePath, b64Value, storedSha)) return false;
    if (b64Value.empty() || storedSha.empty()) return false;

    uint8_t d[32];
    sha256OneShot(reinterpret_cast<const uint8_t*>(b64Value.data()),
                  b64Value.size(), d);
    std::string actual = toHexStr(d, 32);

    return hexEqual(actual, storedSha);
}

}
}
