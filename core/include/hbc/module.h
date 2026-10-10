// Structural serializer. A Module is a snapshot of an HBCFile that accepts
// edits which change sizes (a longer function body, a new string, a resized
// literal, a new function) and re-serializes the whole file: function table,
// section offsets, the exception handler tables of touched functions,
// header.fileLength / debugInfoOffset and the SHA-1 footer. The result is a
// valid .hbc that Hermes loads.
//
// Only ONE kind of edit can be queued per Module (two edits mean two commits,
// i.e. two Modules): serialize() throws if they are combined.
#pragma once

#include "hbc/hbc.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hbc {

// Abstract instruction for the assembler. For jumps, `jumpTargetIdx` is the
// INDEX of the target in the instruction list (not a byte offset) and
// `addrOperandPos` the position of the Addr operand to fill in.
struct AsmInstr {
    uint8_t             opcode         = 0;
    std::vector<double> operands;              // the Addr slot is a placeholder
    int32_t             jumpTargetIdx  = -1;   // -1 = not a jump
    int8_t              addrOperandPos = -1;   // -1 = no Addr operand
    // Byte offset the instruction had in the ORIGINAL body (function-relative),
    // -1 if it is new. Used to relocate the exception handler table.
    int32_t             origByteOffset = -1;
};

struct AssembledFunction {
    std::vector<uint8_t>  bytecode;
    std::vector<uint32_t> offsets;   // byte offset of every instruction
};

// Relocatable assembler: assigns offsets, promotes short jumps (Addr8) to
// their Long variant (Addr32) until every displacement fits, fills in the
// displacements and emits the bytes.
// Throws std::invalid_argument (version/opcode), std::out_of_range (target
// index), std::runtime_error (short jump with no Long variant, no convergence).
AssembledFunction assemble_function(uint32_t version, std::vector<AsmInstr> ir);

class Module {
public:
    // Snapshot of `file`. Handles overflowed functions and real bundles
    // (bodies out of index order, shared bytecode).
    static Module from(const HBCFile& file);

    uint32_t version() const { return version_; }
    uint32_t function_count() const { return static_cast<uint32_t>(funcs_.size()); }

    // Replaces the body of function `funcIdx`. `frameSize` is the new register
    // count (>= original, <= 127). With `editAll`, a function with shared
    // bytecode has the edit applied to every function of the group (their EH
    // tables are relocated at serialize time).
    // Throws std::runtime_error if: the original function contains SwitchImm or
    // SaveGenerator; `ir` contains a SwitchImm; `frameSize` doesn't fit 7 bits;
    // `frameSize` grows on a function with an exception handler (with editAll:
    // on any such member of the group); the bytecode is shared and
    // `editAll == false` (a private clone is not supported).
    void set_function_body(uint32_t funcIdx, std::vector<AsmInstr> ir,
                           uint32_t frameSize, bool editAll = false);

    // Appends a `String`-kind string. Returns its index in the serialized file.
    // It goes at the end of the string storage, so no existing entry moves.
    // Throws std::invalid_argument if `value` is empty or non-ASCII.
    uint32_t add_string(const std::string& value);

    // Replaces the content (and length) of string `idx`. `newBytes` is already
    // encoded by the caller (ASCII on 8-bit entries, UTF-16LE on isUTF16 ones).
    // The new bytes are appended to the string storage and only entry `idx` is
    // repointed, never an overwrite: a shared entry is detached automatically.
    // Throws std::out_of_range (bad idx); std::invalid_argument (odd size on a
    // UTF-16 entry); std::runtime_error if the entry isn't overflow and the new
    // length is >= 255 (promotion not supported), or if a string edit is already
    // queued. An entry that is already overflow stays overflow.
    void set_string_value(uint32_t idx, const std::string& newBytes);

