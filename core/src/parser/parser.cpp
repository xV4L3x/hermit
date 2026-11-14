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
#include <set>
#include <memory>
#include <map>
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
#include "parser/bigint_table.h"
#include "parser/cjs_module_table.h"
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
    ValidationMode mode = ValidationMode::Strict;

    mutable std::optional<std::vector<std::string>>  strings;
    mutable std::optional<std::vector<FunctionInfo>> functions;
    mutable std::optional<std::vector<StringKind>>   kinds;
    mutable std::optional<std::vector<StringEntry>>  stringEntries;
    mutable std::optional<std::vector<CallSite>>     callGraph;
    mutable std::optional<MetroModules>              modules;
    mutable std::optional<EnvModel>                  envModel;
    mutable std::optional<std::vector<int64_t>>      functionGroups;
    mutable std::map<int64_t, std::unique_ptr<ValueGroup>> valueGroups;
    mutable std::set<int64_t>                        valueGroupsInProgress;
    mutable std::map<uint32_t, std::vector<ResolvedCall>> resolvedCalls;  // per function
    mutable std::map<uint32_t, std::vector<Instruction>> disasmCache;     // per function
    mutable std::map<uint32_t, CFGData>                  cfgCache;        // per function
    mutable std::optional<std::vector<CJSModuleInfo>> cjsModules;
    mutable std::optional<std::vector<BigIntInfo>>   bigInts;
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
    const HBCFileHeader* hdr;
    switch (ver) {
        case 96: hdr = reinterpret_cast<const HBCFileHeader_v96*>(data); break;
        case 95: hdr = reinterpret_cast<const HBCFileHeader_v95*>(data); break;
        case 94: hdr = reinterpret_cast<const HBCFileHeader_v94*>(data); break;
        default: throw UnsupportedVersionError{ver};
    }
    
    validateStringSectionLayout(*hdr, size);
    return hdr;
}

HBCFile HBCFile::open(const std::string& path, ValidationMode mode) {
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
    impl->mode = mode;
    return HBCFile{std::move(impl)};
}

HBCFile HBCFile::from_bytes(const std::string& bytes, ValidationMode mode) {
    auto impl = std::make_shared<Impl>();
    impl->owned.assign(bytes.begin(), bytes.end());
    impl->data = impl->owned.data();
    impl->size = impl->owned.size();
    impl->hdr  = validateHeader(impl->data, impl->size);
    impl->mode = mode;
    return HBCFile{std::move(impl)};
}

HBCFile::HBCFile(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

ValidationMode HBCFile::validation_mode() const { return impl_->mode; }

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
        impl_->strings = parseStringTable(raw_data(), header(), raw_size(), impl_->mode);
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
        impl_->stringEntries = parseStringEntries(raw_data(), header(), stringKinds(),
                                                  raw_size(), impl_->mode);
    return *impl_->stringEntries;
}

const std::vector<CallSite>& HBCFile::call_graph() const {
    if (!impl_->callGraph)
        impl_->callGraph = computeCallGraph(*this);
    return *impl_->callGraph;
}

const std::vector<MetroModule>& HBCFile::recover_modules() const {
    if (!impl_->modules)
        impl_->modules = computeModules(*this);
    return impl_->modules->modules;
}

const std::vector<uint32_t>& HBCFile::metro_entry_modules() const {
    if (!impl_->modules)
        impl_->modules = computeModules(*this);
    return impl_->modules->entries;
}

static const EnvModel& envModel(const HBCFile& self, std::optional<EnvModel>& memo) {
    if (!memo) memo = computeEnvironments(self);
    return *memo;
}

static void checkFunctionIndex(const HBCFile& self, uint32_t f, const char* what) {
    if (f >= self.functions().size())
        throw std::out_of_range(std::string(what) + ": function index " + std::to_string(f) +
                                " out of range (functionCount=" +
                                std::to_string(self.functions().size()) + ")");
}

