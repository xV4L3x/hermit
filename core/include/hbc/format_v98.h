#pragma once

// HBC v98 - Static Hermes, shipped by React Native 0.84+ as Hermes V1.
//
// IMPORTANT: "version 98" is not a single stable format. Static Hermes changed
// the layout several times within v98 without bumping the version. hermit
// targets the SHIPPED Hermes V1 (tag hermes-v250829098.0.x, what RN 0.84-0.86
// ship), NOT the first v98 commit. Relative to v94/96 the shipped v98 has:
//   - a new `numStringSwitchImms` header word (shifts segmentID / cjsModuleCount
//     / functionSourceCount / debugInfoOffset by 4 bytes);
//   - a 12-byte function header with a different bitfield layout (ParamCount 5,
//     FunctionName 8, LoopDepth, NumberRegCount, NonPtrRegCount, cache sizes);
//   - a 37-byte overflow header, overflow offset packed as
//     `offset(low 24) | functionName(high 8)`;
//   - one-word per-function debug offsets `{ sourceLocations }`;
//   - a merged literal value buffer + object shape table (as in the first v98);
//   - a renumbered opcode table (219 opcodes, incl. StringSwitchImm).
// Field widths are parsed bit-by-bit in format_v98.cpp (the Hermes BitField
// storage layout does not map to a simple C bitfield struct). Anchored to the
// Hermes source at tag hermes-v250829098.0.14.

#include "hbc/format.h"

namespace hbc {

// v98 file header: same 128 bytes, but a `numStringSwitchImms` word is inserted
// after objShapeTableCount, shifting the last four words. The buffer words keep
// their v94-96 offsets (arrayBufferSize == literalValueBufferSize,
// objValueBufferSize == objShapeTableCount), so only the tail differs.
#pragma pack(push, 1)
struct HBCFileHeader_v98 {
    uint64_t magic;                // 0x00
    uint32_t version;              // 0x08
    uint8_t  sourceHash[20];       // 0x0C
    uint32_t fileLength;           // 0x20
    uint32_t globalCodeIndex;      // 0x24
    uint32_t functionCount;        // 0x28
    uint32_t stringKindCount;      // 0x2C
    uint32_t identifierCount;      // 0x30
    uint32_t stringCount;          // 0x34
    uint32_t overflowStringCount;  // 0x38
    uint32_t stringStorageSize;    // 0x3C
    uint32_t bigIntCount;          // 0x40
    uint32_t bigIntStorageSize;    // 0x44
    uint32_t regExpCount;          // 0x48
    uint32_t regExpStorageSize;    // 0x4C
    uint32_t literalValueBufferSize; // 0x50
    uint32_t objKeyBufferSize;     // 0x54
    uint32_t objShapeTableCount;   // 0x58
    uint32_t numStringSwitchImms;  // 0x5C  <-- new in shipped v98
    uint32_t segmentID;            // 0x60
    uint32_t cjsModuleCount;       // 0x64
    uint32_t functionSourceCount;  // 0x68
    uint32_t debugInfoOffset;      // 0x6C
    uint8_t  options;              // 0x70
    uint8_t  padding[15];          // 0x71
};
static_assert(sizeof(HBCFileHeader_v98) == 128);
#pragma pack(pop)

// v98 function-kind (FunctionHeaderFlag::kind), same values and flag-byte layout
// as the first v98 (prohibitInvoke:2, strictMode:1, hasExceptionHandler:1,
// hasDebugInfo:1, overflowed:1, kind:2).
constexpr uint8_t kFuncKindNormal    = 0;
constexpr uint8_t kFuncKindGenerator = 1;
constexpr uint8_t kFuncKindAsync     = 2;

// v98 function header sizes and field widths (bit offsets within the 12-byte
// small header; parsed explicitly in format_v98.cpp).
constexpr uint32_t kV98SmallFuncHeaderSize = 12;
constexpr uint32_t kV98LargeFuncHeaderSize = 37;

#pragma pack(push, 1)
// New in v98: the object shape table (keys/count of object literals).
struct ShapeTableEntry {
    uint32_t keyBufferOffset;
    uint32_t numProps;
};
static_assert(sizeof(ShapeTableEntry) == 8);
#pragma pack(pop)

} // namespace hbc
