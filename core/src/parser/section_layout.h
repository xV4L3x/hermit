// Private helpers shared by the parsers: HBC stores no section offsets (only
// counts and sizes), so each parser derives the ones it needs from the header.
// section_map() in parser.cpp applies the same rules to the whole file.
#pragma once

#include "hbc/format.h"

#include <cstdint>

namespace hbc {

// Rounds n up to a multiple of a (a must be a power of 2).
inline uint32_t alignUp(uint32_t n, uint32_t a) {
    return (n + a - 1) & ~(a - 1);
}

// Absolute offsets of the sections between the function headers and the
// string storage. Every section is 4-aligned, and the string and overflow
// tables are multiples of 4 bytes, so no padding sits between them.
struct StringSectionOffsets {
    uint32_t kindTable;
    uint32_t stringTable;
    uint32_t overflowTable;
    uint32_t stringStorage;
};

inline StringSectionOffsets stringSectionOffsets(const HBCFileHeader& hdr) {
    StringSectionOffsets o;
    o.kindTable = alignUp(sizeof(HBCFileHeader) + hdr.functionCount * sizeof(SmallFuncHeader), 4);
    const uint32_t identifierHashes = alignUp(o.kindTable + hdr.stringKindCount * 4, 4);
    o.stringTable   = alignUp(identifierHashes + hdr.identifierCount * 4, 4);
    o.overflowTable = o.stringTable   + hdr.stringCount         * sizeof(SmallStringTableEntry);
    o.stringStorage = o.overflowTable + hdr.overflowStringCount * sizeof(OverflowStringTableEntry);
    return o;
}

} // namespace hbc