std::vector<EnvAccess> HBCFile::env_accesses(uint32_t functionIndex) const {
    checkFunctionIndex(*this, functionIndex, "env_accesses");
    return envModel(*this, impl_->envModel).accesses[functionIndex];
}

std::vector<EnvironmentInfo> HBCFile::environments(uint32_t functionIndex) const {
    checkFunctionIndex(*this, functionIndex, "environments");
    return envModel(*this, impl_->envModel).environments[functionIndex];
}

std::optional<EnvKey> HBCFile::captured_environment(uint32_t functionIndex) const {
    checkFunctionIndex(*this, functionIndex, "captured_environment");
    return envModel(*this, impl_->envModel).captured[functionIndex];
}

std::vector<EnvSlotUse> HBCFile::env_slot_uses(const EnvKey& env, uint32_t slot) const {
    const auto& uses = envModel(*this, impl_->envModel).slotUses;
    auto it = uses.find({env, slot});
    return it == uses.end() ? std::vector<EnvSlotUse>{} : it->second;
}

// Value analysis of one group, memoized. nullptr while that group is being
// computed (a cycle through the interop-helper check): callers treat it as unknown.
static const ValueGroup* valueGroup(const HBCFile& self, int64_t group,
                                    std::optional<std::vector<int64_t>>& groupsMemo,
                                    std::map<int64_t, std::unique_ptr<ValueGroup>>& memo,
                                    std::set<int64_t>& inProgress) {
    if (!groupsMemo) groupsMemo = computeFunctionGroups(self);
    auto it = memo.find(group);
    if (it != memo.end()) return it->second.get();
    if (inProgress.count(group)) return nullptr;
    inProgress.insert(group);
    auto result = std::make_unique<ValueGroup>(computeValueGroup(
        self, group, *groupsMemo,
        [&](int64_t g) { return valueGroup(self, g, groupsMemo, memo, inProgress); }));
    inProgress.erase(group);
    return (memo[group] = std::move(result)).get();
}

std::optional<uint32_t> HBCFile::function_module(uint32_t functionIndex) const {
    checkFunctionIndex(*this, functionIndex, "function_module");
    if (!impl_->functionGroups) impl_->functionGroups = computeFunctionGroups(*this);
    const int64_t g = (*impl_->functionGroups)[functionIndex];
    return g < 0 ? std::nullopt : std::optional<uint32_t>(static_cast<uint32_t>(g));
}

std::vector<InstrValues> HBCFile::value_flow(uint32_t functionIndex) const {
    checkFunctionIndex(*this, functionIndex, "value_flow");
    if (!impl_->functionGroups) impl_->functionGroups = computeFunctionGroups(*this);
    const ValueGroup* g = valueGroup(*this, (*impl_->functionGroups)[functionIndex],
                                     impl_->functionGroups, impl_->valueGroups,
                                     impl_->valueGroupsInProgress);
    if (!g) return {};
    auto it = g->flow.find(functionIndex);
    return it == g->flow.end() ? std::vector<InstrValues>{} : it->second;
}

ModuleExports HBCFile::module_exports(uint32_t moduleId) const {
    bool known = false;
    for (const auto& m : recover_modules())
        if (m.moduleId == moduleId) { known = true; break; }
    if (!known) return {};
    if (!impl_->functionGroups) impl_->functionGroups = computeFunctionGroups(*this);
    const ValueGroup* g = valueGroup(*this, moduleId, impl_->functionGroups, impl_->valueGroups,
                                     impl_->valueGroupsInProgress);
    return g ? g->exports : ModuleExports{};
}

AbstractValue HBCFile::resolve_value(const AbstractValue& v) const {
    if (!impl_->functionGroups) impl_->functionGroups = computeFunctionGroups(*this);
    return resolveThroughExports(v, [&](int64_t g) {
        return valueGroup(*this, g, impl_->functionGroups, impl_->valueGroups,
                          impl_->valueGroupsInProgress);
    });
}

