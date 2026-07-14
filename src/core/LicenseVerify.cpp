/**
 * @file LicenseVerify.cpp（文件名）
 * @brief 授权文件 Base64 解码与 HMAC 签名校验（与 license-tool 生成格式一致）
 * 使用内嵌 SHA256/HMAC，不依赖 OpenSSL，避免 Android 预编译库 -fPIC 问题。
 */

#include "core/LicenseVerify.h"
#include "utils/Logger.h"
#include <json/value.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <limits>

namespace hsvj {

static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string licenseBase64Decode(const std::string& encoded) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++)
        T[static_cast<unsigned char>(kBase64Chars[i])] = i;

    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (c == '=') break;
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

namespace {

// 从 UTF-8 解码下一个码点，返回 (码点, 下一位置)；非法序列则返回 (0xFFFD, pos+1)
static bool decodeUtf8(const std::string& s, size_t pos, uint32_t* outCodepoint, size_t* outNext) {
    if (pos >= s.size()) return false;
    unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c <= 0x7F) {
        *outCodepoint = c;
        *outNext = pos + 1;
        return true;
    }
    if (c >= 0xC2 && c <= 0xDF && pos + 1 < s.size()) {
        unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
        if ((c1 & 0xC0) == 0x80) {
            *outCodepoint = ((c & 0x1F) << 6) | (c1 & 0x3F);
            *outNext = pos + 2;
            return true;
        }
    }
    if (c >= 0xE0 && c <= 0xEF && pos + 2 < s.size()) {
        unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
        unsigned char c2 = static_cast<unsigned char>(s[pos + 2]);
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
            *outCodepoint = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            *outNext = pos + 3;
            return true;
        }
    }
    if (c >= 0xF0 && c <= 0xF4 && pos + 3 < s.size()) {
        unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
        unsigned char c2 = static_cast<unsigned char>(s[pos + 2]);
        unsigned char c3 = static_cast<unsigned char>(s[pos + 3]);
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
            *outCodepoint = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            *outNext = pos + 4;
            return true;
        }
    }
    *outCodepoint = 0xFFFD;
    *outNext = pos + 1;
    return true;
}

// 与 license-tool sign.rs 一致：控制符与非 ASCII 统一 \uXXXX，避免设备 UTF-8 差异导致 HMAC 不匹配
std::string escapeJsonString(const std::string& s) {
    std::ostringstream oss;
    size_t pos = 0;
    while (pos < s.size()) {
        uint32_t u = 0;
        size_t next = 0;
        if (!decodeUtf8(s, pos, &u, &next)) break;
        pos = next;
        if (u == '"')
            oss << "\\\"";
        else if (u == '\\')
            oss << "\\\\";
        else if (u == '\b')
            oss << "\\b";
        else if (u == '\f')
            oss << "\\f";
        else if (u == '\n')
            oss << "\\n";
        else if (u == '\r')
            oss << "\\r";
        else if (u == '\t')
            oss << "\\t";
        else if (u <= 0x1F)
            oss << "\\u" << std::hex << std::setfill('0') << std::setw(4) << u;
        else if (u > 0x7F) {
            if (u <= 0xFFFF) {
                oss << "\\u" << std::hex << std::setfill('0') << std::setw(4) << u;
            } else {
                uint32_t v = u - 0x10000u;
                uint32_t hi = (v >> 10) + 0xD800u;
                uint32_t lo = (v & 0x3FFu) + 0xDC00u;
                oss << "\\u" << std::hex << std::setfill('0') << std::setw(4) << hi
                    << "\\u" << std::setfill('0') << std::setw(4) << lo;
            }
        } else
            oss << static_cast<char>(u);
    }
    return oss.str();
}

// 规范 JSON 字符串（对象按 key 字母序），与 Rust canonical_json_string 一致
std::string canonicalJsonString(const Json::Value& v) {
    switch (v.type()) {
    case Json::objectValue: {
        Json::Value::Members members = v.getMemberNames();
        std::sort(members.begin(), members.end());
        std::ostringstream oss;
        oss << "{";
        for (size_t i = 0; i < members.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "\"" << escapeJsonString(members[i]) << "\":" << canonicalJsonString(v[members[i]]);
        }
        oss << "}";
        return oss.str();
    }
    case Json::arrayValue: {
        std::ostringstream oss;
        oss << "[";
        for (Json::ArrayIndex i = 0; i < v.size(); ++i) {
            if (i > 0) oss << ",";
            oss << canonicalJsonString(v[i]);
        }
        oss << "]";
        return oss.str();
    }
    case Json::stringValue:
        return "\"" + escapeJsonString(v.asString()) + "\"";
    case Json::intValue:
        return std::to_string(v.asInt64());
    case Json::uintValue:
        return std::to_string(v.asUInt64());
    case Json::realValue: {
        double d = v.asDouble();
        // 与 Rust 一致：整数值输出为整数形式；用 floor 判断避免浮点误差导致走科学计数
        if (std::floor(d) == d && d >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
            d <= static_cast<double>(std::numeric_limits<int64_t>::max()))
            return std::to_string(static_cast<int64_t>(d));
        std::ostringstream oss;
        oss << std::fixed << d;
        return oss.str();
    }
    case Json::booleanValue:
        return v.asBool() ? "true" : "false";
    case Json::nullValue:
    default:
        return "null";
    }
}