    // Replaces the COMPLETE SLP payload of a NewArrayWithBuffer[Long] /
    // NewObjectWithBuffer[Long] site (`newBufferBytes` is the whole buffer
    // re-encoded by the Python side). It is appended to "array buffer" or
    // "object value buffer" (never "object key buffer": keys are read-only) and
    // only this instruction's bufferIdx operand is repointed, in place, same
    // width. count/sizeHint are unchanged.
    // Resolved IMMEDIATELY by re-disassembling the function, not in serialize().
    // Throws std::out_of_range (no instruction of the expected mnemonic at
    // funcIdx/instrOffset); std::runtime_error if the function has shared
    // bytecode, the new offset doesn't fit the operand (promotion to *Long not
    // supported), or another edit is already queued.
    void set_literal_value(uint32_t funcIdx, uint32_t instrOffset,
                           bool isObject, const std::string& newBufferBytes);

    // Adds (`isAdd`) or removes the LAST element (array) or key-value pair
    // (object) of a literal site. `newBufferBytes` is the COMPLETE value payload
    // with count +/- 1 elements, `newKeyBufferBytes` the complete key payload
    // (objects only). Both are appended to their sections; count, sizeHint (only
    // if it equalled count), bufferIdx and keyBufferIdx are rewritten. An
    // existing key is never touched.
    // Throws std::out_of_range (no such instruction, or `!isAdd` with count == 0);
    // std::runtime_error if the bytecode is shared, sizeHint != count in the
    // original (mixed literal + dynamic), the new count exceeds 65535 (no *Long
    // variant exists), an offset doesn't fit its operand, or another edit is queued.
    void resize_literal(uint32_t funcIdx, uint32_t instrOffset, bool isObject,
                        bool isAdd, const std::string& newBufferBytes,
                        const std::string& newKeyBufferBytes = std::string());

    // Appends a new function at the end of the function table and returns its
    // index (= current function_count()). `ir` is assembled in serialize();
    // `frameSize` must cover the registers used (<= 127, never an overflow
    // function). Metadata is fixed: paramCount = 1 (`this` only), functionName =
    // string 0 (cosmetic), strict mode, no exception handler, no debug info
    // (so its function-info section is empty).
    // Throws std::runtime_error if `ir` is empty, has an unknown opcode or a
    // SwitchImm, `frameSize` > 127, or another edit is already queued.
    uint32_t add_function(std::vector<AsmInstr> ir, uint32_t frameSize);

    // Rebuilds the complete file (header, tables, bytecode, info, debug, footer).
    std::vector<uint8_t> serialize();

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    struct FuncState {
        uint32_t                 origOffset      = 0;
        uint32_t                 origSize        = 0;
        uint32_t                 origInfoOffset  = 0;
        uint32_t                 origFrameSize   = 0;
        bool                     hasEH           = false;
        bool                     hasDebugInfo    = false;
        bool                     overflowed      = false;
        uint32_t                 origLargeOff    = 0;   // only if overflowed
        bool                     sharedBody      = false;
        std::vector<uint32_t>    shareGroup;            // indices sharing origOffset (self included)
        bool                     dirty           = false;
        bool                     editAll         = false;
        std::vector<AsmInstr>    newIr;
        uint32_t                 newFrameSize    = 0;
    };

    // A section between string storage and bytecode (array/object buffers,
    // bigint, regexp, cjs module table, function source table), re-laid-out
    // with its own `align`. `name` is the section_map() name.
    struct MidSection {
        std::string          name;
        uint32_t             align = 1;
        std::vector<uint8_t> bytes;
    };

    // The string sections as they will be serialized, after add_string /
    // set_string_value.
    struct StringSections {
        std::vector<uint8_t> kindTable, stringTable, overflowTable, storage;
        uint32_t stringCount = 0, kindCount = 0, overflowCount = 0;
    };
    StringSections buildStringSections() const;

