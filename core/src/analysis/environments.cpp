// Closure environment model (HBCFile::env_accesses() and friends).
//
// Two passes:
//  1. per function, a forward dataflow over its CFG tracks which environment
//     each register holds, symbolically: Own(offset) after
//     Create[Inner]Environment, Captured(level) after GetEnvironment. Recorded:
//     the environments it creates (inner ones with their symbolic parent), the
//     closures it creates with the captured environment, every slot access.
//  2. a walk of the closure-creation tree from the global function resolves the
//     symbolic references: the environment a function captured is the one its
//     creator passed to CreateClosure*; Captured(level) is that environment
//     followed by `level` parent links (see EnvKey in hbc.h for the semantics).
// Functions never created by a closure instruction keep their own-environment
// accesses resolved and the captured ones unresolved.

#include "analysis/analysis.h"
#include "analysis/dataflow.h"
#include "format/opcode_categories.h"

#include <algorithm>
#include <map>
#include <optional>

namespace hbc {

namespace {

struct EnvRef {
    enum class Kind : uint8_t { Own, Captured } kind = Kind::Own;
    uint32_t value = 0;  // Own: offset of the creating instruction; Captured: level
    bool operator==(const EnvRef& o) const { return kind == o.kind && value == o.value; }
};

using EnvState = std::map<uint32_t, EnvRef>;  // register -> environment

bool isRelevant(const std::string& mn) {
    return isClosureOrGenerator(mn) || isEnvLoad(mn) || isEnvStore(mn) ||
           mn == "CreateEnvironment" || mn == "CreateInnerEnvironment" ||
           mn == "GetEnvironment";
}

void applyEnv(const Instruction& instr, EnvState& st) {
    const auto& ops = instr.operands;
    const std::string& mn = instr.mnemonic;
    std::optional<EnvRef> v;
    if (mn == "CreateEnvironment" || mn == "CreateInnerEnvironment") {
        v = EnvRef{EnvRef::Kind::Own, instr.offset};
    } else if (mn == "GetEnvironment" && ops.size() >= 2) {
        v = EnvRef{EnvRef::Kind::Captured, ops[1].u32};
    } else if ((mn == "Mov" || mn == "MovLong") && ops.size() >= 2) {
        auto it = st.find(ops[1].u32);
        if (it != st.end()) v = it->second;
    }
    const auto written = writtenRegisters(instr);
    for (uint32_t r : written) st.erase(r);
    if (v && !written.empty()) st[written.front()] = *v;
}

struct LocalFacts {
    struct OwnEnv { uint32_t offset; bool isInner; std::optional<EnvRef> parent; };
    struct Closure { uint32_t child; std::optional<EnvRef> env; };
    struct Access { uint32_t offset; bool isStore; uint32_t slot; std::optional<EnvRef> env; };
    std::vector<OwnEnv>  ownEnvs;   // in offset order
    std::vector<Closure> closures;
    std::vector<Access>  accesses;  // in offset order
};

LocalFacts localFacts(const HBCFile& self, uint32_t f) {
    LocalFacts facts;
    const auto& instrs = self.disasm(f);
    bool relevant = false;
    for (const auto& i : instrs)
        if (isRelevant(i.mnemonic)) { relevant = true; break; }
    if (!relevant) return facts;

    const CFGData& cfg = self.cfg(f);
    const auto in = forwardDataflow(
        cfg, EnvState{},
        [](const BasicBlock& bb, EnvState& st) {
            for (const auto& instr : bb.instructions) applyEnv(instr, st);
        },
        [](EnvState& into, const EnvState& from) { return intersectEqual(into, from); });

    auto lookup = [](const EnvState& st, const Operand& op) -> std::optional<EnvRef> {
        if (!isRegisterOperand(op)) return std::nullopt;
        auto it = st.find(op.u32);
        if (it == st.end()) return std::nullopt;
        return it->second;
    };

    for (const auto& bb : cfg.blocks) {
        if (bb.id >= in.size() || !in[bb.id]) continue;  // unreachable block
        EnvState st = *in[bb.id];
        for (const auto& instr : bb.instructions) {
            const auto& mn  = instr.mnemonic;
            const auto& ops = instr.operands;
            if (mn == "CreateEnvironment") {
                facts.ownEnvs.push_back({instr.offset, false, std::nullopt});
            } else if (mn == "CreateInnerEnvironment" && ops.size() >= 2) {
                facts.ownEnvs.push_back({instr.offset, true, lookup(st, ops[1])});
            } else if (isClosureOrGenerator(mn) && ops.size() >= 3) {
                facts.closures.push_back({ops[2].u32, lookup(st, ops[1])});
            } else if (isEnvLoad(mn) && ops.size() >= 3) {
                facts.accesses.push_back({instr.offset, false, ops[2].u32, lookup(st, ops[1])});
            } else if (isEnvStore(mn) && ops.size() >= 3) {
                facts.accesses.push_back({instr.offset, true, ops[1].u32, lookup(st, ops[0])});
            }
            applyEnv(instr, st);
        }
    }
    auto byOffset = [](const auto& a, const auto& b) { return a.offset < b.offset; };
    std::sort(facts.ownEnvs.begin(), facts.ownEnvs.end(), byOffset);
    std::sort(facts.accesses.begin(), facts.accesses.end(), byOffset);
    return facts;
}

} // namespace

EnvModel computeEnvironments(const HBCFile& self) {
    const uint32_t n = static_cast<uint32_t>(self.functions().size());
    EnvModel model;
    model.accesses.resize(n);
    model.environments.resize(n);
    model.captured.resize(n);

    std::vector<LocalFacts> facts(n);
    for (uint32_t f = 0; f < n; ++f) facts[f] = localFacts(self, f);

    // Creator of each function: (parent function, symbolic env). Step 1 of B30
    // verified one creating site per function on real bundles; a second site
    // keeps the first.
    struct Creator { uint32_t parent; std::optional<EnvRef> env; };
    std::vector<std::optional<Creator>> creator(n);
    for (uint32_t f = 0; f < n; ++f)
        for (const auto& c : facts[f].closures)
            if (c.child < n && !creator[c.child]) creator[c.child] = Creator{f, c.env};

    // Resolved state.
    std::map<EnvKey, std::optional<EnvKey>> parentOf;   // nullopt value: no parent
    std::map<EnvKey, bool>                  parentKnown;
    // captured[f]: known + optional key (nullopt = no environment, global scope)
    std::vector<bool>                    capturedKnown(n, false);
    std::vector<std::optional<EnvKey>>   capturedEnv(n);

    auto resolve = [&](uint32_t f, const std::optional<EnvRef>& ref) -> std::optional<EnvKey> {
        if (!ref) return std::nullopt;
        if (ref->kind == EnvRef::Kind::Own) return EnvKey{f, ref->value};
        if (!capturedKnown[f] || !capturedEnv[f]) return std::nullopt;
        EnvKey e = *capturedEnv[f];
        for (uint32_t lv = 0; lv < ref->value; ++lv) {
            auto known = parentKnown.find(e);
            if (known == parentKnown.end() || !known->second) return std::nullopt;
            const auto& p = parentOf[e];
            if (!p) return std::nullopt;
            e = *p;
        }
        return e;
    };

    auto processFunction = [&](uint32_t f) {
        for (const auto& own : facts[f].ownEnvs) {
            const EnvKey key{f, own.offset};
            EnvironmentInfo info;
            info.env     = key;
            info.isInner = own.isInner;
            if (!own.isInner) {
                info.parentKnown = capturedKnown[f];
                if (info.parentKnown && capturedEnv[f]) {
                    info.hasParent = true;
                    info.parent    = *capturedEnv[f];
                }
            } else if (auto p = resolve(f, own.parent)) {
                info.parentKnown = info.hasParent = true;
                info.parent      = *p;
            }
            parentKnown[key] = info.parentKnown;
            parentOf[key]    = info.hasParent ? std::optional<EnvKey>(info.parent) : std::nullopt;
            model.environments[f].push_back(info);
        }
        for (const auto& a : facts[f].accesses) {
            EnvAccess acc;
            acc.offset  = a.offset;
            acc.isStore = a.isStore;
            acc.slot    = a.slot;
            if (auto e = resolve(f, a.env)) { acc.resolved = true; acc.env = *e; }
            model.accesses[f].push_back(acc);
        }
        model.captured[f] = capturedKnown[f] ? capturedEnv[f] : std::nullopt;
    };

    // Walk the creation tree from the global function (captures no environment).
    std::vector<std::vector<uint32_t>> children(n);
    for (uint32_t f = 0; f < n; ++f)
        if (creator[f]) children[creator[f]->parent].push_back(f);

    std::vector<bool> done(n, false);
    std::vector<uint32_t> queue;
    const uint32_t global = self.header().globalCodeIndex;
    if (global < n) {
        capturedKnown[global] = true;
        queue.push_back(global);
    }
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const uint32_t f = queue[qi];
        if (done[f]) continue;
        done[f] = true;
        processFunction(f);
        for (uint32_t c : children[f]) {
            if (done[c]) continue;
            if (auto e = resolve(f, creator[c]->env)) {
                capturedKnown[c] = true;
                capturedEnv[c]   = *e;
            }
            queue.push_back(c);
        }
    }
    // Functions outside the tree: own environments only.
    for (uint32_t f = 0; f < n; ++f)
        if (!done[f]) processFunction(f);

    for (uint32_t f = 0; f < n; ++f)
        for (const auto& a : model.accesses[f])
            if (a.resolved)
                model.slotUses[{a.env, a.slot}].push_back({f, a.offset, a.isStore});
    return model;
}

} // namespace hbc
