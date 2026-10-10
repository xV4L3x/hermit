// On-disk HBC structures. Layout is identical across v94, v95 and v96, so a
// single set of structs covers all supported versions; per-version differences
// live only in the opcode tables (see format/opcodes_vNN.cpp).
#pragma once

#include <array>
#include <cstdint>

namespace hbc {

// Little-endian bytes on disk: C6 1F BC 03 C1 03 19 1F.
constexpr uint64_t HBC_MAGIC = 0x1F1903C103BC1FC6ULL;

constexpr std::array<uint32_t, 3> SUPPORTED_VERSIONS = {94, 95, 96};

// Header byte at offset 0x6C. Usage: BytecodeOptions{h.options}.bits.hasAsync
// (bit order assumes GCC/Clang on little-endian).
union BytecodeOptions {
    struct Bits {
        uint8_t staticBuiltins               : 1;
        uint8_t cjsModulesStaticallyResolved : 1;
        uint8_t hasAsync                     : 1;
        uint8_t _reserved                    : 5;
    } bits;
    uint8_t raw;
    explicit BytecodeOptions(uint8_t r) : raw(r) {}
};
static_assert(sizeof(BytecodeOptions) == 1);

#pragma pack(push, 1)
struct HBCFileHeader {            // First 128 bytes of every HBC file.
    uint64_t magic;               // 0x00  HBC_MAGIC
    uint32_t version;             // 0x08  94, 95 or 96
    uint8_t  sourceHash[20];      // 0x0C  SHA-1 of the JS source (not verified by the runtime)
    uint32_t fileLength;          // 0x20  total size, footer included
    uint32_t globalCodeIndex;     // 0x24  function-table index of the top-level function
    uint32_t functionCount;       // 0x28
    uint32_t stringKindCount;     // 0x2C  RLE entries in the string kind table
    uint32_t identifierCount;     // 0x30  strings used as property keys
    uint32_t stringCount;         // 0x34
    uint32_t overflowStringCount; // 0x38  strings too long for SmallStringTableEntry
    uint32_t stringStorageSize;   // 0x3C  bytes of the raw string blob
    uint32_t bigIntCount;         // 0x40
    uint32_t bigIntStorageSize;   // 0x44
    uint32_t regExpCount;         // 0x48
    uint32_t regExpStorageSize;   // 0x4C
    uint32_t arrayBufferSize;     // 0x50  array literals buffer
    uint32_t objKeyBufferSize;    // 0x54  object literal keys
    uint32_t objValueBufferSize;  // 0x58  object literal values
    uint32_t segmentID;           // 0x5C  0 for monolithic bundles
    uint32_t cjsModuleCount;      // 0x60
    uint32_t functionSourceCount; // 0x64  preserved function sources
    uint32_t debugInfoOffset;     // 0x68  absolute offset of the debug info section
    uint8_t  options;             // 0x6C  see BytecodeOptions
    uint8_t  padding[19];         // 0x6D
};
static_assert(sizeof(HBCFileHeader) == 128);

// 4 bytes per string (bitfields assigned LSB-first on little-endian).
// length == 255 is the sentinel for "real offset/length in OverflowStringTableEntry".
struct SmallStringTableEntry {
    uint32_t isUTF16 : 1;
    uint32_t offset  : 23;  // into string storage
    uint32_t length  : 8;   // in characters
};
static_assert(sizeof(SmallStringTableEntry) == 4);

// Holds the real offset/length of strings with SmallStringTableEntry.length == 255.
struct OverflowStringTableEntry {
    uint32_t offset;
    uint32_t length;
};
static_assert(sizeof(OverflowStringTableEntry) == 8);

// Values of FunctionFlags::prohibitInvoke. The VM checks
// `prohibitInvoke == isConstructCall`, so 3 behaves like None.
constexpr uint8_t kProhibitInvokeCall      = 0;  // call prohibited: usable only with `new` (ES6 class constructor)
constexpr uint8_t kProhibitInvokeConstruct = 1;  // `new` prohibited (arrow, async, generator)
constexpr uint8_t kProhibitInvokeNone      = 2;  // both allowed (plain function)

// Flags byte shared by SmallFuncHeader and FunctionHeader.
struct FunctionFlags {
    uint8_t prohibitInvoke     : 2;  // kProhibitInvoke*, a 2-bit enum (not two flags)
    uint8_t strictMode         : 1;
    uint8_t hasExceptionHandler: 1;
    uint8_t hasDebugInfo       : 1;
    uint8_t overflowed         : 1;  // SmallFuncHeader only, always 0 in FunctionHeader:
                                     // the real fields are in a FunctionHeader at largeOff, where
                                     // largeOff = (offset & 0xFFFF) | ((infoOffset & 0xFFFF) << 16)
    uint8_t _reserved          : 2;
};
static_assert(sizeof(FunctionFlags) == 1);

// 16 bytes per function in the function table: three bitfield words plus
// four byte fields.
struct SmallFuncHeader {
    // Word 1
    uint32_t offset     : 25;  // absolute offset of the bytecode
    uint32_t paramCount :  7;  // includes the implicit `this`
    // Word 2
    uint32_t bytecodeSizeInBytes : 15;
    uint32_t functionName        : 17;  // string table index
    // Word 3
    uint32_t infoOffset : 25;  // absolute offset of the function-info section (EH table + debug info)
    uint32_t frameSize  :  7;  // registers

    uint8_t       environmentSize;
    uint8_t       highestReadCacheIndex;
    uint8_t       highestWriteCacheIndex;
    FunctionFlags flags;
};
static_assert(sizeof(SmallFuncHeader) == 16);

// Full-width version of SmallFuncHeader, for functions whose fields don't fit
// the reduced widths (e.g. environmentSize > 255).
// Hermes sets infoOffset = largeOff (self-reference): the real function-info
// section sits right after this struct, at largeOff + sizeof(FunctionHeader).
struct FunctionHeader {
    uint32_t offset;
    uint32_t paramCount;
    uint32_t bytecodeSizeInBytes;
    uint32_t functionName;
    uint32_t infoOffset;  // sentinel, see above
    uint32_t frameSize;
    uint32_t environmentSize;
    uint8_t  highestReadCacheIndex;
    uint8_t  highestWriteCacheIndex;
    FunctionFlags flags;  // overflowed = 0
    uint8_t  padding;
};
static_assert(sizeof(FunctionHeader) == 32);

// Exception handler table: this header followed by `count` entries, located at
// the function's infoOffset (largeOff + 32 if overflowed), not right after the
// bytecode.
struct ExceptionHandlerTableHeader { uint32_t count; };

// Offsets are relative to the start of the function's bytecode.
struct HBCExceptionHandlerInfo {
    uint32_t start;   // inclusive
    uint32_t end;     // exclusive
    uint32_t target;  // catch handler entry
};
static_assert(sizeof(HBCExceptionHandlerInfo) == 12);

// `offset` is relative to the start of BigInt storage.
struct BigIntTableEntry { uint32_t offset; uint32_t length; };
static_assert(sizeof(BigIntTableEntry) == 8);

// `offset` is relative to the start of RegExp storage.
struct RegExpTableEntry { uint32_t offset; uint32_t length; };
static_assert(sizeof(RegExpTableEntry) == 8);

#pragma pack(pop)

}
