// The string section triple (see HBCFileHeader):
//   String Table          stringCount x SmallStringTableEntry { isUTF16, offset, length }
//                         offset points into the String Storage; length == 255 marks a
//                         string stored in the Overflow Table, and then `offset` is the
//                         index of its entry there.
//   Overflow String Table OverflowStringTableEntry { offset, length } for the strings whose
//                         length is >= 255 or whose offset doesn't fit in 23 bits; the
//                         entry of a string is the one at index `small.offset`.
//   String Storage        flat blob of ASCII strings (isUTF16 = 0, one byte per character) and
//                         UTF-16LE strings (isUTF16 = 1, two bytes per code unit), without
//                         separators. The Hermes compiler stores a string as ASCII when all its
//                         characters are ASCII and as UTF-16 otherwise (StringTableBuilder).

#include "parser/string_table.h"
#include "parser/section_layout.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace hbc {

namespace {

// Appends a code point (U+0000..U+10FFFF, not a surrogate) as UTF-8.
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

// Converts UTF-16LE to UTF-8. `numCodeUnits` counts 16-bit code units; a
// surrogate pair (high D800..DBFF followed by low DC00..DFFF) is one
// supplementary character (emoji in real RN bundles) and becomes one 4-byte
// UTF-8 sequence. An unpaired surrogate becomes U+FFFD, as in Hermes'
// convertUTF16ToUTF8WithReplacements (used by RuntimeModule::getStringFromStringID).
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
        appendUtf8(result, (cu >= 0xD800 && cu <= 0xDFFF) ? 0xFFFD : cu);
    }
    return result;
}

// Narrow entries (isUTF16 = 0) hold ASCII: the bytes are the characters, so the
// string is copied as it is, like Hermes' RuntimeModule::getStringFromStringID.
// (The compiler never writes a byte >= 0x80 there; such a byte would be copied
// unchanged and left to the caller's decoder.)
std::string asciiToString(const uint8_t* src, uint32_t len) {
    return std::string(reinterpret_cast<const char*>(src), len);
}

// Offset and length of a string. When the small entry has length 255 they are
// read from the overflow table at index `entry.offset`, as in Hermes'
// BytecodeDataProvider::getStringTableEntry.
struct StringLocation {
    uint32_t offset;
    uint32_t length;  // code units: 16-bit units for UTF-16, bytes (= characters) for ASCII
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
            result.push_back(asciiToString(strStorage + loc.offset, loc.length));
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
