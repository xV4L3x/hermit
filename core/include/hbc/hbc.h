// Public API of libhbc: the only header clients (Python bindings,
// tests) need.
#pragma once

#include "hbc/format.h"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <optional>
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

// Where the byte range of a string lies, compared with the string storage and
// the file. The runtime reads [offset, offset + length * unit) without checking it.
enum class StringVerdict : uint8_t {
    InsideStorage,    // the range is inside the string storage (every compiler-made file)
    InsideFile,       // outside the storage but inside the file: it aliases other sections
    OutsideFile,      // the range leaves the file: nothing can be read
    BadOverflowIndex, // length == 255 and the overflow table index is out of range
};

// Raw location of a string in the file, for same-length in-place edits.
// HBCFile::string_entries() is parallel to strings() and stringKinds().
struct StringEntry {
    uint32_t   storageOffset;  // ABSOLUTE file offset of the first byte
    uint32_t   byteLength;     // bytes in the storage: charLength (ASCII) or charLength * 2 (UTF-16);
                               // saturates at UINT32_MAX when the declared length is absurd
    uint32_t   charLength;     // code units: bytes of an ASCII entry, 16-bit units of a UTF-16 one
                               // (resolved, overflow table included)
    bool       isUTF16;
    bool       isOverflow;     // SmallStringTableEntry.length == 255
    StringKind kind;
    StringVerdict verdict = StringVerdict::InsideStorage;
    bool       misaligned = false;  // UTF-16 entry at an odd storage offset (the runtime does
                                    // not require alignment; compiler-made files never have it)
};

