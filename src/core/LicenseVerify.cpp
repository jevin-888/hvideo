/**
 * @file LicenseVerify.cpp（文件名）
 * @brief 授权文件 Base64 解码与 HMAC 签名校验（与 license-tool 生成格式一致）
 * 使用内嵌 SHA256/HMAC，不依赖 OpenSSL，避免 Android 预编译库 -fPIC 问题。
 */

#include "core/LicenseVerify.h"
#include "utils/Logger.h"
#include "utils/Sha256.h"
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
        // serde_json emits the shortest round-trippable decimal for non-integral
        // numbers; fixed six-place formatting would invalidate cloud signatures
        // such as arrears_amount=12.5.
        oss << std::setprecision(15) << std::defaultfloat << d;
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

// HMAC uses the shared SHA-256 implementation in utils/Sha256.cpp.
#define SHA256_BLOCK_SIZE 64
#define SHA256_OUTPUT_SIZE 32

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
