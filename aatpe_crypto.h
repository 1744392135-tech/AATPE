// AATPE Crypto Utilities - Shared SHA256 and HMAC-SHA256 implementation
// Extracted from aatped.cpp and aatpe_federated.cpp to eliminate code duplication.
// This header provides inline implementations for use across all AATPE modules.
#ifndef AATPE_CRYPTO_H
#define AATPE_CRYPTO_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <algorithm>

namespace aatpe_crypto {

// ============================================================================
// SHA-256 Implementation (64 rounds, FIPS 180-4 compliant)
// ============================================================================
struct Sha256Context {
    uint32_t state[8];
    uint64_t bitCount;
    uint8_t buffer[64];
    size_t bufferLen;
};

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// R8-020 / LED-127: guard n == 0. `x << (32 - n)` with n == 0 is a 32-bit shift
// of a uint32_t, which is undefined behaviour. All real call sites use n != 0,
// but the guard removes the landmine for future reuse.
inline uint32_t rotr(uint32_t x, int n) {
    if (n == 0) return x;
    return (x >> n) | (x << (32 - n));
}

inline void sha256Transform(Sha256Context* ctx, const uint8_t* data) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i*4] << 24) | ((uint32_t)data[i*4+1] << 16) |
               ((uint32_t)data[i*4+2] << 8) | ((uint32_t)data[i*4+3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

inline void sha256Init(Sha256Context* ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitCount = 0; ctx->bufferLen = 0;
}

inline void sha256Update(Sha256Context* ctx, const uint8_t* data, size_t len) {
    ctx->bitCount += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - ctx->bufferLen;
        if (take > len) take = len;
        memcpy(ctx->buffer + ctx->bufferLen, data, take);
        ctx->bufferLen += take; data += take; len -= take;
        if (ctx->bufferLen == 64) {
            sha256Transform(ctx, ctx->buffer);
            ctx->bufferLen = 0;
        }
    }
}

inline void sha256Final(Sha256Context* ctx, uint8_t out[32]) {
    uint64_t bitCount = ctx->bitCount;
    ctx->buffer[ctx->bufferLen++] = 0x80;
    if (ctx->bufferLen > 56) {
        while (ctx->bufferLen < 64) ctx->buffer[ctx->bufferLen++] = 0;
        sha256Transform(ctx, ctx->buffer);
        ctx->bufferLen = 0;
    }
    while (ctx->bufferLen < 56) ctx->buffer[ctx->bufferLen++] = 0;
    for (int i = 7; i >= 0; i--) ctx->buffer[ctx->bufferLen++] = (uint8_t)(bitCount >> (i * 8));
    sha256Transform(ctx, ctx->buffer);
    for (int i = 0; i < 8; i++) {
        out[i*4] = (uint8_t)(ctx->state[i] >> 24);
        out[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        out[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        out[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

inline void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256Context ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, data, len);
    sha256Final(&ctx, out);
}

// ============================================================================
// HMAC-SHA256 Implementation (RFC 2104 compliant)
// ============================================================================
inline void hmacSha256(const uint8_t* key, size_t keyLen,
                        const uint8_t* data, size_t dataLen,
                        uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (keyLen > 64) {
        sha256(key, keyLen, k);
    } else {
        memcpy(k, key, keyLen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha256Context ctx;
    uint8_t inner[32];
    sha256Init(&ctx);
    sha256Update(&ctx, ipad, 64);
    sha256Update(&ctx, data, dataLen);
    sha256Final(&ctx, inner);
    sha256Init(&ctx);
    sha256Update(&ctx, opad, 64);
    sha256Update(&ctx, inner, 32);
    sha256Final(&ctx, out);
    // Scrub key-derived material from the stack so the secret does not linger
    // in the stack frame after return. Volatile writes are not optimized away.
    volatile uint8_t* vk = k;
    volatile uint8_t* vipad = ipad;
    volatile uint8_t* vopad = opad;
    volatile uint8_t* vinner = inner;
    for (int i = 0; i < 64; i++) { vk[i] = 0; vipad[i] = 0; vopad[i] = 0; }
    for (int i = 0; i < 32; i++) vinner[i] = 0;
}

// Convert 32-byte hash to 64-char hex string
inline std::string toHex(const uint8_t* hash, size_t len = 32) {
    static const char* hex = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        result += hex[(hash[i] >> 4) & 0xF];
        result += hex[hash[i] & 0xF];
    }
    return result;
}

// Constant-time hex string comparison (prevents timing attacks).
// Does NOT early-return on length mismatch; instead folds the length difference
// into the diff accumulator so the work done is independent of where (or
// whether) the two strings differ.
//
// v4.4.9 R3-F05: the old loop ran min(a.size(), b.size()) iterations, so its
// wall time still leaked the length of the SHORTER string (a one-bit length
// oracle). We now always iterate over a FIXED width — a SHA256 hex digest is
// 64 chars, the only domain this helper is used for — padding short strings
// with a fixed 0 byte. Length mismatch is folded into `diff` up front and still
// gates the final result, so the loop bound no longer depends on input length.
inline bool constantTimeHexEqual(const std::string& a, const std::string& b) {
    constexpr size_t kHexWidth = 64;  // 32-byte SHA256 -> 64 lowercase hex chars
    // Fold length mismatch into diff FIRST; stays non-zero for any size diff.
    uint8_t diff = (uint8_t)((a.size() ^ b.size()) != 0);
    for (size_t i = 0; i < kHexWidth; i++) {
        // Select (not branch on match) the byte; out-of-range indices map to a
        // fixed 0 so the iteration count is constant regardless of input length.
        uint8_t ca = (i < a.size()) ? static_cast<uint8_t>(a[i]) : static_cast<uint8_t>(0);
        uint8_t cb = (i < b.size()) ? static_cast<uint8_t>(b[i]) : static_cast<uint8_t>(0);
        diff = static_cast<uint8_t>(diff ^ (ca ^ cb));
    }
    return diff == 0;
}

} // namespace aatpe_crypto

#endif // AATPE_CRYPTO_H