// How strictly HBCFile checks what it parses. The header and the section layout are
// always checked; the mode only decides what happens to a string entry whose range
// is not InsideStorage.
enum class ValidationMode : uint8_t {
    Strict,      // throw InvalidStringEntryError (default)
    Permissive,  // keep the entry, report it in string_entries(); strings() yields the bytes when
                 // the range is InsideFile and "" otherwise. For analysing malformed or hostile
                 // files; the patcher must treat such a file as read-only.
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
    GeneratorResume,   // SaveGenerator block -> its resume point (next generator call)
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

// How an instruction refers to a string.
enum class XRefKind : uint8_t {
    Operand,       // a StringID operand of the instruction itself
    ArrayElement,  // an element of the array literal buffer built by NewArrayWithBuffer*
    ObjectKey,     // a key in the object key buffer built by NewObjectWithBuffer*
    ObjectValue,   // a value in the object value buffer built by NewObjectWithBuffer*
};

struct XRefSite {
    uint32_t    functionIndex;
    uint32_t    instructionOffset;  // in the function's bytecode
    std::string mnemonic;           // e.g. "LoadConstString", "GetByIdShort", "NewArrayWithBuffer"
    // Literal references only (kind != Operand): the string is an element of the
    // literal buffer the instruction points at, not an operand. One site per
    // instruction, string and buffer, however many elements hold the string.
    XRefKind    kind           = XRefKind::Operand;
    uint32_t    elementIndex   = 0;  // first element holding the string (0 for Operand)
    uint32_t    elementCount   = 1;  // how many elements of this literal hold it
};

// Reference sites of one string. string_xrefs() only returns strings with at least one site.
struct StringXRef {
    uint32_t              stringIndex;  // in strings() and stringKinds()
    std::string           stringValue;  // the string as UTF-8 (as in strings())
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

// Metro module, from a `__d(factory, moduleId, dependencyMap)` call in the global
// function (recover_modules()). The ids in `dependencies` are module ids, in the
// order of the factory's dependencyMap (`_$$_REQUIRE(dependencyMap[i])`). A
// nullopt entry is a `null` in the dependencyMap: an optional dependency
// (`try { require('x') } catch {}`) that Metro could not resolve at build time.
struct MetroModule {
    uint32_t functionIndex = 0;          // factory function
    uint32_t moduleId      = 0;
    std::vector<std::optional<uint32_t>> dependencies;  // dependencyMap, in order
    // false: the dependencyMap argument is not an array literal of integers and
    // nulls, so `dependencies` is empty without meaning "no dependencies".
    bool     dependenciesKnown = true;
    uint32_t defineOffset      = 0;      // offset of the __d call in the global function
};

// Closure environments (env_accesses() and friends). An environment is created
// by CreateEnvironment (the function's scope; parent = the environment the
// running closure captured) or CreateInnerEnvironment (parent = the one in its
// register operand), and is identified by that instruction: (function, offset).
// GetEnvironment N reads the captured environment of the running closure and
// walks N parent links; CreateClosure*/CreateGenerator* capture the environment
// in their register operand. Semantics from lib/VM/Interpreter.cpp at
// hermes-2023-09-26-RNv0.73.0 (HBC 96).
struct EnvKey {
    uint32_t function = 0;  // function that creates the environment
    uint32_t offset   = 0;  // of its Create[Inner]Environment instruction
    bool operator==(const EnvKey& o) const { return function == o.function && offset == o.offset; }
    bool operator<(const EnvKey& o) const {
        return function != o.function ? function < o.function : offset < o.offset;
    }
};

// One Load/Store[NP]ToEnvironment[L] instruction, resolved to the environment
// it touches.
struct EnvAccess {
    uint32_t offset   = 0;      // of the instruction, in its function
    bool     isStore  = false;
    uint32_t slot     = 0;
    bool     resolved = false;  // false: the environment could not be traced
    EnvKey   env;               // valid when resolved
};

// An environment a function creates, with its parent.
struct EnvironmentInfo {
    EnvKey env;
    bool   isInner     = false;  // CreateInnerEnvironment
    bool   parentKnown = false;  // false: parent could not be traced
    bool   hasParent   = false;  // parentKnown && a parent exists (not the global scope)
    EnvKey parent;
};

// A use of an environment slot: function + offset of the Load/Store.
struct EnvSlotUse {
    uint32_t functionIndex = 0;
    uint32_t offset        = 0;
    bool     isStore       = false;
};

// Value analysis (value_flow()): what a register provably holds at an
// instruction. Only facts that hold on every path are kept; anything else is
// absent (std::nullopt), never guessed.
struct AbstractValue {
    enum class Kind : uint8_t {
        GlobalObject,   // the global object (GetGlobalObject)
        Global,         // a property of it: `name`
        FactoryParam,   // a Metro factory parameter: index 1..7 = global, require,
                        // importDefault, importAll, module, exports, dependencyMap
        Closure,        // a closure of function `index`
        Module,         // the exports of Metro module `index` (require(dependencyMap[k]))
        ImportDefault,  // _interopRequireDefault(module `index`): {default: m} or m itself
        DepIndex,       // dependencyMap[`index`] of the enclosing module
        Prop,           // `base`.`name`
        Object,         // the object literal allocated at offset `index` of function
                        // `(uint32_t)number` (NewObject / NewObjectWithBuffer)
        Number, String, Undefined, Null, Boolean,  // constants (`number` / `name`)
    };
    Kind        kind  = Kind::Undefined;
    uint32_t    index = 0;
    double      number = 0;       // Number; Boolean (0 or 1)
    std::string name;             // Global / Prop: property name; String: the value
    std::shared_ptr<const AbstractValue> base;  // Prop only

