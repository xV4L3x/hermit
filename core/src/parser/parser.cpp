// HBCFile implementation: open/from_bytes, memoized accessors, section map and
// the debug info decoders. The only translation unit that sees HBCFile::Impl.

#include "hbc/hbc.h"
#include "analysis/analysis.h"
#include "hbc/format_v94.h"
#include "hbc/format_v95.h"
#include "hbc/format_v96.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "parser/section_layout.h"
#include "parser/string_table.h"
#include "parser/function_table.h"
#include "parser/string_kind.h"
#include "parser/exception_table.h"
#include "parser/function_source_table.h"
#include "disasm/disasm.h"
#include "cfg/cfg.h"

namespace hbc {

// Backing storage is either an mmap of the file (fd != -1) or an owned copy of
// an in-memory image (from_bytes). Every derived table is parsed on first
// access and memoized. call_graph(), recover_modules() and literal_sites() in
// particular disassemble the whole file: without the cache, decompiling every
// function would be O(N^2) (~1.25 s/function on ridemovi.hbc, 37774 functions).
struct HBCFile::Impl {
    void*  data = MAP_FAILED;
    size_t size = 0;
    int    fd   = -1;
    std::vector<uint8_t> owned;  // from_bytes() only; `data` points into it
    const HBCFileHeader* hdr = nullptr;

    mutable std::optional<std::vector<std::string>>  strings;
    mutable std::optional<std::vector<FunctionInfo>> functions;
    mutable std::optional<std::vector<StringKind>>   kinds;
    mutable std::optional<std::vector<StringEntry>>  stringEntries;
    mutable std::optional<std::vector<CallSite>>     callGraph;
    mutable std::optional<std::vector<ModuleInfo>>   modules;
    mutable std::optional<std::vector<LiteralSite>>  literalSites;
    mutable std::optional<std::string> arrayBuffer, objKeyBuffer, objValueBuffer;

