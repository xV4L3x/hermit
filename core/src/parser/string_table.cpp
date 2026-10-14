// The string section triple (see HBCFileHeader):
//   String Table          stringCount x SmallStringTableEntry { isUTF16, offset, length }
//                         offset points into the String Storage; length == 255 marks a
//                         string stored in the Overflow Table, and then `offset` is the
//                         index of its entry there.
//   Overflow String Table OverflowStringTableEntry { offset, length } for the strings whose
//                         length is >= 255 or whose offset doesn't fit in 23 bits; the
//                         entry of a string is the one at index `small.offset`.
//   String Storage        flat blob of UTF-16LE and CESU-8 strings, without separators.

#include "parser/string_table.h"
#include "parser/section_layout.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace hbc {

namespace {

// Appends a code point (U+0000..U+10FFFF) as UTF-8. Surrogate code points
// are encoded as 3 bytes, like any other BMP value.
void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

uint32_t combineSurrogates(uint16_t hi, uint16_t lo) {
    return 0x10000 + ((static_cast<uint32_t>(hi) - 0xD800) << 10) + (lo - 0xDC00);
}

// Converts UTF-16LE to UTF-8, recombining surrogate pairs into supplementary
// code points. Real RN bundles contain them (emoji), so BMP-only isn't enough.
// An unpaired surrogate is encoded as-is (3 bytes).
std::string utf16LeToUtf8(const uint8_t* src, uint32_t numCodeUnits) {
    std::string result;
    result.reserve(numCodeUnits);

    for (uint32_t i = 0; i < numCodeUnits; ++i) {
        uint16_t cu;
        std::memcpy(&cu, src + i * 2, 2);  // memcpy: src may be unaligned

        if (cu >= 0xD800 && cu <= 0xDBFF && i + 1 < numCodeUnits) {
            uint16_t lo;
            std::memcpy(&lo, src + (i + 1) * 2, 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                appendUtf8(result, combineSurrogates(cu, lo));
                ++i;
                continue;
            }
        }
        appendUtf8(result, cu);
    }
    return result;
}

// Converts CESU-8 to UTF-8. Hermes encodes narrow strings by UTF-8-encoding
// each UTF-16 code unit separately, so a supplementary character becomes two
// 3-byte surrogate sequences instead of one 4-byte sequence. Everything else
// is already valid UTF-8 and is copied verbatim; only surrogate pairs are
// recombined. (Strict decoders, pybind11's included, reject the CESU-8 form.)
// An unpaired surrogate is copied through unchanged; bindings.cpp's
// errors="replace" fallback covers it.
std::string cesu8ToUtf8(const uint8_t* src, uint32_t len) {
    std::string result;
    result.reserve(len);

    auto decode3 = [&](uint32_t at) {
        return static_cast<uint16_t>(((src[at] & 0x0F) << 12) |
                                     ((src[at + 1] & 0x3F) << 6) |
                                     (src[at + 2] & 0x3F));
    };

    uint32_t i = 0;
    while (i < len) {
        const uint8_t b0 = src[i];

        // Only a 3-byte sequence (lead 1110xxxx) can encode a surrogate half.
        if ((b0 & 0xF0) == 0xE0 && i + 6 <= len && (src[i + 3] & 0xF0) == 0xE0) {
            const uint16_t hi = decode3(i);
            const uint16_t lo = decode3(i + 3);
            if (hi >= 0xD800 && hi <= 0xDBFF && lo >= 0xDC00 && lo <= 0xDFFF) {
                appendUtf8(result, combineSurrogates(hi, lo));
                i += 6;
                continue;
            }
        }

        uint32_t seqLen = (b0 < 0x80)           ? 1
                        : ((b0 & 0xE0) == 0xC0) ? 2
                        : ((b0 & 0xF0) == 0xE0) ? 3
                                                : 1;
        if (seqLen > len - i) seqLen = len - i;
        result.append(reinterpret_cast<const char*>(src + i), seqLen);
        i += seqLen;
    }

    return result;
}

// Offset and length of a string. When the small entry has length 255 they are
// read from the overflow table at index `entry.offset`, as in Hermes'
// BytecodeDataProvider::getStringTableEntry.
struct StringLocation {
    uint32_t offset;
    uint32_t length;  // characters for UTF-16, bytes for CESU-8
    bool     isOverflow;
};

StringLocation locate(const SmallStringTableEntry& entry,
                      const OverflowStringTableEntry* ovfTable, uint32_t ovfCount) {
    if (entry.length == 255) {
        if (entry.offset >= ovfCount)
            throw std::runtime_error("string table: overflow index " + std::to_string(entry.offset) +
                                     " out of range (overflowStringCount = " +
                                     std::to_string(ovfCount) + ")");
        const auto& ovf = ovfTable[entry.offset];
        return {ovf.offset, ovf.length, true};
    }
    return {entry.offset, entry.length, false};
}

} // namespace

// The tables are read through typed pointers: the structs are packed, the file
// is little-endian like the host, and `data` is page-aligned (mmap) with
// 4-aligned section offsets.
std::vector<std::string> parseStringTable(const uint8_t* data, const HBCFileHeader& hdr) {
    const auto off = stringSectionOffsets(hdr);
    const auto* strTable = reinterpret_cast<const SmallStringTableEntry*>(data + off.stringTable);
    const auto* ovfTable = reinterpret_cast<const OverflowStringTableEntry*>(data + off.overflowTable);
    const uint8_t* strStorage = data + off.stringStorage;

    std::vector<std::string> result;
    result.reserve(hdr.stringCount);

    for (uint32_t i = 0; i < hdr.stringCount; ++i) {
        const auto& entry = strTable[i];
        const auto loc = locate(entry, ovfTable, hdr.overflowStringCount);

        // isUTF16 always comes from the small entry: the overflow table has no such field.
        if (entry.isUTF16)
            result.push_back(utf16LeToUtf8(strStorage + loc.offset, loc.length));
        else
            result.push_back(cesu8ToUtf8(strStorage + loc.offset, loc.length));
    }

    return result;
}

std::vector<StringEntry> parseStringEntries(const uint8_t* data,
                                            const HBCFileHeader& hdr,
                                            const std::vector<StringKind>& kinds) {
    const auto off = stringSectionOffsets(hdr);
    const auto* strTable = reinterpret_cast<const SmallStringTableEntry*>(data + off.stringTable);
    const auto* ovfTable = reinterpret_cast<const OverflowStringTableEntry*>(data + off.overflowTable);

    std::vector<StringEntry> result;
    result.reserve(hdr.stringCount);

    for (uint32_t i = 0; i < hdr.stringCount; ++i) {
        const auto& entry = strTable[i];
        const auto loc = locate(entry, ovfTable, hdr.overflowStringCount);

        StringEntry se;
        se.storageOffset = off.stringStorage + loc.offset;
        se.charLength    = loc.length;
        se.isUTF16       = entry.isUTF16 != 0;
        se.byteLength    = se.isUTF16 ? loc.length * 2 : loc.length;
        se.isOverflow    = loc.isOverflow;
        se.kind          = i < kinds.size() ? kinds[i] : StringKind::String;
        result.push_back(se);
    }

    return result;
}

} // namespace hbc