    bool operator==(const AbstractValue& o) const;
    bool operator!=(const AbstractValue& o) const { return !(*this == o); }
    // Readable form: module_2.AppRegistry, func_84, require, dependencyMap[3], "s", 3
    std::string toString() const;
};

// The exports of a Metro module (module_exports()), from its code:
// `exports.X = v`, `Object.defineProperty(exports, "X", {get() {return v}})` or
// `{value: v}`, `module.exports = v`, `module.exports.X = v`. A name keeps a
// value only when every store agrees (Babel's `exports.X = void 0` placeholders
// are ignored when a real store exists).
struct ModuleExport {
    std::string name;
    std::optional<AbstractValue> value;
    bool viaGetter = false;  // Object.defineProperty with a getter
};

struct ModuleExports {
    bool reassigned = false;             // module.exports = value
    std::optional<AbstractValue> value;  // that value, when every assignment agrees
    bool esModule = false;               // __esModule defined (Babel ES module)
    bool dynamicNames = false;           // some name is not a constant (e.g. export *)
    std::vector<ModuleExport> names;     // sorted by name, __esModule excluded
};

// A call whose callee is a known function (resolved_calls()).
struct ResolvedCall {
    enum class Via : uint8_t {
        Closure,  // the callee register holds the closure (local, or an environment slot)
        Export,   // reached through module exports (module_M.x, importDefaultOf(module_M).default)
    };
    uint32_t      offset      = 0;   // of the Call*/Construct* instruction
    uint32_t      callee      = 0;   // function index
    Via           via         = Via::Closure;
    bool          isConstruct = false;
    AbstractValue calleeValue;       // the callee as the analysis sees it, before resolution
};

// A call of a function from elsewhere (function_callers()).
struct CallerSite {
    uint32_t         caller = 0;   // function index
    uint32_t         offset = 0;   // of the call instruction
    ResolvedCall::Via via   = ResolvedCall::Via::Closure;
    bool             isConstruct = false;
};

// Register values at one instruction: `operands` parallel to
// Instruction::operands (the value of each register operand before the
// instruction executes; nullopt for unknown or non-register operands), `result`
// the value of the destination register after it.
struct InstrValues {
    uint32_t offset = 0;
    std::vector<std::optional<AbstractValue>> operands;
    std::optional<AbstractValue> result;
};

// An entry of the CJS Module Table (header.cjsModuleCount), present only
// when the file was compiled with Hermes's own native CommonJS mode
// (`hermes -commonjs`), never in a Metro-bundled bundle (recover_modules()
// covers that case instead; the two mechanisms are mutually exclusive). See
// doc/hbc-format/v96-format.md §9 for the on-disk format. The table has two
// on-disk shapes, selected by header.options.cjsModulesStaticallyResolved;
// as of the public Hermes source this project targets, the "static" shape
// can never actually be emitted by the compiler (confirmed by reading
// ResolveStaticRequire's dead `getUseCJSModules()` guard and by compiling
// samples), so `modulePath` is populated and `isStaticallyResolved` is
// false in every file observed so far.
struct CJSModuleInfo {
    uint32_t    functionIndex;        // the module's wrapper function (exports, require, module)
    bool        isStaticallyResolved; // header.options bit 1, see above
    uint32_t    moduleId;             // raw "first" field of the entry: a String Table
                                       // index when !isStaticallyResolved, otherwise a
                                       // compiler-assigned integer (never observed)
    std::string modulePath;           // !isStaticallyResolved only: strings()[moduleId],
                                       // the module's source file path. Empty otherwise.
};

// An entry of the BigInt Table (header.bigIntCount). The value is the bytes of the
// entry read as little-endian two's complement (redundant sign bytes dropped: 0n is
// empty, 255n is `ff 00`, -1n is `ff`). `ok` is false if the entry does not fit in the
// BigInt storage; then `error` says why and the value fields are empty.
struct BigIntInfo {
    uint32_t             index;
    uint32_t             offset;      // in the BigInt storage
    uint32_t             length;      // in bytes
    std::vector<uint8_t> bytes;
    bool                 isNegative;
    std::string          decimal;     // signed decimal text, e.g. "-5"; "0" for an empty entry
    bool                 ok;
    std::string          error;
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
    static HBCFile open(const std::string& path, ValidationMode mode = ValidationMode::Strict);

    // Like open() but from an in-memory image (copied). Used by the patcher to
    // re-parse an edited image. Same exceptions as open().
    static HBCFile from_bytes(const std::string& bytes, ValidationMode mode = ValidationMode::Strict);

