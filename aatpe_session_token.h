// AATPE - short-lived session token store (#61).
//
// A client that has already authenticated once with the long-term HMAC key can
// exchange that proof for a bearer session token (TTL 300s / 5 min). Subsequent
// /control commands carry ".stok=<hex>" instead of ".sig=<hmac>". The long-term
// HMAC path remains fully available (backward compatible); a session token is
// strictly an additive shortcut, never a replacement.
//
// Tokens are held only in memory (never written to disk), are 256-bit random,
// and are purged lazily on every access. This file has no dependency on the
// daemon loop so it can be unit-tested hermetically.
#ifndef AATPE_SESSION_TOKEN_H
#define AATPE_SESSION_TOKEN_H

#include <string>

// Issue a new session token. The caller MUST have already verified HMAC auth.
// The returned token is 64 lowercase hex chars (256 bits). Expires after
// AATPE_SESSION_TTL_SEC from issue time.
std::string aatpeSessionIssue(void);

// Verify a bearer session token. Returns true iff the token exists in the store
// and has not expired. Expired entries are purged as a side effect.
bool aatpeSessionVerify(const std::string& token);

// True iff at least one unexpired session exists. Used to publish the
// STATE_DIR/hmac_session_active flag (1/0) for the WebUI.
bool aatpeSessionAnyActive(void);

// Drop all sessions (called on daemon shutdown / forced re-auth).
void aatpeSessionPurgeAll(void);

#ifdef AATPE_HOST_TEST
// SEC-04 test seam: force the monotonic clock read to fail (on=true) or
// resume normal operation (on=false). Used by host unit tests to exercise the
// fail-closed path; not present in release builds.
void aatpeSessionSetClockFailForTest(bool on);
#endif

#endif // AATPE_SESSION_TOKEN_H
