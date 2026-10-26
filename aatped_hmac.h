// AATPE - HMAC key management / SHA256 signature sub-header
#ifndef AATPED_HMAC_H
#define AATPED_HMAC_H

#include <string>
#include <mutex>

// HMAC command verification
extern char g_hmacKey[128];          // 128 bytes for 508-bit entropy
// P3-19: g_hmacKeyStatus is a non-atomic std::string written under g_hmacMutex.
// Do NOT read it directly from another thread — use getHmacKeyStatus() which
// snapshots it under the mutex.
extern std::string g_hmacKeyStatus;  // generated/loaded/rotated
extern bool g_hmacActive;            // HMAC verification active
extern bool g_hmacRequired;          // synced from g_cfg.hmacRequired in loadConfig()

// P3-19: thread-safe accessor for g_hmacKeyStatus (snapshots under g_hmacMutex).
std::string getHmacKeyStatus(void);

// P1-4: mutex protecting g_hmacKey / g_hmacActive / g_hmacKeyLoaded /
// g_hmacKeyGeneratedAt / g_hmacReadonly. The main daemon loop calls
// checkHmacKeyRotation() which rewrites g_hmacKey; the HTTP server thread
// calls computeCommandHmac() / handleHmacKey() which read it. Without this
// mutex the reader can observe a half-written key during rotation.
extern std::mutex g_hmacMutex;

// P1-5: fail-closed read-only mode. Set true when initHmacKey() cannot
// generate or load a key from /dev/urandom. In this state /control write
// commands are rejected regardless of the hmacRequired config flag — we
// never leave the daemon fully open when authentication material could not
// be established.
extern bool g_hmacReadonly;
bool isHmacReadonly(void);

// G3-001: runtime hardening latch. Called by the integrity module's fail-hook
// when a re-verify detects tampering; makes the daemon reject all privileged
// /control writes immediately.
void hmacForceReadonly(const char* reason);

// ROUND-003: symmetric release of the read-only latch (on integrity recovery).
void hmacClearReadonly(const char* reason);

bool verifyCommandHmac(const char* command, const char* signature);
std::string computeCommandHmac(const char* command);

// Called from daemonInit / daemonLoop (non-static after split)
void initHmacKey(void);
void checkHmacKeyRotation(void);

// P2-9: shutdown-time key scrub. Zeroes g_hmacKey and munlock()s it; called by
// the daemon shutdown path so the long-term signing key does not persist in
// memory after the daemon stops.
void shutdownHmacKey(void);

#endif // AATPED_HMAC_H
