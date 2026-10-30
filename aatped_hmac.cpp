#include "aatped.h"
#include "aatpe_crypto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

using namespace aatpe;

static void secureZero(void* p, size_t n) {
    volatile unsigned char* vp = (volatile unsigned char*)p;
    while (n--) *vp++ = 0;
}

char g_hmacKey[128] = {0};
std::string g_hmacKeyStatus = "uninitialized";
bool g_hmacActive = false;

bool g_hmacRequired = true;

std::mutex g_hmacMutex;

bool g_hmacReadonly = true;

#ifdef AATPE_HOST_TEST
bool g_forceHmacGenFail = false;
#endif

bool isHmacReadonly(void) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    return g_hmacReadonly;
}

void hmacForceReadonly(const char* reason) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    if (!g_hmacReadonly) {
        g_hmacReadonly = true;
        LOG_ERROR("HMAC: forced READ-ONLY by integrity/security gate: %s",
                  reason ? reason : "(unspecified)");
    }
}

void hmacClearReadonly(const char* reason) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    if (g_hmacReadonly) {
        g_hmacReadonly = false;
        LOG_INFO("HMAC: READ-ONLY released (integrity recovered): %s",
                 reason ? reason : "(unspecified)");
    }
}

std::string getHmacKeyStatus(void) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    return g_hmacKeyStatus;
}

static bool g_hmacKeyLoaded = false;
static time_t g_hmacKeyGeneratedAt = 0;
static const int HMAC_ROTATE_INTERVAL_SEC = 7 * 24 * 3600;

static bool generateHmacKey(char* outKey, size_t keyLen) {
    if (keyLen < 65) return false;
#ifdef AATPE_HOST_TEST
    if (g_forceHmacGenFail) return false;
#endif
    size_t keyBytes = keyLen - 1;
    FILE* fp = fopen("/dev/urandom", "r");
    if (!fp) return false;
    size_t read = fread(outKey, 1, keyBytes, fp);
    fclose(fp);
    if (read != keyBytes) return false;
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < keyBytes; i++) {
        outKey[i] = hex[(unsigned char)outKey[i] % 16];
    }
    outKey[keyBytes] = '\0';
    return true;
}

static std::string persistentHmacKeyPath() {
    return getModuleDir() + "/hmac_key";
}

static bool saveHmacKey(const char* key, time_t createdAt) {
    std::string persistent = persistentHmacKeyPath();
    const char* paths[] = {
        "/dev/aatpe/hmac_key",
        persistent.c_str(),
        nullptr
    };
    std::string payload = key;
    payload += "\n";
    payload += std::to_string((long long)createdAt);
    payload += "\n";
    bool okTmpfs = false, okPersistent = false;
    for (int i = 0; paths[i]; i++) {
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            size_t len = payload.size();
            size_t off = 0;
            bool complete = true;
            while (off < len) {
                ssize_t w = write(fd, payload.c_str() + off, len - off);
                if (w <= 0) {
                    if (errno == EINTR) continue;
                    complete = false;
                    break;
                }
                off += (size_t)w;
            }
            if (complete && fsync(fd) != 0) complete = false;
            close(fd);
            if (i == 0) okTmpfs = complete;
            else        okPersistent = complete;
        }
    }
    if (!okPersistent) {
        LOG_ERROR("HMAC key: persistent write FAILED (%s); key will not survive reboot",
                  persistent.c_str());
        return false;
    }
    if (!okTmpfs) {
        LOG_WARN("HMAC key: tmpfs write failed (persistent OK; current session still works)");
    }
    return true;
}

static bool loadHmacKey(char* outKey, size_t keyLen, time_t* outCreatedAt) {
    std::string persistent = persistentHmacKeyPath();
    const char* paths[] = {
        "/dev/aatpe/hmac_key",
        persistent.c_str(),
        nullptr
    };
    for (int i = 0; paths[i]; i++) {
        int fd = open(paths[i], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) continue;
        char buf[1024];
        ssize_t n;
        do {
            n = read(fd, buf, sizeof(buf) - 1);
        } while (n < 0 && errno == EINTR);
        struct stat st;
        time_t mtime = 0;
        if (fstat(fd, &st) == 0) mtime = st.st_mtime;
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string content(buf, n);
        size_t nl1 = content.find('\n');
        std::string keyStr;
        std::string tsStr;
        if (nl1 == std::string::npos) {
            keyStr = content;
        } else {
            keyStr = content.substr(0, nl1);
            size_t nl2 = content.find('\n', nl1 + 1);
            if (nl2 != std::string::npos) {
                tsStr = content.substr(nl1 + 1, nl2 - nl1 - 1);
            } else {
                tsStr = content.substr(nl1 + 1);
            }
        }
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' '||s.back()=='\t')) s.pop_back();
            while (!s.empty() && (s.front()==' '||s.front()=='\t'||s.front()=='\r')) s.erase(s.begin());
        };
        trim(keyStr);
        trim(tsStr);
        if (keyStr.size() < 64 || keyStr.size() >= keyLen) continue;
        memcpy(outKey, keyStr.c_str(), keyStr.size());
        outKey[keyStr.size()] = '\0';
        time_t created = 0;
        if (!tsStr.empty()) {
            try { created = (time_t)std::stoll(tsStr); } catch (...) { created = 0; }
        }
        if (created <= 0) created = mtime;
        if (outCreatedAt) *outCreatedAt = created;
        return true;
    }
    return false;
}

