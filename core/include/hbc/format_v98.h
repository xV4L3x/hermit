#pragma once

// HBC v98 - Static Hermes, shipped by React Native 0.84+ via Hermes V1.
//
// v98 lives on the Hermes `static_h` branch, a lineage that diverged from the
// v94/95/96 mainline (it is NOT a successor of v96). Its wire format differs in
// three ways that the shared `format.h` structs do not cover, so v98 gets its
// own structs here:
//   - 12-byte function headers (no `infoOffset`, no `environmentSize`; a 2-bit
//     `kind` flag is added). A function has an info section (large header + EH
//     table + debug offsets) iff its small header is `overflowed`.
//   - the array buffer and the object-value buffer are merged into one literal
//     value buffer, and a new object shape table is added.
//   - the file header keeps the same 128-byte layout, but two of its words are
//     repurposed (see HBCFileHeader_v98 below).
// See doc/hbc-format and the Static Hermes source at commit c00cc5759.

#include "hbc/format.h"

namespace hbc {

// Same 128 bytes as the shared HBCFileHeader. Two header words are repurposed
// for v98 (the byte offsets and the field widths are unchanged):
//   arrayBufferSize     holds literalValueBufferSize (array + object-value
//                       buffers are merged into one literal value buffer),
//   objValueBufferSize  holds objShapeTableCount (number of ShapeTableEntry).
using HBCFileHeader_v98 = HBCFileHeader;
static_assert(sizeof(HBCFileHeader_v98) == 128);

// Values of FunctionFlags_v98::kind. v98 marks generators/async in the header
// instead of using dedicated closure opcodes (which v98 removed).
constexpr uint8_t kFuncKindNormal    = 0;
constexpr uint8_t kFuncKindGenerator = 1;
constexpr uint8_t kFuncKindAsync     = 2;

#pragma pack(push, 1)

// v98 flags byte: a 2-bit `kind` field takes the place of the v94-96
// FunctionFlags `_reserved` bits. The lower six bits are identical.
struct FunctionFlags_v98 {
    uint8_t prohibitInvoke      : 2;  // kProhibitInvoke* (a 2-bit enum)
    uint8_t strictMode          : 1;
    uint8_t hasExceptionHandler : 1;
    uint8_t hasDebugInfo        : 1;
    uint8_t overflowed          : 1;  // -> real fields in a FunctionHeader_v98 at the large offset
    uint8_t kind                : 2;  // kFuncKind*
};
static_assert(sizeof(FunctionFlags_v98) == 1);

// 12 bytes per function (vs the 16-byte v94-96 SmallFuncHeader): `infoOffset`
// and `environmentSize` are gone, and `frameSize` is a full byte.
//   !overflowed: all fields are valid; the function has no info section.
//   overflowed:  the real fields live in a FunctionHeader_v98 at
//                largeOff = (functionName << 16) | offset, followed by the
//                function's info section (EH table + debug offsets).
struct SmallFuncHeader_v98 {
    // Word 1
    uint32_t offset     : 25;  // absolute offset of the bytecode
    uint32_t paramCount :  7;  // includes the implicit `this`
    // Word 2
    uint32_t bytecodeSizeInBytes : 15;
    uint32_t functionName        : 17;  // string table index
    // Word 3
    uint8_t  frameSize;              // registers (was 7-bit + infoOffset in v96)
    uint8_t  highestReadCacheIndex;
    uint8_t  highestWriteCacheIndex;
    FunctionFlags_v98 flags;
};
static_assert(sizeof(SmallFuncHeader_v98) == 12);

// Full-width (overflow) header, written at the large offset. The API widths
// come from Hermes's FUNC_HEADER_FIELDS: note `frameSize` is a full uint32 here
// even though the small header stores it in a byte. 23 bytes (packed).
struct FunctionHeader_v98 {
    uint32_t offset;
    uint32_t paramCount;
    uint32_t bytecodeSizeInBytes;
    uint32_t functionName;
    uint32_t frameSize;
    uint8_t  highestReadCacheIndex;
    uint8_t  highestWriteCacheIndex;
    FunctionFlags_v98 flags;  // overflowed = 0 in the large header
};
static_assert(sizeof(FunctionHeader_v98) == 23);

// New in v98: the object shape table. Object literals reference a shape (the
// set of keys) by index instead of carrying a key-buffer index per literal.
// Section element count is HBCFileHeader_v98's objShapeTableCount word.
struct ShapeTableEntry {
    uint32_t keyBufferOffset;  // byte offset into the object key buffer
    uint32_t numProps;         // number of properties in the shape
};
static_assert(sizeof(ShapeTableEntry) == 8);

#pragma pack(pop)

} // namespace hbc
