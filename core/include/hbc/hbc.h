// Public API of libhbc: the only header clients (Python bindings,
// tests) need.
#pragma once

#include "hbc/format.h"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace hbc {

// =============================================================================
// Strings
// =============================================================================

//  Kind from the String Kind Table. Identifiers are property keys
// (GetByVal/PutByVal), Strings are literal values.
enum class StringKind : uint8_t {
    String     = 0,
    Identifier = 1,
};

// Raw location of a string in the file, for same-length in-place edits.
// HBCFile::string_entries() is parallel to strings() and stringKinds().
struct StringEntry {
    uint32_t   storageOffset;  // ABSOLUTE file offset of the first byte
    uint32_t   byteLength;     // charLength, or charLength * 2 if UTF-16
    uint32_t   charLength;     // resolved (overflow table included)
    bool       isUTF16;
    bool       isOverflow;     // SmallStringTableEntry.length == 255
    StringKind kind;
};

// =============================================================================
// Functions
// =============================================================================

// Offsets are relative to the start of the function's bytecode.
struct ExceptionHandlerInfo {
    uint32_t start;   // try-range start (inclusive)
    uint32_t end;     // try-range end (exclusive)
    uint32_t target;  // catch handler entry
};

struct FunctionInfo {
    uint32_t    index;                // position in the function table
    std::string name;                 // "anonymous_func_N" if anonymous
    uint32_t    offset;               // absolute offset of the bytecode
    uint32_t    bytecodeSizeInBytes;
    uint32_t    paramCount;           // includes the implicit `this`
    uint32_t    frameSize;            // registers
    uint32_t    infoOffset;           // absolute offset of the function-info section (EH + debug)
    bool        isStrictMode;
    bool        isAsync;
    bool        isGenerator;
    bool        hasExceptionHandler = false;
    bool        hasDebugInfo        = false;
    bool        prohibitCall        = false;  // usable only with `new` (class constructor)
    bool        prohibitConstruct   = false;  // `new` prohibited (arrow, async, generator)
    bool        overflowed          = false;  // real fields live in a FunctionHeader at largeOff
    bool        shared              = false;  // (offset, size) shared with at least one other function
    uint32_t    shareCount          = 1;      // functions (this one included) pointing to the same body
    uint32_t    debugOffset             = 0;  // into the source location data (relative to its start)
    uint32_t    scopeDescOffset         = 0;  // into the scope descriptor section (relative to its start)
    uint32_t    textifiedCalleesOffset  = 0;  // into the textified callees section (relative to its start)
    // Function Source Table: per-function `"show source"` / `"hide source"` /
    // `"sensitive"` directives (Hermes, same mechanism as `"use strict"`).
    bool        hasPreservedSource = false;  // function has any entry in the table
    bool        sourceIsHidden     = false;  // entry resolves to "" (hide/sensitive marker)
    std::string preservedSource;             // non-empty only for "show source"
    std::vector<ExceptionHandlerInfo> exceptionHandlers;
};

// =============================================================================
// Instructions and opcodes
// =============================================================================

struct Operand {
    enum class Type { Reg8, Reg32, UInt8, UInt16, UInt32, Imm32, Double, Addr8, Addr32, StringID };
    Type type;
    union {  // the member matching `type` is valid
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
        int32_t  i32;
        double   f64;
    };
    std::string resolvedString;  // StringID only: the resolved string value
};

struct Instruction {
    uint32_t             offset;    // from the start of the function's bytecode
    uint8_t              opcode;
    std::string          mnemonic;  // e.g. "LoadParam", "Ret"
    std::vector<Operand> operands;
    // SwitchImm only: jump table, one target per case, ordered by case value
    // (index i = case operands[3].u32 + i). Targets are offsets from the start
    // of the function's bytecode. Empty for other opcodes, or if the
    // disassembler had no file buffer (see disassemble()).
    std::vector<uint32_t> switchTargets;
};

// Static description of an opcode, exposed to the Python bytecode editor to
// validate edits and suggest compatible opcodes.
struct OpcodeDesc {
    uint8_t                    opcode;
    std::string                mnemonic;       // e.g. "LoadConstTrue"
    std::vector<Operand::Type> operands;       // PHYSICAL types (never StringID)
    uint8_t                    stringRefMask;  // bit N set: operand N is a string table index
    std::string                jumpKind;       // "None"|"Unconditional"|"Conditional"|"Switch"|"Return"|"Throw"
};

// Defined opcodes for an HBC version; empty if the version isn't supported.
std::vector<OpcodeDesc> opcode_table(uint32_t version);