const std::vector<ResolvedCall>& HBCFile::resolved_calls(uint32_t functionIndex) const {
    if (auto memo = impl_->resolvedCalls.find(functionIndex); memo != impl_->resolvedCalls.end())
        return memo->second;
    const auto flow = value_flow(functionIndex);  // checks the index
    std::map<uint32_t, const InstrValues*> at;
    for (const auto& iv : flow) at[iv.offset] = &iv;
    std::vector<ResolvedCall> out;
    const size_t n = functions().size();
    for (const auto& instr : disasm(functionIndex)) {
        const std::string& mn = instr.mnemonic;
        const bool isConstruct = mn.rfind("Construct", 0) == 0;
        if (!(mn.rfind("Call", 0) == 0 || isConstruct) || mn.rfind("CallBuiltin", 0) == 0 ||
            mn.rfind("CallDirect", 0) == 0)
            continue;
        auto it = at.find(instr.offset);
        if (it == at.end() || it->second->operands.size() < 2 || !it->second->operands[1]) continue;
        const AbstractValue& callee = *it->second->operands[1];
        const AbstractValue r = resolve_value(callee);
        if (r.kind != AbstractValue::Kind::Closure || r.index >= n) continue;
        ResolvedCall rc;
        rc.offset      = instr.offset;
        rc.callee      = r.index;
        rc.via         = callee.kind == AbstractValue::Kind::Closure ? ResolvedCall::Via::Closure
                                                                     : ResolvedCall::Via::Export;
        rc.isConstruct = isConstruct;
        rc.calleeValue = callee;
        out.push_back(std::move(rc));
    }
    return impl_->resolvedCalls[functionIndex] = std::move(out);
}

std::vector<CallerSite> HBCFile::function_callers(uint32_t functionIndex) const {
    checkFunctionIndex(*this, functionIndex, "function_callers");
    if (!impl_->functionGroups) impl_->functionGroups = computeFunctionGroups(*this);
    const auto& groups = *impl_->functionGroups;
    const int64_t home = groups[functionIndex];

    // Candidate groups: the function's own, plus every module that imports it
    // (reverse dependencyMap edges, transitively: re-exports).
    std::set<int64_t> candidates{home};
    if (home >= 0) {
        std::map<uint32_t, std::vector<uint32_t>> importers;
        for (const auto& m : recover_modules())
            for (const auto& d : m.dependencies)
                if (d) importers[*d].push_back(m.moduleId);
        std::vector<uint32_t> stack{static_cast<uint32_t>(home)};
        while (!stack.empty()) {
            const uint32_t m = stack.back();
            stack.pop_back();
            for (uint32_t i : importers[m])
                if (candidates.insert(i).second) stack.push_back(i);
        }
    }
    std::vector<CallerSite> out;
    for (uint32_t f = 0; f < groups.size(); ++f) {
        if (!candidates.count(groups[f])) continue;
        for (const auto& rc : resolved_calls(f))
            if (rc.callee == functionIndex)
                out.push_back({f, rc.offset, rc.via, rc.isConstruct});
    }
    return out;
}

std::optional<AbstractValue> HBCFile::env_slot_value(const EnvKey& env, uint32_t slot) const {
    checkFunctionIndex(*this, env.function, "env_slot_value");
    if (!impl_->functionGroups) impl_->functionGroups = computeFunctionGroups(*this);
    const ValueGroup* g = valueGroup(*this, (*impl_->functionGroups)[env.function],
                                     impl_->functionGroups, impl_->valueGroups,
                                     impl_->valueGroupsInProgress);
    if (!g) return std::nullopt;
    auto it = g->slots.find({env, slot});
    if (it == g->slots.end()) return std::nullopt;
    return it->second;
}

std::vector<CJSModuleInfo> HBCFile::cjs_modules() const {
    if (!impl_->cjsModules) {
        uint32_t sectionOffset = 0;
        for (const auto& s : section_map())
            if (s.name == "cjs module table") { sectionOffset = s.offset; break; }

        const bool staticallyResolved = (header().options >> 1) & 1;
        impl_->cjsModules = parseCJSModuleTable(
            raw_data(), sectionOffset, header().cjsModuleCount,
            staticallyResolved, strings());
    }
    return *impl_->cjsModules;
}