    ~Impl() {
        if (fd != -1) {
            if (data != MAP_FAILED) munmap(data, size);
            close(fd);
        }
    }
};

// Checks size, magic and version. Returns the header, or throws the same
// exceptions as open().
static const HBCFileHeader* validateHeader(const void* data, size_t size) {
    if (size < sizeof(HBCFileHeader))
        throw FileTooSmallError{};

    uint64_t magic{};
    std::memcpy(&magic, data, sizeof(magic));
    if (magic != HBC_MAGIC)
        throw InvalidMagicError{};

    uint32_t ver{};
    std::memcpy(&ver, static_cast<const uint8_t*>(data) + 8, sizeof(ver));
    switch (ver) {
        case 96: return reinterpret_cast<const HBCFileHeader_v96*>(data);
        case 95: return reinterpret_cast<const HBCFileHeader_v95*>(data);
        case 94: return reinterpret_cast<const HBCFileHeader_v94*>(data);
        default: throw UnsupportedVersionError{ver};
    }
}

HBCFile HBCFile::open(const std::string& path) {
    auto impl = std::make_shared<Impl>();

    impl->fd = ::open(path.c_str(), O_RDONLY);
    if (impl->fd == -1)
        throw std::runtime_error("Cannot open '" + path + "': " + std::strerror(errno));

    struct stat st{};
    if (fstat(impl->fd, &st) == -1)
        throw std::runtime_error("Cannot stat '" + path + "': " + std::strerror(errno));
    impl->size = static_cast<size_t>(st.st_size);

    if (impl->size < sizeof(HBCFileHeader))
        throw FileTooSmallError{};

    impl->data = mmap(nullptr, impl->size, PROT_READ, MAP_PRIVATE, impl->fd, 0);
    if (impl->data == MAP_FAILED)
        throw std::runtime_error("mmap failed for '" + path + "': " + std::strerror(errno));

    impl->hdr = validateHeader(impl->data, impl->size);
    return HBCFile{std::move(impl)};
}

HBCFile HBCFile::from_bytes(const std::string& bytes) {
    auto impl = std::make_shared<Impl>();
    impl->owned.assign(bytes.begin(), bytes.end());
    impl->data = impl->owned.data();
    impl->size = impl->owned.size();
    impl->hdr  = validateHeader(impl->data, impl->size);
    return HBCFile{std::move(impl)};
}

HBCFile::HBCFile(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

uint32_t HBCFile::version() const { return impl_->hdr->version; }

const HBCFileHeader& HBCFile::header() const { return *impl_->hdr; }

const uint8_t* HBCFile::raw_data() const { return static_cast<const uint8_t*>(impl_->data); }

size_t HBCFile::raw_size() const { return impl_->size; }

// Copy of the mapped bytes. The patcher rebuilds sections itself (Module).
std::vector<uint8_t> HBCFile::serialize() const {
    const uint8_t* src = raw_data();
    return std::vector<uint8_t>(src, src + raw_size());
}

// -----------------------------------------------------------------------------
// Memoized tables
// -----------------------------------------------------------------------------

const std::vector<std::string>& HBCFile::strings() const {
    if (!impl_->strings)
        impl_->strings = parseStringTable(raw_data(), header());
    return *impl_->strings;
}

const std::vector<FunctionInfo>& HBCFile::functions() const {
    if (!impl_->functions) {
        auto funcs = parseFunctionTable(raw_data(), header(), strings());
        const uint32_t fileSize = static_cast<uint32_t>(raw_size());
        parseExceptionHandlers(raw_data(), fileSize, funcs);

        uint32_t srcTableOffset = 0;
        for (const auto& s : section_map())
            if (s.name == "function source table") { srcTableOffset = s.offset; break; }
        parseFunctionSourceTable(raw_data(), fileSize, srcTableOffset,
                                 header().functionSourceCount, strings(), funcs);

        impl_->functions = std::move(funcs);
    }
    return *impl_->functions;
}

const std::vector<StringKind>& HBCFile::stringKinds() const {
    if (!impl_->kinds)
        impl_->kinds = parseStringKinds(raw_data(), header());
    return *impl_->kinds;
}

const std::vector<StringEntry>& HBCFile::string_entries() const {
    if (!impl_->stringEntries)
        impl_->stringEntries = parseStringEntries(raw_data(), header(), stringKinds());
    return *impl_->stringEntries;
}

std::vector<CallSite> HBCFile::call_graph() const {
    if (!impl_->callGraph)
        impl_->callGraph = computeCallGraph(*this);
    return *impl_->callGraph;
}

std::vector<ModuleInfo> HBCFile::recover_modules() const {
    if (!impl_->modules)
        impl_->modules = computeModules(*this);
    return *impl_->modules;
}

const std::vector<LiteralSite>& HBCFile::literal_sites() const {
    if (!impl_->literalSites)
        impl_->literalSites = computeLiteralSites(*this);
    return *impl_->literalSites;
}

namespace {
const Section* findSection(const std::vector<Section>& map, const char* name) {
    for (const auto& s : map)
        if (s.name == name) return &s;
    return nullptr;
}

// Verbatim bytes of a section; empty if the section is absent (size 0).
std::string sliceSectionByName(const HBCFile& f, const char* name) {
    const auto map = f.section_map();
    const Section* s = findSection(map, name);
    if (!s) return {};
    return std::string(reinterpret_cast<const char*>(f.raw_data() + s->offset), s->size);
}

uint32_t offsetOfSection(const HBCFile& f, const char* name) {
    const auto map = f.section_map();
    const Section* s = findSection(map, name);
    return s ? s->offset : 0;
}
} // namespace

const std::string& HBCFile::array_buffer() const {
    if (!impl_->arrayBuffer)
        impl_->arrayBuffer = sliceSectionByName(*this, "array buffer");
    return *impl_->arrayBuffer;
}

const std::string& HBCFile::obj_key_buffer() const {
    if (!impl_->objKeyBuffer)
        impl_->objKeyBuffer = sliceSectionByName(*this, "object key buffer");
    return *impl_->objKeyBuffer;
}

const std::string& HBCFile::obj_value_buffer() const {
    if (!impl_->objValueBuffer)
        impl_->objValueBuffer = sliceSectionByName(*this, "object value buffer");
    return *impl_->objValueBuffer;
}

uint32_t HBCFile::array_buffer_offset()     const { return offsetOfSection(*this, "array buffer"); }
uint32_t HBCFile::obj_key_buffer_offset()   const { return offsetOfSection(*this, "object key buffer"); }
uint32_t HBCFile::obj_value_buffer_offset() const { return offsetOfSection(*this, "object value buffer"); }

// -----------------------------------------------------------------------------
// Disassembly and CFG
// -----------------------------------------------------------------------------

// Also updates isAsync/isGenerator in the cached FunctionInfo: they are not
// stored in the header, so they are detected from the CreateGenerator / Await
// opcodes the first time the function is disassembled.
std::vector<Instruction> HBCFile::disasm(uint32_t functionIndex) const {
    const auto& funcs = functions();

    if (functionIndex >= funcs.size()) {
        throw std::out_of_range(
            "disasm: function index " + std::to_string(functionIndex) +
            " out of range (functionCount=" + std::to_string(funcs.size()) + ")");
    }

    const FunctionInfo& fi = funcs[functionIndex];
    auto result = disassemble(
        raw_data() + fi.offset, fi.bytecodeSizeInBytes,
        version(), strings(), stringKinds(),
        raw_data(), static_cast<uint32_t>(raw_size())
    );

    bool hasCreateGenerator = false;
    bool hasAwait           = false;
    for (const auto& instr : result) {
        if (instr.mnemonic == "CreateGenerator") hasCreateGenerator = true;
        if (instr.mnemonic == "Await")           hasAwait           = true;
    }
    (*impl_->functions)[functionIndex].isGenerator = hasCreateGenerator;
    (*impl_->functions)[functionIndex].isAsync     = hasAwait;

    return result;
}

CFGData HBCFile::cfg(uint32_t functionIndex) const {
    const auto& funcs = functions();
    if (functionIndex >= funcs.size()) {
        throw std::out_of_range(
            "cfg: function index " + std::to_string(functionIndex) +
            " out of range (functionCount=" + std::to_string(funcs.size()) + ")");
    }
    const FunctionInfo& fi = funcs[functionIndex];
    auto instrs = disasm(functionIndex);
    return buildCFG(instrs, fi.bytecodeSizeInBytes, version(), fi.exceptionHandlers);
}

// -----------------------------------------------------------------------------
// section_map()
// -----------------------------------------------------------------------------
// The format stores only counts and sizes (plus debugInfoOffset), so offsets
// are computed cumulatively from the end of the header, in Hermes's canonical
// order (visitBytecodeSegmentsInOrder).
std::vector<Section> HBCFile::section_map() const {
    const auto& h = *impl_->hdr;
    std::vector<Section> sections;

    uint32_t pos = 0;
    sections.push_back({"file header", pos, sizeof(HBCFileHeader), 1});
    pos += sizeof(HBCFileHeader);

    // Empty sections are omitted and don't advance pos.
    auto add = [&](const char* name, uint32_t size, uint32_t align = 4) {
        if (size == 0) return;
        pos = alignUp(pos, align);
        sections.push_back({name, pos, size, align});
        pos += size;
    };

    add("function headers",      h.functionCount       * sizeof(SmallFuncHeader));
    add("string kind table",     h.stringKindCount     * 4);
    add("identifier hash table", h.identifierCount     * 4);
    add("string table",          h.stringCount         * 4);
    add("overflow string table", h.overflowStringCount * 8);
    // Hermes aligns every segment to 4 (BYTECODE_ALIGNMENT), blobs included. With
    // align 1 the 0-3 padding bytes after an odd-sized string storage vanished:
    // invisible to serialize() (raw copy) but it broke Module's re-layout.
    add("string storage",        h.stringStorageSize,    4);
    add("array buffer",          h.arrayBufferSize,      4);
    add("object key buffer",     h.objKeyBufferSize,     4);
    add("object value buffer",   h.objValueBufferSize,   4);
    add("bigint table",          h.bigIntCount         * sizeof(BigIntTableEntry));
    add("bigint storage",        h.bigIntStorageSize,    4);
    add("regexp table",          h.regExpCount         * sizeof(RegExpTableEntry));
    add("regexp storage",        h.regExpStorageSize,    4);
    add("cjs module table",      h.cjsModuleCount      * 8);
    add("function source table", h.functionSourceCount * 8);

    // All function bodies as one block, up to debugInfoOffset. Exact
    // per-function extents come from SmallFuncHeader.bytecodeSizeInBytes.
    if (h.debugInfoOffset > alignUp(pos, 4))
        add("bytecode", h.debugInfoOffset - alignUp(pos, 4));

    if (h.debugInfoOffset > 0)
        add("debug info", h.fileLength - 20 - h.debugInfoOffset);

    // SHA-1 of everything above the footer. The runtime doesn't verify it, but
    // the patcher recomputes it.
    add("file footer", 20, 1);

    return sections;
}

// -----------------------------------------------------------------------------
// Debug info
// -----------------------------------------------------------------------------

// Signed LEB128: 7 bits per byte, bit 7 = continuation. Returns 0 if `limit` is hit.
static int64_t readSLEB128(const uint8_t* data, uint32_t limit, uint32_t& pos) {
    int64_t result = 0;
    int     shift  = 0;
    uint8_t byte   = 0;
    do {
        if (pos >= limit) return 0;
        byte = data[pos++];
        result |= static_cast<int64_t>(byte & 0x7F) << shift;
        shift  += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) {
        result |= -(static_cast<int64_t>(1) << shift);
    }
    return result;
}

// Layout of the debug info section, starting at header.debugInfoOffset:
//   DebugInfoHeader                       7 x uint32
//   filename table                        filenameCount x { offset, length }, 8 bytes each
//   filename storage                      filenameStorageSize bytes, NUL-separated
//   file regions                          fileRegionCount x 12 bytes
//   data blob, in 4 contiguous sub-sections (see DebugInfoHeader in hbc.h):
//     source location data, scope descriptors, textified callees, debug string table
namespace {
struct DebugLayout {
    DebugInfoHeader hdr{};
    uint32_t filenameStorageStart = 0;
    uint32_t fileRegionsStart     = 0;
    uint32_t blobStart            = 0;
};

// False if the section is absent or truncated.
bool computeDebugLayout(const HBCFileHeader& h, const uint8_t* fileData, uint32_t fileSize, DebugLayout& out) {
    uint32_t dbgOff = h.debugInfoOffset;
    constexpr uint32_t HEADER_SIZE = 7 * 4;
    if (dbgOff == 0 || dbgOff + HEADER_SIZE > fileSize) return false;

    std::memcpy(&out.hdr, fileData + dbgOff, HEADER_SIZE);

    out.filenameStorageStart = dbgOff + HEADER_SIZE + out.hdr.filenameCount * 8;
    out.fileRegionsStart     = out.filenameStorageStart + out.hdr.filenameStorageSize;
    out.blobStart            = out.fileRegionsStart + out.hdr.fileRegionCount * 12;

    return out.blobStart <= fileSize;
}
} // namespace

// Zero-initialized if the section is absent or corrupt.
DebugInfoHeader HBCFile::debug_info_header() const {
    DebugLayout layout;
    computeDebugLayout(*impl_->hdr, raw_data(), static_cast<uint32_t>(raw_size()), layout);
    return layout.hdr;
}

std::vector<std::string> HBCFile::source_files() const {
    DebugLayout layout;
    if (!computeDebugLayout(*impl_->hdr, raw_data(), static_cast<uint32_t>(raw_size()), layout))
        return {};

    const uint32_t fileSize = static_cast<uint32_t>(raw_size());
    const uint32_t storageSize = layout.hdr.filenameStorageSize;

    if (storageSize == 0 || layout.filenameStorageStart + storageSize > fileSize)
        return {};

    const uint8_t* p   = raw_data() + layout.filenameStorageStart;
    const uint8_t* end = p + storageSize;

    std::vector<std::string> result;
    result.reserve(layout.hdr.filenameCount);
    while (p < end && result.size() < layout.hdr.filenameCount) {
        const uint8_t* nul = p;
        while (nul < end && *nul != '\0') ++nul;
        result.emplace_back(reinterpret_cast<const char*>(p), nul - p);
        p = nul + 1;
    }
    return result;
}

// Decodes a function's source location stream (v94-96), which starts at
// blobStart + fi.debugOffset:
//   header: functionIndex (discarded, NOT the start address), startLine, startCol
//   then, until addrDelta == -1 (the 0x7F terminator), per entry:
//     addrDelta     delta from curAddr, which starts at 0
//     lineDeltaRaw  bit 0: a stmtDelta follows; bits 1+: line delta
//     colDelta      delta from curCol
//     scope_addr, env_register                          ignored
//     stmtDelta (only if lineDeltaRaw bit 0 is set)     ignored
std::vector<SourceLocation> HBCFile::source_locations(uint32_t funcIndex) const {
    const uint8_t* fileData = raw_data();
    const uint32_t fileSize = static_cast<uint32_t>(raw_size());

    DebugLayout layout;
    if (!computeDebugLayout(*impl_->hdr, fileData, fileSize, layout)) return {};
    if (layout.blobStart >= fileSize) return {};

    const auto& funcs = functions();
    if (funcIndex >= funcs.size()) return {};
    const FunctionInfo& fi = funcs[funcIndex];
    if (!fi.hasDebugInfo) return {};

    uint32_t pos = layout.blobStart + fi.debugOffset;
    if (pos >= fileSize) return {};

    readSLEB128(fileData, fileSize, pos);  // functionIndex
    int64_t curLine = readSLEB128(fileData, fileSize, pos);
    int64_t curCol  = readSLEB128(fileData, fileSize, pos);
    int64_t curAddr = 0;

    std::vector<SourceLocation> result;

    // Guard against corrupt streams.
    for (int guard = 0; guard < 100000; ++guard) {
        if (pos >= fileSize) break;

        int64_t addrDelta = readSLEB128(fileData, fileSize, pos);
        if (addrDelta == -1) break;

        int64_t lineDeltaRaw = readSLEB128(fileData, fileSize, pos);
        int64_t colDelta     = readSLEB128(fileData, fileSize, pos);

        readSLEB128(fileData, fileSize, pos);  // scope_address
        readSLEB128(fileData, fileSize, pos);  // env_register

        if (lineDeltaRaw & 1)
            readSLEB128(fileData, fileSize, pos);  // stmt_delta

        curAddr += addrDelta;
        curLine += lineDeltaRaw >> 1;
        curCol  += colDelta;

        result.push_back({
            static_cast<uint32_t>(curAddr),
            static_cast<uint32_t>(curLine),
            static_cast<uint32_t>(curCol),
        });
    }

    return result;
}

// Debug string table entry: [len: SLEB128][len raw bytes].
static std::string readLengthPrefixedString(const uint8_t* data, uint32_t limit, uint32_t& pos) {
    int64_t len = readSLEB128(data, limit, pos);
    if (len < 0 || pos + static_cast<uint64_t>(len) > limit) return {};
    std::string s(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(len));
    pos += static_cast<uint32_t>(len);
    return s;
}

// Debug string table as (byte offset, string) pairs in file order. Scope
// descriptors and textified callees reference a name by the BYTE OFFSET of its
// entry from the start of the table, not by position (Hermes
// DebugInfoGenerator::appendString / DebugInfo::decodeString). The two coincide
// only for the first entry, so a corpus with a single debug string can't tell
// them apart.
using DebugStringTable = std::vector<std::pair<uint32_t, std::string>>;

static DebugStringTable readDebugStringTable(const uint8_t* fileData, uint32_t fileSize,
                                             const DebugLayout& layout) {
    DebugStringTable table;
    uint32_t start = layout.blobStart + layout.hdr.textifiedCalleesEnd;
    uint32_t end   = layout.blobStart + layout.hdr.stringTableEnd;
    if (end > fileSize) end = fileSize;

    uint32_t pos = start;
    while (pos < end) {
        uint32_t offset = pos - start;
        std::string s = readLengthPrefixedString(fileData, end, pos);
        table.emplace_back(offset, std::move(s));
    }
    return table;
}

// A reference that doesn't land on an entry start yields "<debug_str_N>"
// instead of a wrong name.
static std::string resolveDebugString(const DebugStringTable& table, int64_t offset) {
    if (offset >= 0) {
        auto it = std::lower_bound(table.begin(), table.end(), offset,
            [](const std::pair<uint32_t, std::string>& e, int64_t off) {
                return static_cast<int64_t>(e.first) < off;
            });
        if (it != table.end() && static_cast<int64_t>(it->first) == offset) return it->second;
    }
    return "<debug_str_" + std::to_string(offset) + ">";
}

// Entry at blobStart + sourceLocDataEnd + fi.scopeDescOffset:
//   parentId       SLEB128  (-1 = none; else byte offset of the parent entry)   unused
//   flags          SLEB128  (bit 0 InnerScope, bit 1 Dynamic)                   unused
//   varNamesCount  SLEB128
//   varNamesCount x nameOffset SLEB128 (byte offset into the debug string table)
std::vector<ScopeVariable> HBCFile::scope_variables(uint32_t functionIndex) const {
    const uint8_t* fileData = raw_data();
    const uint32_t fileSize = static_cast<uint32_t>(raw_size());

    DebugLayout layout;
    if (!computeDebugLayout(*impl_->hdr, fileData, fileSize, layout)) return {};

    const auto& funcs = functions();
    if (functionIndex >= funcs.size()) return {};
    const FunctionInfo& fi = funcs[functionIndex];
    if (!fi.hasDebugInfo) return {};

    uint32_t pos = layout.blobStart + layout.hdr.sourceLocDataEnd + fi.scopeDescOffset;
    if (pos >= fileSize) return {};

    readSLEB128(fileData, fileSize, pos);  // parentId
    if (pos >= fileSize) return {};
    readSLEB128(fileData, fileSize, pos);  // flags
    int64_t count = readSLEB128(fileData, fileSize, pos);
    if (count < 0 || count > 10000) return {};

    DebugStringTable strings = readDebugStringTable(fileData, fileSize, layout);

    std::vector<ScopeVariable> result;
    result.reserve(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; i++) {
        int64_t nameOffset = readSLEB128(fileData, fileSize, pos);
        result.push_back({resolveDebugString(strings, nameOffset)});
    }
    return result;
}

// Entry at blobStart + scopeDescDataEnd + fi.textifiedCalleesOffset:
//   count SLEB128, then count x { bcOffset SLEB128, nameOffset SLEB128 }
// nameOffset is a byte offset into the debug string table.
std::vector<TextifiedCallee> HBCFile::textified_callees(uint32_t functionIndex) const {
    const uint8_t* fileData = raw_data();
    const uint32_t fileSize = static_cast<uint32_t>(raw_size());

    DebugLayout layout;
    if (!computeDebugLayout(*impl_->hdr, fileData, fileSize, layout)) return {};

    const auto& funcs = functions();
    if (functionIndex >= funcs.size()) return {};
    const FunctionInfo& fi = funcs[functionIndex];
    if (!fi.hasDebugInfo) return {};

    uint32_t pos = layout.blobStart + layout.hdr.scopeDescDataEnd + fi.textifiedCalleesOffset;
    if (pos >= fileSize) return {};

    int64_t count = readSLEB128(fileData, fileSize, pos);
    if (count < 0 || count > 10000) return {};

    DebugStringTable strings = readDebugStringTable(fileData, fileSize, layout);

    std::vector<TextifiedCallee> result;
    result.reserve(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; i++) {
        int64_t bcOffset   = readSLEB128(fileData, fileSize, pos);
        int64_t nameOffset = readSLEB128(fileData, fileSize, pos);
        result.push_back({static_cast<uint32_t>(bcOffset), resolveDebugString(strings, nameOffset)});
    }
    return result;
}

// Debug string table in file order. Positions in the result are NOT the
// references used by scope descriptors and textified callees (those are byte offsets).
std::vector<std::string> HBCFile::debug_strings() const {
    const uint8_t* fileData = raw_data();
    const uint32_t fileSize = static_cast<uint32_t>(raw_size());

    DebugLayout layout;
    if (!computeDebugLayout(*impl_->hdr, fileData, fileSize, layout)) return {};

    std::vector<std::string> result;
    for (auto& entry : readDebugStringTable(fileData, fileSize, layout))
        result.push_back(std::move(entry.second));
    return result;
}

} // namespace hbc
