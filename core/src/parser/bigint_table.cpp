#include "parser/bigint_table.h"

#include <algorithm>
#include <cstring>

namespace hbc {

namespace {

// Magnitude (little-endian bytes, no sign) to decimal, by repeated division
// of 32-bit limbs by 10^9.
std::string magnitudeToDecimal(const std::vector<uint8_t>& mag) {
    std::vector<uint32_t> limbs((mag.size() + 3) / 4, 0);
    for (size_t i = 0; i < mag.size(); ++i)
        limbs[i / 4] |= static_cast<uint32_t>(mag[i]) << (8 * (i % 4));
    while (!limbs.empty() && limbs.back() == 0) limbs.pop_back();
    if (limbs.empty()) return "0";

    std::vector<uint32_t> chunks;  // base 10^9, least significant first
    while (!limbs.empty()) {
        uint64_t rem = 0;
        for (size_t i = limbs.size(); i-- > 0;) {
            const uint64_t cur = (rem << 32) | limbs[i];
            limbs[i] = static_cast<uint32_t>(cur / 1000000000u);
            rem      = cur % 1000000000u;
        }
        chunks.push_back(static_cast<uint32_t>(rem));
        while (!limbs.empty() && limbs.back() == 0) limbs.pop_back();
    }

    std::string out = std::to_string(chunks.back());
    for (size_t i = chunks.size() - 1; i-- > 0;) {
        std::string part = std::to_string(chunks[i]);
        out.append(9 - part.size(), '0');
        out += part;
    }
    return out;
}

} // namespace

std::string bigIntBytesToDecimal(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return "0";
    const bool negative = (bytes.back() & 0x80) != 0;
    if (!negative) return magnitudeToDecimal(bytes);

    // |x| = ~x + 1 over the same width (the magnitude of the most negative
    // value, 2^(8n-1), fits in n bytes).
    std::vector<uint8_t> mag(bytes.size());
    unsigned carry = 1;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const unsigned v = static_cast<uint8_t>(~bytes[i]) + carry;
        mag[i] = static_cast<uint8_t>(v & 0xFF);
        carry  = v >> 8;
    }
    return "-" + magnitudeToDecimal(mag);
}

std::vector<BigIntInfo> parseBigIntTable(
    const uint8_t* fileData,
    uint32_t       tableOffset,
    uint32_t       count,
    uint32_t       storageOffset,
    uint32_t       storageSize)
{
    std::vector<BigIntInfo> result;
    if (count == 0) return result;
    result.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        BigIntInfo info;
        info.index = i;
        info.ok    = false;
        info.isNegative = false;

        if (tableOffset == 0) {
            info.error = "bigint table section missing";
            result.push_back(std::move(info));
            continue;
        }
        std::memcpy(&info.offset, fileData + tableOffset + i * 8,     4);
        std::memcpy(&info.length, fileData + tableOffset + i * 8 + 4, 4);

        // 64-bit sum: offset and length are untrusted.
        if (static_cast<uint64_t>(info.offset) + info.length > storageSize) {
            info.error = "entry [" + std::to_string(info.offset) + ", +" +
                         std::to_string(info.length) + ") is outside the " +
                         std::to_string(storageSize) + "-byte storage";
            result.push_back(std::move(info));
            continue;
        }

        const uint8_t* p = fileData + storageOffset + info.offset;
        info.bytes.assign(p, p + info.length);
        info.isNegative = !info.bytes.empty() && (info.bytes.back() & 0x80) != 0;
        info.decimal    = bigIntBytesToDecimal(info.bytes);
        info.ok         = true;
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace hbc
