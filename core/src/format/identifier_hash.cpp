#include "hbc/hbc.h"

#include <stdexcept>

namespace hbc {

uint32_t identifier_hash(const uint8_t* data, size_t byteLen, bool isUTF16) {
    if (isUTF16 && (byteLen % 2 != 0))
        throw std::invalid_argument("identifier_hash: odd byte length on a UTF-16 string");

    uint32_t h = 0;
    auto mix = [&h](uint32_t c) {
        h += c;
        h += h << 10;
        h ^= h >> 6;
    };
    if (isUTF16) {
        for (size_t i = 0; i < byteLen; i += 2)
            mix(static_cast<uint32_t>(data[i]) | (static_cast<uint32_t>(data[i + 1]) << 8));
    } else {
        for (size_t i = 0; i < byteLen; ++i)
            mix(data[i]);
    }
    return h;
}

} // namespace hbc
