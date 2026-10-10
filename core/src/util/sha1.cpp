// SHA-1 one-shot (public domain, after Steve Reid / RFC 3174).

#include "util/sha1.h"

#include <cstring>

namespace hbc {
namespace {

inline uint32_t rol(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

void transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(buffer[i * 4]) << 24) | (uint32_t(buffer[i * 4 + 1]) << 16) |
               (uint32_t(buffer[i * 4 + 2]) << 8) | uint32_t(buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);              k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                       k = 0xCA62C1D6; }

        uint32_t tmp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

}  // namespace

std::array<uint8_t, 20> sha1(const uint8_t* data, std::size_t len) {
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    std::size_t i = 0;
    for (; i + 64 <= len; i += 64)
        transform(state, data + i);

    // Padding: 0x80, zeros, then the bit length as a 64-bit big-endian value.
    uint8_t buffer[64];
    std::size_t rem = len - i;
    std::memcpy(buffer, data + i, rem);
    buffer[rem++] = 0x80;
    if (rem > 56) {
        std::memset(buffer + rem, 0, 64 - rem);
        transform(state, buffer);
        rem = 0;
    }
    std::memset(buffer + rem, 0, 56 - rem);
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int b = 0; b < 8; ++b)
        buffer[56 + b] = uint8_t(bits >> (56 - 8 * b));
    transform(state, buffer);

    std::array<uint8_t, 20> digest{};
    for (int j = 0; j < 20; ++j)
        digest[j] = uint8_t(state[j >> 2] >> (24 - 8 * (j & 3)));
    return digest;
}

}  // namespace hbc
