#include "utils/Sha256.h"
#include <cstring>

namespace hsvj {
constexpr size_t SHA256_BLOCK_SIZE = 64;

static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}
static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static inline uint32_t sig0(uint32_t x) { return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22); }
static inline uint32_t sig1(uint32_t x) { return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25); }
static inline uint32_t gam0(uint32_t x) { return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3); }
static inline uint32_t gam1(uint32_t x) { return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10); }

static const uint32_t kSha256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

void sha256Init(Sha256Ctx* ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85; ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a; ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256Transform(Sha256Ctx* ctx, const unsigned char* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (static_cast<uint32_t>(block[i*4]) << 24) | (static_cast<uint32_t>(block[i*4+1]) << 16) |
               (static_cast<uint32_t>(block[i*4+2]) << 8) | static_cast<uint32_t>(block[i*4+3]);
    }
    for (int i = 16; i < 64; i++)
        w[i] = gam1(w[i-2]) + w[i-7] + gam0(w[i-15]) + w[i-16];

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + sig1(e) + ch(e,f,g) + kSha256[i] + w[i];
        uint32_t t2 = sig0(a) + maj(a,b,c);
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256Update(Sha256Ctx* ctx, const unsigned char* data, size_t len) {
    size_t idx = static_cast<size_t>(ctx->count & 63);
    ctx->count += len;
    while (len > 0) {
        size_t n = SHA256_BLOCK_SIZE - idx;
        if (n > len) n = len;
        std::memcpy(ctx->buf + idx, data, n);
        idx += n; data += n; len -= n;
        if (idx == SHA256_BLOCK_SIZE) {
            sha256Transform(ctx, ctx->buf);
            idx = 0;
        }
    }
}

void sha256Final(Sha256Ctx* ctx, unsigned char* out) {
    size_t idx = static_cast<size_t>(ctx->count & 63);
    ctx->buf[idx++] = 0x80;
    if (idx > 56) {
        while (idx < SHA256_BLOCK_SIZE) ctx->buf[idx++] = 0;
        sha256Transform(ctx, ctx->buf);
        idx = 0;
    }
    while (idx < 56) ctx->buf[idx++] = 0;
    uint64_t bits = ctx->count * 8;
    for (int i = 7; i >= 0; i--) {
        ctx->buf[56 + i] = static_cast<unsigned char>(bits);
        bits >>= 8;
    }
    sha256Transform(ctx, ctx->buf);
    for (int i = 0; i < 8; i++) {
        uint32_t s = ctx->state[i];
        out[i*4]   = static_cast<unsigned char>(s >> 24);
        out[i*4+1] = static_cast<unsigned char>(s >> 16);
        out[i*4+2] = static_cast<unsigned char>(s >> 8);
        out[i*4+3] = static_cast<unsigned char>(s);
    }
}

} // namespace hsvj
