#include "analysis/analysis.h"
#include "hbc/hbc.h"
#include <algorithm>
#include <limits>
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

// Reference sites of every StringID operand, grouped by string. Only strings
// with at least one reference are returned, in index order.
std::vector<StringXRef> HBCFile::string_xrefs() const {
    const auto& strs = strings();
    std::vector<std::vector<XRefSite>> sitesByString(strs.size());

    forEachInstruction(*this, [&](const FunctionInfo& func, const Instruction& instr) {
        for (const auto& op : instr.operands)
            if (op.type == Operand::Type::StringID && op.u32 < strs.size())
                sitesByString[op.u32].push_back({func.index, instr.offset, instr.mnemonic});
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

// Finds Metro modules in the global function, from the pattern
// __d(factory, moduleId, deps):
//   TryGetById "__d"
//   CreateClosure K              factory = func[K]
//   LoadConstZero/UInt8/Int N    moduleId = N, within 5 instructions
//   NewArrayWithBuffer ...
//   Call4 ...                    within 5 instructions after the moduleId
// The Call4 check avoids false modules from LoadConst* used as ordinary
// arguments (e.g. debug_func(2, ...)).
std::vector<ModuleInfo> computeModules(const HBCFile& self) {
    const auto& funcs = self.functions();
    const auto instrs = self.disasm(self.header().globalCodeIndex);

    std::vector<ModuleInfo> result;
    const size_t N = instrs.size();
    const uint32_t NONE = std::numeric_limits<uint32_t>::max();

    for (size_t i = 0; i < N; ++i) {
        const auto& instr = instrs[i];
        if (!isCreateClosure(instr.mnemonic) || instr.operands.size() < 3) continue;

        const uint32_t factoryIdx = instr.operands[2].u32;
        if (factoryIdx >= funcs.size()) continue;

        uint32_t moduleId    = NONE;
        size_t   moduleIdPos = N;
        for (size_t j = i + 1; j < std::min(N, i + 6); ++j) {
            const auto& next = instrs[j];
            if (next.mnemonic == "LoadConstZero") {
                moduleId    = 0;
                moduleIdPos = j;
                break;
            }
            if ((next.mnemonic == "LoadConstUInt8" || next.mnemonic == "LoadConstInt")
                && next.operands.size() >= 2)
            {
                moduleId    = next.operands[1].u32;
                moduleIdPos = j;
                break;
            }
        }
        if (moduleId == NONE) continue;

        bool hasCall = false;
        for (size_t k = moduleIdPos + 1; k < std::min(N, moduleIdPos + 6); ++k) {
            const std::string& mn = instrs[k].mnemonic;
            if (mn == "Call4" || mn == "CallLong") {
                hasCall = true;
                break;
            }
            if (isCreateClosure(mn)) break;  // the next module starts here
        }

        if (hasCall)
            result.push_back({factoryIdx, moduleId});
    }
    return result;
}

// New{Array,Object}WithBuffer[Long] sites. Operands (v94-96): dst, sizeHint,
// count, then one buffer index (array) or two (object: key buffer, value
// buffer). An object's bufferIdx is the value buffer, the editable one.
std::vector<LiteralSite> computeLiteralSites(const HBCFile& self) {
    std::vector<LiteralSite> result;

    forEachInstruction(self, [&](const FunctionInfo& func, const Instruction& instr) {
        const bool isArr = instr.mnemonic == "NewArrayWithBuffer" ||
                           instr.mnemonic == "NewArrayWithBufferLong";
        const bool isObj = instr.mnemonic == "NewObjectWithBuffer" ||
                           instr.mnemonic == "NewObjectWithBufferLong";
        if (!isArr && !isObj) return;

        LiteralSite site{};
        site.functionIndex     = func.index;
        site.instructionOffset = instr.offset;
        site.mnemonic          = instr.mnemonic;
        site.isObject          = isObj;

        if (isArr) {
            if (instr.operands.size() < 4) return;  // truncated
            site.count        = instr.operands[2].u32;
            site.bufferIdx    = instr.operands[3].u32;
            site.keyBufferIdx = 0;
        } else {
            if (instr.operands.size() < 5) return;
            site.count        = instr.operands[2].u32;
            site.keyBufferIdx = instr.operands[3].u32;
            site.bufferIdx    = instr.operands[4].u32;
        }
        result.push_back(std::move(site));
    });
    return result;
}

} // namespace hbc