// Size in bytes of a physical operand type.
uint32_t operand_size(Operand::Type t);

// Encodes one instruction (opcode byte + little-endian operands). Inverse of
// the disassembler's operand decoding. `operands` are numeric values in
// signature order, all passed as double (exact up to 2^53, enough for UInt32/Imm32).
// Throws std::invalid_argument on unknown opcode or wrong operand count,
// std::range_error if a value doesn't fit its operand's physical width.
std::vector<uint8_t> encode_instruction(uint32_t version, uint8_t opcode,
                                        const std::vector<double>& operands);

// =============================================================================
// CFG
// =============================================================================

enum class EdgeKind : uint8_t {
    FallThrough,
    Jump,              // unconditional (Jmp / JmpLong)
    CondTaken,
    CondFallThru,
    SwitchDefault,
    SwitchCase,
    BackEdge,          // loop edge, detected via DFS
    ExceptionHandler,  // edge to the catch block
};

struct CFGEdge {
    uint32_t targetId;  // id of the destination BasicBlock
    EdgeKind kind;
};

// Maximal instruction sequence with no branches in or out except at the ends.
struct BasicBlock {
    uint32_t id;
    uint32_t startOffset;  // in the function's bytecode
    uint32_t endOffset;    // exclusive
    std::vector<Instruction> instructions;
    std::vector<CFGEdge>     successors;
    std::vector<uint32_t>    predecessors;  // block ids
    bool isLoopHeader = false;              // target of a back-edge
};

struct CFGData {
    std::vector<BasicBlock> blocks;
    uint32_t entryId  = 0;
    bool     hasLoops = false;
};

// =============================================================================
// XREFs and call graph
// =============================================================================

struct XRefSite {
    uint32_t    functionIndex;
    uint32_t    instructionOffset;  // in the function's bytecode
    std::string mnemonic;           // e.g. "LoadConstString", "GetByIdShort"
};

// Reference sites of one string. string_xrefs() only returns strings with at least one site.
struct StringXRef {
    uint32_t              stringIndex;  // in strings() and stringKinds()
    std::string           stringValue;  // raw CESU-8 bytes
    std::vector<XRefSite> sites;        // never empty
};

// Edge in the call graph. calleeIndex == 0xFFFFFFFF means not statically
// resolvable (indirect call).
struct CallSite {
    uint32_t    callerIndex;
    uint32_t    calleeIndex;
    uint32_t    offset;       // in the caller's bytecode
    bool        isDirect;     // true: CallDirect*; false: CreateClosure* or Call*/Construct*
    std::string mnemonic;     // distinguishes the cases, e.g. "CallDirect", "CreateClosure", "Call2"
};

// =============================================================================
// Literal buffers
// =============================================================================

// A NewArrayWithBuffer[Long] / NewObjectWithBuffer[Long] instruction. The
// serialized literal (SLP format, decoded in `cli/literals.py`) lives in the
// array / object key / object value buffers.
struct LiteralSite {
    uint32_t    functionIndex;
    uint32_t    instructionOffset;  // in the function's bytecode
    std::string mnemonic;
    bool        isObject;
    uint32_t    count;              // numElems (array) or numLiterals (object)
    uint32_t    bufferIdx;          // array: the buffer; object: the value buffer
    uint32_t    keyBufferIdx;       // object only (0 for arrays)
};

// =============================================================================
// Modules, source locations, sections
// =============================================================================

// Metro module, from the __d(factory, moduleId, deps) pattern in the global function.
struct ModuleInfo {
    uint32_t functionIndex;  // factory function
    uint32_t moduleId;
};

// Maps a bytecode address to a source position (from the debug info section).
struct SourceLocation {
    uint32_t address;  // from the start of the function's bytecode
    uint32_t line;     // 1-indexed, relative to FileRegion.line
    uint32_t column;   // 0-indexed
};

struct Section {
    std::string name;
    uint32_t    offset;  // absolute
    uint32_t    size;
    uint32_t    align;
};

