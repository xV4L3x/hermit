// Decoder for SerializedLiteral (SLP) buffers: the array buffer, the object key
// buffer and the object value buffer. Python twin: cli/literals.py (which also
// tracks byte offsets and encodes; this one only reads).
//
// A buffer is a sequence of typed RLE runs. A tag byte [ext:1][tag:3][len:4]
// introduces `len` consecutive elements of the same tag (up to 4095 when the
// extension bit is set, the length then being [len:4] << 8 | next byte, else 15),
// each followed by its value bytes (little-endian, width fixed by the tag).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hbc {

enum class SLPTag : uint8_t {
    Null = 0, True = 1, False = 2, Number = 3,
    LongString = 4, ShortString = 5, ByteString = 6, Integer = 7,
};

struct SLPElement {
    SLPTag   tag;
    uint32_t stringIndex = 0;  // String*: index in the String Table
    int32_t  integer     = 0;  // Integer
    double   number      = 0;  // Number
};

inline bool isSLPStringTag(SLPTag t) {
    return t == SLPTag::LongString || t == SLPTag::ShortString || t == SLPTag::ByteString;
}

// Decodes `count` elements of `buf` starting at byte offset `start`. nullopt if
// the buffer ends first (truncated, or a wrong offset). Never reads out of bounds.
std::optional<std::vector<SLPElement>> decodeSLP(
    const std::string& buf, uint32_t start, uint32_t count);

} // namespace hbc
