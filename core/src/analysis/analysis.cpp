#include "analysis/analysis.h"
#include "analysis/dataflow.h"
#include "hbc/hbc.h"
#include "util/slp.h"
#include <algorithm>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace hbc {

namespace {

// Calls fn(function, instruction) for every instruction of every function.
template <typename Fn>
void forEachInstruction(const HBCFile& f, Fn&& fn) {
    for (const auto& func : f.functions())
        for (const auto& instr : f.disasm(func.index))
            fn(func, instr);
}

bool isCreateClosure(const std::string& mnemonic) {
    return mnemonic == "CreateClosure" || mnemonic == "CreateClosureLongIndex";
}

} // namespace

namespace {

// New{Array,Object}WithBuffer[Long] operands (v94-96): dst, sizeHint, count,
// then one buffer offset (array) or two (object: key buffer, value buffer).
// An object's bufferIdx is the value buffer, the editable one. False for any
// other instruction and for a truncated operand list.
bool literalSiteFrom(const FunctionInfo& func, const Instruction& instr, LiteralSite& site) {
    const bool isArr = instr.mnemonic == "NewArrayWithBuffer" ||
                       instr.mnemonic == "NewArrayWithBufferLong";
    const bool isObj = instr.mnemonic == "NewObjectWithBuffer" ||
                       instr.mnemonic == "NewObjectWithBufferLong";
    if (!isArr && !isObj) return false;

    site = LiteralSite{};
    site.functionIndex     = func.index;
    site.instructionOffset = instr.offset;
    site.mnemonic          = instr.mnemonic;
    site.isObject          = isObj;

    if (isArr) {
        if (instr.operands.size() < 4) return false;  // truncated
        site.count        = instr.operands[2].u32;
        site.bufferIdx    = instr.operands[3].u32;
        site.keyBufferIdx = 0;
    } else {
        if (instr.operands.size() < 5) return false;
        site.count        = instr.operands[2].u32;
        site.keyBufferIdx = instr.operands[3].u32;
        site.bufferIdx    = instr.operands[4].u32;
    }
    return true;
}

// The strings referenced by one literal buffer segment: for each distinct
// string, its first element and how many elements hold it.
struct LiteralStringHit {
    uint32_t stringIndex;
    uint32_t firstElement;
    uint32_t elementCount;
};

std::vector<LiteralStringHit> stringsInLiteral(
    const std::string& buffer, uint32_t offset, uint32_t count, size_t stringCount)
{
    std::vector<LiteralStringHit> hits;
    auto elems = decodeSLP(buffer, offset, count);
    if (!elems) return hits;  // truncated or wrong offset: no reference reported
    std::unordered_map<uint32_t, size_t> slot;  // stringIndex -> position in hits
    for (uint32_t i = 0; i < elems->size(); ++i) {
        const SLPElement& e = (*elems)[i];
        if (!isSLPStringTag(e.tag) || e.stringIndex >= stringCount) continue;
        auto [it, isNew] = slot.try_emplace(e.stringIndex, hits.size());
        if (isNew) hits.push_back({e.stringIndex, i, 1});
        else ++hits[it->second].elementCount;
    }
    return hits;
}

} // namespace

// Reference sites of every string: StringID operands, and string elements of
// the array/object literal buffers a New*WithBuffer instruction points at.
// Grouped by string; only strings with at least one reference are returned, in
// index order. Within a string, sites follow the instruction order.
std::vector<StringXRef> HBCFile::string_xrefs() const {
    const auto& strs = strings();
    std::vector<std::vector<XRefSite>> sitesByString(strs.size());

    // Sites commonly share a buffer segment (identical literals in several
    // functions): decode each (buffer, offset, count) once.
    using SegmentKey = std::tuple<const std::string*, uint32_t, uint32_t>;
    std::map<SegmentKey, std::vector<LiteralStringHit>> segments;
    auto hitsOf = [&](const std::string& buffer, uint32_t offset, uint32_t count)
        -> const std::vector<LiteralStringHit>& {
        auto key = SegmentKey{&buffer, offset, count};
        auto it = segments.find(key);
        if (it == segments.end())
            it = segments.emplace(key, stringsInLiteral(buffer, offset, count, strs.size())).first;
        return it->second;
    };
    auto addLiteralSites = [&](const LiteralSite& lit, XRefKind kind,
                               const std::vector<LiteralStringHit>& hits) {
        for (const auto& h : hits)
            sitesByString[h.stringIndex].push_back(
                {lit.functionIndex, lit.instructionOffset, lit.mnemonic,
                 kind, h.firstElement, h.elementCount});
    };

    forEachInstruction(*this, [&](const FunctionInfo& func, const Instruction& instr) {
        for (const auto& op : instr.operands)
            if (op.type == Operand::Type::StringID && op.u32 < strs.size())
                sitesByString[op.u32].push_back({func.index, instr.offset, instr.mnemonic});

        LiteralSite lit;
        if (!literalSiteFrom(func, instr, lit)) return;
        if (lit.isObject) {
            addLiteralSites(lit, XRefKind::ObjectKey,
                            hitsOf(obj_key_buffer(), lit.keyBufferIdx, lit.count));
            addLiteralSites(lit, XRefKind::ObjectValue,
                            hitsOf(obj_value_buffer(), lit.bufferIdx, lit.count));
        } else {
            addLiteralSites(lit, XRefKind::ArrayElement,
                            hitsOf(array_buffer(), lit.bufferIdx, lit.count));
        }
    });

    std::vector<StringXRef> result;
    for (uint32_t i = 0; i < static_cast<uint32_t>(strs.size()); ++i)
        if (!sitesByString[i].empty())
            result.push_back({i, strs[i], std::move(sitesByString[i])});
    return result;
}

