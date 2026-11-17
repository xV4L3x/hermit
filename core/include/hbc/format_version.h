#pragma once

// Per-version format dispatch. HBC versions differ in their function-header
// layout, section set, overflow encoding and per-function debug offsets. Instead
// of branching on the version inline across the parser/analysis/serializer, each
// version family provides a FormatVersion implementation and the core calls it
// at runtime via formatForVersion() - the same pattern getOpcodeTable() uses for
// the opcode tables. Adding a version means adding one implementation module and
// registering it, with no `if (version == ...)` spread through the code.

#include "hbc/format.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hbc {

// Version-neutral function-header fields produced by FormatVersion::readFuncHeader.
struct FuncHeaderFields {
    uint32_t offset              = 0;
    uint32_t bytecodeSizeInBytes = 0;
    uint32_t paramCount          = 0;   // includes the implicit `this`
    uint32_t frameSize           = 0;
    uint32_t functionName        = 0;   // string table index
    uint32_t infoOffset          = 0;   // start of the info section (EH + debug), 0 if none
    uint8_t  prohibitInvoke      = kProhibitInvokeNone;
    uint8_t  kind                = 0;   // 0 normal / 1 generator / 2 async (v98); 0 on v94-96
    bool     strictMode          = false;
    bool     hasExceptionHandler = false;
    bool     hasDebugInfo        = false;
    bool     overflowed          = false;
    bool     valid               = true;  // false: overflow offset out of file bounds
};

// Per-function debug offsets, read from the info section after the EH table.
struct DebugOffsetsFields {
    uint32_t debugOffset            = 0;  // source locations
    uint32_t scopeDescOffset        = 0;  // 0 on v98 (no scope descriptor section)
    uint32_t textifiedCalleesOffset = 0;  // 0 on v98 (no textified callees section)
};

// One section between the string storage and the bytecode.
struct MidSectionDesc {
    const char* name;
    uint32_t    size;
    uint32_t    align;
};

// Fields needed to (re)write one function header during serialization.
struct FuncHeaderWrite {
    uint32_t offset              = 0;
    uint32_t bytecodeSizeInBytes = 0;
    uint32_t infoOffset          = 0;  // v94-96 only (non-overflow info section)
    uint32_t origLargeOffset     = 0;  // overflow: large-header position in the ORIGINAL info block
    uint32_t newLargeOffset      = 0;  // overflow: new large-header offset stored in the small header
    uint32_t frameSize           = 0;
    bool     overflowed          = false;
    bool     writeFrameSize      = false;  // only when the body changed
};

// Everything that differs between HBC format versions. One singleton per version
// family; formatForVersion() selects it.
class FormatVersion {
public:
    virtual ~FormatVersion() = default;

    // --- sizes ---
    virtual uint32_t funcHeaderSize()  const = 0;  // small header (one table entry)
    virtual uint32_t largeHeaderSize() const = 0;  // overflow (large) header

    // --- parse ---
    // Reads function-table entry `index` from `fhBase`, resolving the overflow
    // large header (in `fileData`, length `fileLen`) when present.
    virtual FuncHeaderFields readFuncHeader(const uint8_t* fileData, uint32_t fileLen,
                                            const uint8_t* fhBase, uint32_t index) const = 0;

    // Overflow large-header offset packed into a small header (valid if overflowed).
    virtual uint32_t largeHeaderOffset(const uint8_t* smallHeader) const = 0;

    virtual uint32_t           debugOffsetsSize()  const = 0;
    virtual DebugOffsetsFields readDebugOffsets(const uint8_t* p) const = 0;

    // Object-literal layout for a NewObjectWithBuffer[Long] site. `operandU32`
    // holds the instruction's operand values; `shapeTable` is the object shape
    // table bytes (v98 only). Yields the key-buffer offset, property count and
    // value-buffer index. Returns false if the operands/shape are unusable.
    virtual bool objectLiteral(const std::vector<uint32_t>& operandU32,
                               const uint8_t* shapeTable, uint32_t shapeTableSize,
                               uint32_t& keyBufferOffset, uint32_t& count,
                               uint32_t& valueBufferIdx) const = 0;

    // The literal buffer sections, in order, with sizes from the header. v94-96:
    // array buffer, object key buffer, object value buffer. v98: literal value
    // buffer, object key buffer, object shape table.
    virtual std::vector<MidSectionDesc> literalBufferSections(const HBCFileHeader& h) const = 0;
    // Section name holding array literals / object-literal values (merged on v98).
    virtual const char* arrayBufferSection()    const = 0;
    virtual const char* objValueBufferSection() const = 0;
    // Section whose size feeds the third literal-buffer header word: the object
    // value buffer on v94-96, the object shape table on v98.
    virtual const char* objValueWordSection()   const = 0;

    // Operand index of the value-buffer index in a NewObjectWithBuffer[Long]
    // instruction (4 on v94-96, 2 on v98).
    virtual uint32_t objectValueOperandIndex() const = 0;
    // True if object literals reference their keys/count through the object shape
    // table (v98) rather than inline operands (v94-96). When true, object-literal
    // resize (which rewrites inline count/key operands) is not supported.
    virtual bool objectLiteralUsesShapeTable() const = 0;

    // Tail header words whose file offset is version-specific (v98 inserts a
    // numStringSwitchImms word that shifts them by 4 bytes). The common header
    // words (up to the literal buffers) keep their offsets and are read straight
    // off the shared struct.
    virtual uint32_t debugInfoOffset(const HBCFileHeader& h)     const = 0;
    virtual uint32_t cjsModuleCount(const HBCFileHeader& h)      const = 0;
    virtual uint32_t functionSourceCount(const HBCFileHeader& h) const = 0;
    virtual void     setDebugInfoOffset(HBCFileHeader& h, uint32_t v) const = 0;

    // --- serialize ---
    // Writes small header `index` into `fhBase` and, when overflowed, the large
    // header into `info` at `largeOffset - infoStart`. Throws on field overflow.
    virtual void writeFuncHeader(uint8_t* fhBase, uint32_t index,
                                 uint8_t* info, uint32_t infoStart, size_t infoSize,
                                 const FuncHeaderWrite& w) const = 0;

    // Writes the three literal-buffer header words from the serialized section
    // byte sizes (the object value size is the shape-table byte size on v98,
    // which this converts to the stored entry count).
    virtual void writeHeaderBufferWords(HBCFileHeader& h, uint32_t arrayOrLiteralValueBytes,
                                        uint32_t objKeyBytes, uint32_t objValueOrShapeBytes) const = 0;

    // Builds the small header bytes for a function appended by add_function: no
    // exception handler, no debug info, prohibitInvoke = None. `infoOffset` is
    // used only by v94-96 (v98 non-overflow functions have no info section).
    virtual std::vector<uint8_t> buildNewSmallHeader(uint32_t offset, uint32_t bodySize,
        uint32_t paramCount, uint32_t nameId, uint32_t frameSize, uint32_t infoOffset,
        bool strictMode) const = 0;
};

// The format module for `version`, or nullptr if unsupported.
const FormatVersion* formatForVersion(uint32_t version);

} // namespace hbc
