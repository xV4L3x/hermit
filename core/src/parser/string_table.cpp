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
    bool     badOverflowIndex;
};

StringLocation locate(const SmallStringTableEntry& entry,
                      const OverflowStringTableEntry* ovfTable, uint32_t ovfCount) {
    if (entry.length == 255) {
        if (entry.offset >= ovfCount)
            return {0, 0, true, true};
        const auto& ovf = ovfTable[entry.offset];
        return {ovf.offset, ovf.length, true, false};
    }
    return {entry.offset, entry.length, false, false};
}

std::string describe(uint32_t index, const StringEntry& se, const StringLocation& loc,
                     uint32_t overflowCount, uint64_t storageSize, uint64_t fileSize) {
    const std::string who = "string table: string " + std::to_string(index);
    const uint64_t end = uint64_t(loc.offset) + uint64_t(loc.length) * (se.isUTF16 ? 2 : 1);
    switch (se.verdict) {
        case StringVerdict::BadOverflowIndex:
            return who + ": overflow index out of range (overflowStringCount = " +
                   std::to_string(overflowCount) + ")";
        case StringVerdict::InsideFile:
            return who + " spans bytes [" + std::to_string(loc.offset) + ", " + std::to_string(end) +
                   ") but string storage is " + std::to_string(storageSize) +
                   " bytes (the range stays inside the file)";
        default:
            return who + " spans bytes [" + std::to_string(loc.offset) + ", " + std::to_string(end) +
                   ") but string storage is " + std::to_string(storageSize) +
                   " bytes (the range leaves the file, " + std::to_string(fileSize) + " bytes)";
    }
}

// One StringEntry per string, with its verdict. Throws in Strict mode. The caller
// guarantees (validateStringSectionLayout) that the three tables and the storage lie
// inside the file, so an InsideStorage range is always readable.
std::vector<StringEntry> resolveEntries(const uint8_t* data, const HBCFileHeader& hdr,
                                        size_t fileSize, ValidationMode mode) {
    const auto off = stringSectionOffsets(hdr);
    const auto* strTable = reinterpret_cast<const SmallStringTableEntry*>(data + off.stringTable);
    const auto* ovfTable = reinterpret_cast<const OverflowStringTableEntry*>(data + off.overflowTable);
    const uint64_t storageSize = hdr.stringStorageSize;

    std::vector<StringEntry> result;
    result.reserve(hdr.stringCount);

    for (uint32_t i = 0; i < hdr.stringCount; ++i) {
        const auto& entry = strTable[i];
        const auto loc = locate(entry, ovfTable, hdr.overflowStringCount);

        StringEntry se{};
        se.storageOffset = off.stringStorage + loc.offset;
        se.charLength    = loc.length;
        // isUTF16 always comes from the small entry: the overflow table has no such field.
        se.isUTF16       = entry.isUTF16 != 0;
        const uint64_t bytes = uint64_t(loc.length) * (se.isUTF16 ? 2 : 1);
        se.byteLength    = bytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(bytes);
        se.isOverflow    = loc.isOverflow;
        se.misaligned    = se.isUTF16 && (loc.offset % 2) != 0;

        if (loc.badOverflowIndex)
            se.verdict = StringVerdict::BadOverflowIndex;
        else if (uint64_t(loc.offset) + bytes <= storageSize)
            se.verdict = StringVerdict::InsideStorage;
        else if (uint64_t(off.stringStorage) + loc.offset + bytes <= fileSize)
            se.verdict = StringVerdict::InsideFile;
        else
            se.verdict = StringVerdict::OutsideFile;

        if (se.verdict != StringVerdict::InsideStorage && mode == ValidationMode::Strict)
            throw InvalidStringEntryError(
                i, se.verdict, describe(i, se, loc, hdr.overflowStringCount, storageSize, fileSize));

        result.push_back(se);
    }
    return result;
}

} // namespace

// The tables are read through typed pointers: the structs are packed, the file
// is little-endian like the host, and `data` is page-aligned (mmap) with
// 4-aligned section offsets.
std::vector<std::string> parseStringTable(const uint8_t* data, const HBCFileHeader& hdr,
                                          size_t fileSize, ValidationMode mode) {
    const auto entries = resolveEntries(data, hdr, fileSize, mode);

    std::vector<std::string> result;
    result.reserve(entries.size());
    for (const auto& se : entries) {
        if (se.verdict == StringVerdict::BadOverflowIndex || se.verdict == StringVerdict::OutsideFile) {
            result.emplace_back();  // nothing to read
        } else if (se.isUTF16) {
            result.push_back(utf16LeToUtf8(data + se.storageOffset, se.charLength));
        } else {
            result.push_back(asciiToString(data + se.storageOffset, se.charLength));
        }
    }
    return result;
}

std::vector<StringEntry> parseStringEntries(const uint8_t* data,
                                            const HBCFileHeader& hdr,
                                            const std::vector<StringKind>& kinds,
                                            size_t fileSize, ValidationMode mode) {
    auto result = resolveEntries(data, hdr, fileSize, mode);
    for (size_t i = 0; i < result.size(); ++i)
        result[i].kind = i < kinds.size() ? kinds[i] : StringKind::String;
    return result;
}

} // namespace hbc