    ValidationMode validation_mode() const;

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
    const std::vector<Instruction>&  disasm(uint32_t functionIndex) const;
    const CFGData&                   cfg(uint32_t functionIndex)    const;
    std::vector<StringXRef>          string_xrefs()                 const;
    const std::vector<CallSite>&     call_graph()                   const;
    const std::vector<MetroModule>&   recover_modules()              const;
    // Metro entry points: the module ids passed to `__r(id)` in the global
    // function, in call order (empty if the file is not a Metro bundle).
    const std::vector<uint32_t>&     metro_entry_modules()          const;
    // Closure environments, see EnvKey. env_accesses(): every environment
    // load/store of a function, in offset order. environments(): the
    // environments a function creates. captured_environment(): the environment
    // the function captured when it was created (nullopt: none, as for the
    // global function, or not traceable, or the function is never created by
    // a CreateClosure*/CreateGenerator*).
    // env_slot_uses(): every load/store of one slot, across the whole file.
    std::vector<EnvAccess>           env_accesses(uint32_t functionIndex) const;
    std::vector<EnvironmentInfo>     environments(uint32_t functionIndex) const;
    std::optional<EnvKey>            captured_environment(uint32_t functionIndex) const;
    std::vector<EnvSlotUse>          env_slot_uses(const EnvKey& env, uint32_t slot) const;
    // Value analysis, computed per Metro module (the module's functions share
    // their environment slots) or for the functions outside every module.
    // value_flow(): the instructions of `functionIndex` where some register
    // value is known, in offset order. env_slot_value(): the value every store
    // of that slot agrees on, if any. function_module(): the Metro module the
    // function belongs to (its factory or a function nested in it).
    std::vector<InstrValues>         value_flow(uint32_t functionIndex) const;
    std::optional<AbstractValue>     env_slot_value(const EnvKey& env, uint32_t slot) const;
    std::optional<uint32_t>          function_module(uint32_t functionIndex) const;
    // The exports of a Metro module (empty if `moduleId` is not a module).
    ModuleExports                    module_exports(uint32_t moduleId) const;
    // `v` followed through module exports: module_M.x -> the value of export x
    // of module M, importDefaultOf(module_M).default -> its default export (ES
    // module) or module.exports, module_M -> module.exports when reassigned.
    // Values relative to M (its `module.exports`) become module_M. Unknown
    // exports leave the value as it is.
    AbstractValue                    resolve_value(const AbstractValue& v) const;
    // Calls of `functionIndex` whose callee resolves to a function, in offset order.
    const std::vector<ResolvedCall>& resolved_calls(uint32_t functionIndex) const;
    // Calls of `functionIndex` from its own module and every module that imports
    // it, directly or through other modules (dependencyMap), or from the
    // functions outside modules for a function outside modules.
    std::vector<CallerSite>          function_callers(uint32_t functionIndex) const;
    std::vector<CJSModuleInfo>       cjs_modules()                  const;
    // The BigInt Table decoded, one entry per header.bigIntCount (empty if none).
    const std::vector<BigIntInfo>&   bigints()                      const;
    // The Identifier Hash Table as stored: header.identifierCount values, one per
    // Identifier entry in string-table order (see identifier_hash()).
    std::vector<uint32_t>            identifier_hashes()            const;
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

// A string entry whose byte range is not inside the string storage (Strict mode).
struct InvalidStringEntryError : std::runtime_error {
    uint32_t      index;
    StringVerdict verdict;
    InvalidStringEntryError(uint32_t i, StringVerdict v, const std::string& reason)
        : std::runtime_error(reason), index(i), verdict(v) {}
};

// Smaller than the 128-byte header.
struct FileTooSmallError : std::runtime_error {
    FileTooSmallError() : std::runtime_error("File too small to be a valid HBC file") {}
};

// =============================================================================
// Identifier hash
// =============================================================================

// Hash of an Identifier string as Hermes stores it in the Identifier Hash Table
// and the VM uses it to register the symbol: Jenkins one-at-a-time with no
// final mix (h += c; h += h << 10; h ^= h >> 6, from h = 0). `c` is each byte of
// an ASCII entry or each UTF-16LE code unit of a UTF-16 entry, so an ASCII string
// and its UTF-16 form hash the same. Described in the Hermes source:
// include/hermes/Support/HashString.h (hashString), JenkinsHash.h.
// Throws std::invalid_argument on an odd byte length with `isUTF16`.
uint32_t identifier_hash(const uint8_t* data, size_t byteLen, bool isUTF16);

} // namespace hbc
