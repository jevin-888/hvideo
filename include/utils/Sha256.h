#pragma once
#include <cstddef>
#include <cstdint>

namespace hsvj {
// Shared streaming SHA-256 for license HMAC and verified file transfer.
struct Sha256Ctx {
    uint32_t state[8];
    uint64_t count;
    unsigned char buf[64];
};
void sha256Init(Sha256Ctx* context);
void sha256Update(Sha256Ctx* context, const unsigned char* bytes, size_t size);
void sha256Final(Sha256Ctx* context, unsigned char* digest); // 32 bytes
} // namespace hsvj