// Call sites in three categories:
//   CallDirect*             callee known, isDirect = true
//   CreateClosure*          callee known, isDirect = false (static reference)
//   Call*, Construct*       callee 0xFFFFFFFF, isDirect = false (indirect)
std::vector<CallSite> computeCallGraph(const HBCFile& self) {
    static const std::unordered_set<std::string> DIRECT_CALL = {
        "CallDirect", "CallDirectLongIndex"
    };
    static const std::unordered_set<std::string> STATIC_REF = {
        "CreateClosure",              "CreateClosureLongIndex",
        "CreateGeneratorClosure",     "CreateGeneratorClosureLongIndex",
        "CreateAsyncClosure",         "CreateAsyncClosureLongIndex",
        "CreateGenerator",            "CreateGeneratorLongIndex"
    };
    static const std::unordered_set<std::string> INDIRECT = {
        "Call", "Call1", "Call2", "Call3", "Call4", "CallLong",
        "CallBuiltin", "CallBuiltinLong",
        "Construct", "ConstructLong"
    };

    const size_t functionCount = self.functions().size();
    const uint32_t UNRESOLVED = std::numeric_limits<uint32_t>::max();
    std::vector<CallSite> result;

    forEachInstruction(self, [&](const FunctionInfo& func, const Instruction& instr) {
        const bool isDirect = DIRECT_CALL.count(instr.mnemonic) > 0;
        if ((isDirect || STATIC_REF.count(instr.mnemonic)) && instr.operands.size() >= 3) {
            const uint32_t calleeIdx = instr.operands[2].u32;
            if (calleeIdx < functionCount)
                result.push_back({func.index, calleeIdx, instr.offset, isDirect, instr.mnemonic});
        } else if (INDIRECT.count(instr.mnemonic)) {
            result.push_back({func.index, UNRESOLVED, instr.offset, false, instr.mnemonic});
        }
    });
    return result;
}

namespace {

// Abstract value of a register in the global function (computeModules).
struct RegValue {
    enum class Kind { Unknown, GlobalObject, Global, Closure, Int, Array };
    Kind kind = Kind::Unknown;
    std::string           name;   // Global: the global's name
    uint32_t              num = 0;  // Closure: function index; Int: value
    std::vector<std::optional<uint32_t>> ids;  // Array: integer (or null) elements
    bool                  idsKnown = true;  // Array: false if another element type / not decodable
};

// Metro registers its runtime as `__d`/`__r`, prefixed by __METRO_GLOBAL_PREFIX__
// (empty in a React Native bundle).
bool isMetroGlobal(const std::string& name, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
}

} // namespace

