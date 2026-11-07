#include "util/slp.h"

#include <cstring>

namespace hbc {

std::optional<std::vector<SLPElement>> decodeSLP(
    const std::string& buf, uint32_t start, uint32_t count)
{
    std::vector<SLPElement> out;
    // A corrupt count must not cause a huge allocation: each run header is at
    // least one byte and yields at most 4095 elements, so the buffer size bounds
    // the number of elements a valid buffer can hold.
    if (count > buf.size() * 4096ull) return std::nullopt;
    out.reserve(count);

    const auto* data = reinterpret_cast<const uint8_t*>(buf.data());
    const size_t n = buf.size();
    size_t pos = start;

    while (out.size() < count) {
        if (pos >= n) return std::nullopt;
        const uint8_t tagByte = data[pos];
        const auto tag = static_cast<SLPTag>((tagByte >> 4) & 0x7);
        uint32_t runLen;
        if (tagByte >> 7) {
            if (pos + 1 >= n) return std::nullopt;
            runLen = (static_cast<uint32_t>(tagByte & 0x0F) << 8) | data[pos + 1];
            pos += 2;
        } else {
            runLen = tagByte & 0x0F;
            pos += 1;
        }

        uint32_t width = 0;
        switch (tag) {
            case SLPTag::Null: case SLPTag::True: case SLPTag::False: width = 0; break;
            case SLPTag::Number:                                      width = 8; break;
            case SLPTag::LongString: case SLPTag::Integer:            width = 4; break;
            case SLPTag::ShortString:                                 width = 2; break;
            case SLPTag::ByteString:                                  width = 1; break;
        }

        for (uint32_t i = 0; i < runLen && out.size() < count; i++) {
            if (pos + width > n) return std::nullopt;
            SLPElement e;
            e.tag = tag;
            switch (tag) {
                case SLPTag::Number:
                    std::memcpy(&e.number, data + pos, 8);
                    break;
                case SLPTag::Integer:
                    std::memcpy(&e.integer, data + pos, 4);
                    break;
                case SLPTag::LongString: case SLPTag::ShortString: case SLPTag::ByteString:
                    std::memcpy(&e.stringIndex, data + pos, width);  // zero-extended
                    break;
                default:
                    break;
            }
            out.push_back(e);
            pos += width;
        }
    }
    return out;
}

} // namespace hbc