// =============================================================================
// Debug info
// =============================================================================
// 7-field header at header.debugInfoOffset. The last four fields are
// CUMULATIVE offsets into the data blob (which starts after the filename
// table, filename storage and file regions): each marks the end of its
// sub-section, i.e. the start of the next.
//   [0, sourceLocDataEnd)               source location data
//   [sourceLocDataEnd, scopeDescDataEnd)   scope descriptor data
//   [scopeDescDataEnd, textifiedCalleesEnd) textified callees data
//   [textifiedCalleesEnd, stringTableEnd)   debug string table
// Hermes calls them scopeDescDataOffset, textifiedCalleeOffset,
// stringTableOffset and debugDataSize (same values, read as the start of the
// following sub-section).
struct DebugInfoHeader {
    uint32_t filenameCount;
    uint32_t filenameStorageSize;
    uint32_t fileRegionCount;
    uint32_t sourceLocDataEnd;
    uint32_t scopeDescDataEnd;
    uint32_t textifiedCalleesEnd;
    uint32_t stringTableEnd;  // = total blob size
};

// A variable captured in a function's scope, in declaration order. NOT indexed
// by register: the register-to-name binding must be derived separately (e.g.
// from StoreToEnvironment/LoadFromEnvironment).
struct ScopeVariable {
    std::string name;
};

// Call site with the callee's textual name, for readable stack traces.
struct TextifiedCallee {
    uint32_t    bytecodeOffset;  // of the call instruction
    std::string name;            // from the debug string table
};

// =============================================================================
// HBCFile
// =============================================================================
// An open HBC file (Pimpl: fd/mmap live in parser.cpp). Copies share the
// underlying Impl; the last one destroyed unmaps and closes.
class HBCFile {
public:
    // Opens and validates a file. Throws FileTooSmallError, InvalidMagicError
    // or UnsupportedVersionError.
    static HBCFile open(const std::string& path);

    // Like open() but from an in-memory image (copied). Used by the patcher to
    // re-parse an edited image. Same exceptions as open().
    static HBCFile from_bytes(const std::string& bytes);

    uint32_t version() const;  // 94, 95 or 96

    // Header, referencing the mapped file.
    const HBCFileHeader& header() const;

    const std::vector<std::string>&  strings()                      const;
    const std::vector<FunctionInfo>& functions()                    const;
    const std::vector<StringKind>&   stringKinds()                  const;
    const std::vector<StringEntry>&  string_entries()               const;
    const std::vector<LiteralSite>&  literal_sites()                const;
    const std::string&               array_buffer()                 const;
    const std::string&               obj_key_buffer()               const;
    const std::string&               obj_value_buffer()             const;
    // ABSOLUTE file offsets of the three buffers (from section_map()); 0 if the
    // section is absent.
    uint32_t                         array_buffer_offset()          const;
    uint32_t                         obj_key_buffer_offset()        const;
    uint32_t                         obj_value_buffer_offset()      const;
    std::vector<Instruction>         disasm(uint32_t functionIndex) const;
    CFGData                          cfg(uint32_t functionIndex)    const;
    std::vector<StringXRef>          string_xrefs()                 const;
    std::vector<CallSite>            call_graph()                   const;
    std::vector<ModuleInfo>          recover_modules()              const;
    std::vector<std::string>         source_files()                 const;
    std::vector<SourceLocation>      source_locations(uint32_t functionIndex) const;
    std::string                      decompile(uint32_t functionIndex) const;
    DebugInfoHeader                  debug_info_header()            const;
    std::vector<ScopeVariable>       scope_variables(uint32_t functionIndex)   const;
    std::vector<TextifiedCallee>     textified_callees(uint32_t functionIndex) const;
    std::vector<std::string>         debug_strings()                const;

    // The file's raw bytes (read-only mapped memory) and size (= header
    // fileLength, footer included).
    const uint8_t* raw_data() const;
    size_t         raw_size() const;

    // All sections with absolute offset, size and alignment. Offsets are
    // computed from the header counts: the format only stores debugInfoOffset.
    std::vector<Section> section_map() const;

    // Byte copy of the mapped file (an unchanged file is identical to the source).
    std::vector<uint8_t> serialize() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit HBCFile(std::shared_ptr<Impl> impl);
};

// =============================================================================
// Exceptions
// =============================================================================

// First 8 bytes don't match HBC_MAGIC.
struct InvalidMagicError : std::runtime_error {
    InvalidMagicError() : std::runtime_error("Not a valid HBC file (bad magic)") {}
};

// Version not in SUPPORTED_VERSIONS.
struct UnsupportedVersionError : std::runtime_error {
    explicit UnsupportedVersionError(uint32_t v)
        : std::runtime_error("Unsupported HBC version: " + std::to_string(v)) {}
};

// Smaller than the 128-byte header.
struct FileTooSmallError : std::runtime_error {
    FileTooSmallError() : std::runtime_error("File too small to be a valid HBC file") {}
};

} // namespace hbc