    // A NewArray/ObjectWithBuffer instruction located in its function, with the
    // absolute byte position (in bytecodeBlob_) and width of each operand.
    struct LiteralTarget {
        Instruction           instr;
        std::vector<uint32_t> operandBytePos;
        std::vector<uint32_t> operandWidth;
    };
    // `who` prefixes error messages; `action` completes the shared-bytecode error;
    // `maxOperandIdx` is the highest operand the caller uses.
    LiteralTarget locateLiteralSite(const char* who, const char* action, uint32_t funcIdx,
                                    uint32_t instrOffset, bool isObject,
                                    uint32_t maxOperandIdx) const;

    // Offset a buffer appended to section `midName` will start at, checked
    // against the operand width.
    uint32_t newBufferOffset(const char* who, const char* midName, uint32_t operandWidth) const;

    uint32_t version_ = 0;

    std::vector<uint8_t>     header_;        // 128 bytes
    std::vector<uint8_t>     funcHeaders_;   // 16 * count (raw SmallFuncHeader)

    std::vector<uint8_t>     kindTable_;     // string kind table
    std::vector<uint8_t>     idHash_;        // identifier hash table, kept verbatim
    std::vector<uint8_t>     stringTable_;
    std::vector<uint8_t>     overflowTable_;
    std::vector<uint8_t>     stringStorage_;
    std::vector<MidSection>  midSections_;

    std::vector<uint8_t>     bytecodeBlob_;  // all function bodies, verbatim, from blobStart_
    std::vector<uint8_t>     infoBlob_;      // function-info sections (overflow FunctionHeader + EH + debug)
    std::vector<uint8_t>     debugBlob_;     // debug info section

    uint32_t funcTableEnd_       = 0;   // 128 + count*16 (start of the kind table)
    uint32_t blobStart_          = 0;   // lowest function offset (NOT funcs_[0].origOffset)
    uint32_t oldInfoStart_       = 0;
    uint32_t oldDebugInfoOffset_ = 0;
    uint32_t origStringCount_    = 0;

    // Queued edits. The setters resolve everything that depends on the original
    // file immediately (operand positions, overflow slot, ...), so serialize()
    // only does mechanical work.
    std::vector<std::string> pendingStrings_;  // add_string()

    struct PendingStringEdit {
        uint32_t    idx;
        std::string newBytes;    // already encoded
        bool        isUTF16;
        bool        wasOverflow;
        uint32_t    ovfSlot;     // valid only if wasOverflow
    };
    std::optional<PendingStringEdit> pendingStringEdit_;

    struct PendingLiteralEdit {
        uint32_t    operandBytePos;   // absolute in bytecodeBlob_
        uint32_t    operandWidth;     // 2 (UInt16) or 4 (UInt32)
        std::string midSectionName;   // "array buffer" | "object value buffer"
        std::string newBufferBytes;
        uint32_t    offRel;           // new value of the bufferIdx operand
    };
    std::optional<PendingLiteralEdit> pendingLiteralEdit_;

    struct PendingLiteralResize {
        uint32_t    countOperandBytePos;
        uint32_t    sizeHintOperandBytePos;
        uint32_t    newCount;                 // written into count and sizeHint

        uint32_t    valueOperandBytePos;
        uint32_t    valueOperandWidth;
        std::string valueMidSectionName;      // "array buffer" | "object value buffer"
        std::string newBufferBytes;
        uint32_t    valueOffRel;

        bool        isObject;
        uint32_t    keyOperandBytePos;        // objects only
        uint32_t    keyOperandWidth;
        std::string newKeyBufferBytes;
        uint32_t    keyOffRel;
    };
    std::optional<PendingLiteralResize> pendingLiteralResize_;

    struct PendingFunction {
        std::vector<AsmInstr> ir;
        uint32_t              frameSize      = 0;
        uint32_t              paramCount     = 1;
        uint32_t              functionNameId = 0;
        bool                  strictMode     = true;
    };
    std::optional<PendingFunction> pendingNewFunction_;

    std::vector<FuncState>    funcs_;
    std::vector<std::string>  warnings_;
};

}  // namespace hbc