// 十六进制解码
bool hexDecode(const std::string& hex, std::string& out) {
    out.clear();
    if (hex.size() % 2 != 0) return false;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int a = 0, b = 0;
        char c1 = hex[i], c2 = hex[i + 1];
        if (c1 >= '0' && c1 <= '9') a = c1 - '0';
        else if (c1 >= 'a' && c1 <= 'f') a = c1 - 'a' + 10;
        else if (c1 >= 'A' && c1 <= 'F') a = c1 - 'A' + 10;
        else return false;
        if (c2 >= '0' && c2 <= '9') b = c2 - '0';
        else if (c2 >= 'a' && c2 <= 'f') b = c2 - 'a' + 10;
        else if (c2 >= 'A' && c2 <= 'F') b = c2 - 'A' + 10;
        else return false;
        out.push_back(static_cast<char>((a << 4) + b));
    }
    return true;
}

// ----- 内嵌 SHA256 + HMAC-SHA256（不依赖 OpenSSL，避免 Android -fPIC 链接问题） -----
#define SHA256_BLOCK_SIZE 64
#define SHA256_OUTPUT_SIZE 32

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

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t count;
    unsigned char buf[SHA256_BLOCK_SIZE];
};

static void sha256Init(Sha256Ctx* ctx) {
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

static void sha256Update(Sha256Ctx* ctx, const unsigned char* data, size_t len) {
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

static void sha256Final(Sha256Ctx* ctx, unsigned char* out) {
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

static void hmacSha256(const unsigned char* key, size_t keyLen,
                       const unsigned char* msg, size_t msgLen,
                       unsigned char* out) {
    unsigned char k[SHA256_BLOCK_SIZE];
    if (keyLen > SHA256_BLOCK_SIZE) {
        Sha256Ctx ctx;
        sha256Init(&ctx);
        sha256Update(&ctx, key, keyLen);
        sha256Final(&ctx, k);
        keyLen = SHA256_OUTPUT_SIZE;
        std::memset(k + keyLen, 0, SHA256_BLOCK_SIZE - keyLen);
    } else {
        std::memcpy(k, key, keyLen);
        std::memset(k + keyLen, 0, SHA256_BLOCK_SIZE - keyLen);
    }
    unsigned char ipad[SHA256_BLOCK_SIZE], opad[SHA256_BLOCK_SIZE];
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Sha256Ctx ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, ipad, SHA256_BLOCK_SIZE);
    sha256Update(&ctx, msg, msgLen);
    unsigned char inner[SHA256_OUTPUT_SIZE];
    sha256Final(&ctx, inner);
    sha256Init(&ctx);
    sha256Update(&ctx, opad, SHA256_BLOCK_SIZE);
    sha256Update(&ctx, inner, SHA256_OUTPUT_SIZE);
    sha256Final(&ctx, out);
}

}  // 命名空间

static std::string bytesToHex(const unsigned char* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out += hex[(data[i] >> 4) & 0xF];
        out += hex[data[i] & 0xF];
    }
    return out;
}

bool verifyLicenseSignature(const Json::Value& root, const std::string& hmacKey) {
    if (!root.isObject() || !root.isMember("signature") || !root["signature"].isString())
        return false;

    std::string expectedHex = root["signature"].asString();
    std::string expectedBin;
    if (!hexDecode(expectedHex, expectedBin) || expectedBin.size() != 32)
        return false;

    Json::Value payload = root;
    payload.removeMember("signature");
    std::string canonical = canonicalJsonString(payload);

    unsigned char digest[32];
    hmacSha256(reinterpret_cast<const unsigned char*>(hmacKey.data()), hmacKey.size(),
               reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size(),
               digest);

    bool ok = (std::memcmp(digest, expectedBin.data(), 32) == 0);
    if (!ok) {
        std::string computedHex = bytesToHex(digest, 32);
        size_t prefixLen = (canonical.size() < 320u) ? canonical.size() : 320u;
        LOG_WARN("[License] 签名不匹配 expected=%s computed=%s canonical_len=%zu",
                 expectedHex.c_str(), computedHex.c_str(), canonical.size());
        LOG_WARN("[License] canonical 前 %zu 字符: %.*s", prefixLen, static_cast<int>(prefixLen), canonical.c_str());
        size_t hexLen = (canonical.size() < 80u) ? canonical.size() : 80u;
        std::string canonicalHex = bytesToHex(reinterpret_cast<const unsigned char*>(canonical.data()), hexLen);
        LOG_WARN("[License] canonical 前 %zu 字节 hex: %s", hexLen, canonicalHex.c_str());
    }
    return ok;
}

}  // 命名空间 hsvj