void initHmacKey(void) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    time_t loadedAt = 0;
    if (loadHmacKey(g_hmacKey, sizeof(g_hmacKey), &loadedAt)) {
        g_hmacKeyLoaded = true;
        g_hmacKeyGeneratedAt = (loadedAt > 0) ? loadedAt : time(nullptr);
        g_hmacActive = g_hmacRequired;
        g_hmacKeyStatus = "loaded";
        g_hmacReadonly = false;
        LOG_INFO("HMAC key: loaded from persistent storage (len=%zu, age=%ld days, active=%d)",
                 strlen(g_hmacKey),
                 (long)(time(nullptr) - g_hmacKeyGeneratedAt) / 86400,
                 (int)g_hmacActive);
    } else {
        if (generateHmacKey(g_hmacKey, sizeof(g_hmacKey))) {
            time_t now = time(nullptr);
            if (!saveHmacKey(g_hmacKey, now)) LOG_WARN("HMAC key: failed to persist to disk");
            g_hmacKeyLoaded = true;
            g_hmacKeyGeneratedAt = now;
            g_hmacActive = g_hmacRequired;
            g_hmacKeyStatus = "generated";
            g_hmacReadonly = false;
            LOG_INFO("HMAC key: generated new random key (len=%zu, active=%d)",
                     strlen(g_hmacKey), (int)g_hmacActive);
        } else {
            g_hmacKey[0] = '\0';
            g_hmacKeyLoaded = false;
            g_hmacActive = false;
            g_hmacKeyStatus = "disabled";
            g_hmacReadonly = true;
            LOG_ERROR("HMAC key: CRITICAL - failed to generate from /dev/urandom, "
                      "HMAC verification DISABLED, entering READ-ONLY mode "
                      "(all /control write commands rejected)");
        }
    }
    if (g_hmacKeyLoaded) {
        if (mlock(g_hmacKey, sizeof(g_hmacKey)) != 0) {
            LOG_WARN("HMAC key: mlock(g_hmacKey) failed (%s); key may be swappable",
                     aatpeErrStr(errno));
        }
    }
}

void checkHmacKeyRotation(void) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    if (!g_hmacKeyLoaded) return;
    time_t now = time(nullptr);
    if (now - g_hmacKeyGeneratedAt > HMAC_ROTATE_INTERVAL_SEC) {
        LOG_INFO("HMAC key: rotating (age=%ld days)", (long)(now - g_hmacKeyGeneratedAt) / 86400);
        char newKey[128];
        if (generateHmacKey(newKey, sizeof(newKey))) {
            if (!saveHmacKey(newKey, now)) {
                LOG_WARN("HMAC key: rotation FAILED to persist; keeping old key");
            } else {
                secureZero(g_hmacKey, sizeof(g_hmacKey));
                memcpy(g_hmacKey, newKey, sizeof(g_hmacKey));
                g_hmacKeyGeneratedAt = now;
                g_hmacKeyStatus = "rotated";
                LOG_INFO("HMAC key: rotated successfully (active=%d)", (int)g_hmacActive);
            }
            secureZero(newKey, sizeof(newKey));
        }
    }
}

void shutdownHmacKey(void) {
    std::lock_guard<std::mutex> lk(g_hmacMutex);
    secureZero(g_hmacKey, sizeof(g_hmacKey));
    munlock(g_hmacKey, sizeof(g_hmacKey));
    g_hmacKeyStatus = "shutdown";
    g_hmacActive = false;
}

std::string computeCommandHmac(const char* command) {
    if (!command) return "";
    char keySnap[128];
    {
        std::lock_guard<std::mutex> lk(g_hmacMutex);
        memcpy(keySnap, g_hmacKey, sizeof(g_hmacKey));
    }
    unsigned char out[32];
    aatpe_crypto::hmacSha256(reinterpret_cast<const uint8_t*>(keySnap), strlen(keySnap),
                reinterpret_cast<const uint8_t*>(command), strlen(command), out);
    secureZero(keySnap, sizeof(keySnap));
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + size_t(i)*2, 3, "%02x", out[i]);
    hex[64] = 0;
    return std::string(hex);
}

bool verifyCommandHmac(const char* command, const char* signature) {
    if (!command) return false;
    if (!signature || signature[0] == '\0') return false;
    std::string expected = computeCommandHmac(command);
    constexpr size_t kHexLen = 64;
    size_t siglen = 0;
    while (siglen < 256 && signature[siglen] != '\0') ++siglen;
    unsigned char diff = 0;
    if (siglen != kHexLen) diff |= 0xFF;
    for (size_t i = 0; i < kHexLen; i++) {
        char e = (i < expected.size()) ? expected[i] : 0;
        char s = (i < siglen) ? signature[i] : 0;
        diff |= (unsigned char)(e ^ s);
    }
    return diff == 0;
}