// Finds Metro modules in the global function. A Metro bundle registers each
// module with `__d(factory, moduleId, dependencyMap)` and starts the app with
// `__r(moduleId)`; hermesc -O compiles them to
//   TryGetById      rD, rG, "__d"
//   CreateClosure   rF, env, K          factory = func[K]
//   LoadConst*      rI, N               moduleId = N
//   NewArrayWithBuffer rA, n, n, off    dependencyMap (SLP integers/nulls) | NewArray rA, 0
//   Call4           rX, rD, this, rF, rI, rA
//   ...
//   TryGetById      rR, rG, "__r"
//   Call2           rX, rR, this, rI2   entry point, rI2 loaded possibly far earlier
// Operands are matched by register value, not by position: a linear abstract
// interpretation of the global function tracks what each register holds
// (closure, integer, integer array, global __d/__r). Any other instruction
// clobbers the register it writes; the state is reset at every branch target and
// exception handler, so values never flow across control-flow joins.
MetroModules computeModules(const HBCFile& self) {
    MetroModules result;
    const auto& funcs   = self.functions();
    const auto& strings = self.strings();
    const auto& arrBuf  = self.array_buffer();
    const auto instrs   = self.disasm(self.header().globalCodeIndex);

    // Branch targets and handler starts: values are not propagated across them.
    std::unordered_set<uint32_t> joins;
    for (const auto& instr : instrs) {
        for (const auto& op : instr.operands)
            if (op.type == Operand::Type::Addr8 || op.type == Operand::Type::Addr32)
                joins.insert(static_cast<uint32_t>(static_cast<int32_t>(instr.offset) + op.i32));
        for (uint32_t t : instr.switchTargets) joins.insert(t);
    }

    std::unordered_map<uint32_t, RegValue> regs;
    auto get = [&](const Operand& op) -> const RegValue* {
        if (!isRegisterOperand(op)) return nullptr;
        auto it = regs.find(op.u32);
        return it == regs.end() ? nullptr : &it->second;
    };
    auto is = [&](const RegValue* v, RegValue::Kind k) { return v && v->kind == k; };

    for (const auto& instr : instrs) {
        const std::string& mn  = instr.mnemonic;
        const auto&        ops = instr.operands;
        if (joins.count(instr.offset) || mn == "Catch") regs.clear();

        // Calls of __d / __r. Call4: dst, callee, this, a1, a2, a3.
        if ((mn == "Call4" && ops.size() == 6) || (mn == "Call2" && ops.size() == 4) ||
            (mn == "Call3" && ops.size() == 5)) {
            const RegValue* callee = get(ops[1]);
            if (is(callee, RegValue::Kind::Global)) {
                if (mn == "Call4" && isMetroGlobal(callee->name, "__d")) {
                    const RegValue* f  = get(ops[3]);
                    const RegValue* id = get(ops[4]);
                    const RegValue* dm = get(ops[5]);
                    if (is(f, RegValue::Kind::Closure) && is(id, RegValue::Kind::Int)) {
                        ModuleInfo mod;
                        mod.functionIndex = f->num;
                        mod.moduleId      = id->num;
                        mod.defineOffset  = instr.offset;
                        if (is(dm, RegValue::Kind::Array) && dm->idsKnown) {
                            mod.dependencies = dm->ids;
                        } else {
                            mod.dependenciesKnown = false;
                        }
                        result.modules.push_back(std::move(mod));
                    }
                } else if (isMetroGlobal(callee->name, "__r")) {
                    const RegValue* id = get(ops[3]);
                    if (is(id, RegValue::Kind::Int)) result.entries.push_back(id->num);
                }
            }
        }

        // Transfer function: the modelled value of the destination is computed
        // from the operands first, then every written register is clobbered
        // and the destination gets the value (if any).
        const auto written = writtenRegisters(instr);
        if (written.empty()) continue;
        const uint32_t dst = written.front();
        RegValue v;  // Unknown unless modelled below
        if (mn == "GetGlobalObject") {
            v.kind = RegValue::Kind::GlobalObject;
        } else if ((mn == "TryGetById" || mn == "TryGetByIdLong" || mn == "GetById" ||
                    mn == "GetByIdShort" || mn == "GetByIdLong") && ops.size() >= 4) {
            const RegValue* obj = get(ops[1]);
            // TryGetById is only emitted for global-object lookups.
            const bool onGlobal = mn.rfind("Try", 0) == 0 || is(obj, RegValue::Kind::GlobalObject);
            const uint32_t sid = ops[3].u32;
            if (onGlobal && sid < strings.size() &&
                (isMetroGlobal(strings[sid], "__d") || isMetroGlobal(strings[sid], "__r"))) {
                v.kind = RegValue::Kind::Global;
                v.name = strings[sid];
            }
        } else if (isCreateClosure(mn) && ops.size() >= 3) {
            if (ops[2].u32 < funcs.size()) {
                v.kind = RegValue::Kind::Closure;
                v.num  = ops[2].u32;
            }
        } else if (mn == "LoadConstZero") {
            v.kind = RegValue::Kind::Int;
        } else if ((mn == "LoadConstUInt8" || mn == "LoadConstInt") && ops.size() >= 2) {
            if (mn == "LoadConstUInt8" || ops[1].i32 >= 0) {
                v.kind = RegValue::Kind::Int;
                v.num  = ops[1].u32;
            }
        } else if (mn == "NewArray" && ops.size() >= 2 && ops[1].u32 == 0) {
            v.kind = RegValue::Kind::Array;
        } else if ((mn == "NewArrayWithBuffer" || mn == "NewArrayWithBufferLong") &&
                   ops.size() >= 4) {
            v.kind = RegValue::Kind::Array;
            auto elems = decodeSLP(arrBuf, ops[3].u32, ops[2].u32);
            if (!elems) {
                v.idsKnown = false;
            } else {
                for (const auto& e : *elems) {
                    if (e.tag == SLPTag::Null) {
                        v.ids.push_back(std::nullopt);
                    } else if (e.tag == SLPTag::Integer && e.integer >= 0) {
                        v.ids.push_back(static_cast<uint32_t>(e.integer));
                    } else {
                        v.idsKnown = false;
                        break;
                    }
                }
                if (!v.idsKnown) v.ids.clear();
            }
        } else if ((mn == "Mov" || mn == "MovLong") && ops.size() >= 2) {
            if (const RegValue* src = get(ops[1])) v = *src;
        }

        for (uint32_t r : written) regs.erase(r);
        if (v.kind != RegValue::Kind::Unknown) regs[dst] = std::move(v);
    }
    return result;
}

// New{Array,Object}WithBuffer[Long] sites (see literalSiteFrom).
std::vector<LiteralSite> computeLiteralSites(const HBCFile& self) {
    std::vector<LiteralSite> result;
    forEachInstruction(self, [&](const FunctionInfo& func, const Instruction& instr) {
        LiteralSite site;
        if (literalSiteFrom(func, instr, site)) result.push_back(std::move(site));
    });
    return result;
}

} // namespace hbc