const std::vector<BigIntInfo>& HBCFile::bigints() const {
    if (!impl_->bigInts) {
        uint32_t tableOffset = 0, storageOffset = 0;
        for (const auto& sec : section_map()) {
            if (sec.name == "bigint table")   tableOffset   = sec.offset;
            if (sec.name == "bigint storage") storageOffset = sec.offset;
        }
        impl_->bigInts = parseBigIntTable(
            raw_data(), tableOffset, header().bigIntCount,
            storageOffset, header().bigIntStorageSize);
    }
    return *impl_->bigInts;
}

std::vector<uint32_t> HBCFile::identifier_hashes() const {
    const uint32_t off   = stringSectionOffsets(header()).identifierHashes;
    const uint32_t count = header().identifierCount;
    if (uint64_t(off) + uint64_t(count) * 4 > raw_size())
        throw std::runtime_error("Identifier Hash Table extends past the end of the file");
    std::vector<uint32_t> out(count);
    if (count) std::memcpy(out.data(), raw_data() + off, size_t(count) * 4);
    return out;
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
const std::vector<Instruction>& HBCFile::disasm(uint32_t functionIndex) const {
    const auto& funcs = functions();

    if (functionIndex >= funcs.size()) {
        throw std::out_of_range(
            "disasm: function index " + std::to_string(functionIndex) +
            " out of range (functionCount=" + std::to_string(funcs.size()) + ")");
    }

    if (auto it = impl_->disasmCache.find(functionIndex); it != impl_->disasmCache.end())
        return it->second;

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

    return impl_->disasmCache.emplace(functionIndex, std::move(result)).first->second;
}

const CFGData& HBCFile::cfg(uint32_t functionIndex) const {
    const auto& funcs = functions();
    if (functionIndex >= funcs.size()) {
        throw std::out_of_range(
            "cfg: function index " + std::to_string(functionIndex) +
            " out of range (functionCount=" + std::to_string(funcs.size()) + ")");
    }
    if (auto it = impl_->cfgCache.find(functionIndex); it != impl_->cfgCache.end())
        return it->second;
    const FunctionInfo& fi = funcs[functionIndex];
    const auto& instrs = disasm(functionIndex);
    auto data = buildCFG(instrs, fi.bytecodeSizeInBytes, version(), fi.exceptionHandlers);
    return impl_->cfgCache.emplace(functionIndex, std::move(data)).first->second;
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
//   filename storage                      filenameStorageSize bytes, NOT NUL-separated:
//     consecutive names can sit back-to-back with no terminator (verified on a
//     real filenameCount == 2 file -- see source_files(), which reads the
//     filename table's (offset, length) pairs rather than scanning for \0)
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

    const uint32_t fileSize    = static_cast<uint32_t>(raw_size());
    const uint32_t storageSize = layout.hdr.filenameStorageSize;

    if (storageSize == 0 || layout.filenameStorageStart + storageSize > fileSize)
        return {};

    // Filename table: filenameCount x { offset:u32, length:u32 } into the
    // filename storage blob, right before it (see the DebugLayout comment).
    constexpr uint32_t HEADER_SIZE = 7 * 4;
    const uint32_t tableStart = impl_->hdr->debugInfoOffset + HEADER_SIZE;
    const uint8_t* storage    = raw_data() + layout.filenameStorageStart;

    std::vector<std::string> result;
    result.reserve(layout.hdr.filenameCount);
    for (uint32_t i = 0; i < layout.hdr.filenameCount; ++i) {
        uint32_t off = 0, len = 0;
        std::memcpy(&off, raw_data() + tableStart + i * 8,     4);
        std::memcpy(&len, raw_data() + tableStart + i * 8 + 4, 4);
        if (static_cast<uint64_t>(off) + len > storageSize) {
            result.emplace_back();  // corrupt entry: keep positional alignment, don't crash
            continue;
        }
        result.emplace_back(reinterpret_cast<const char*>(storage + off), len);
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
