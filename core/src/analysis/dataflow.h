// Private header: building blocks shared by the register-tracking analyses
// (Metro modules, closure environments, value analysis).
//
// - writtenRegisters(): which registers an instruction overwrites. The opcode
//   tables (core/src/format/) carry no dst metadata, so this encodes the HBC
//   v94-96 conventions: the first register operand is the destination except
//   for the families that only read it (Put*, Store*, jumps, Ret, Throw, ...),
//   and a few opcodes write more than one register (BytecodeList.def at
//   hermes-2023-09-26-RNv0.73.0: GetPNameList, GetNextPName, ResumeGenerator,
//   IteratorBegin, IteratorNext). Over-reporting only costs precision.
// - forwardDataflow(): forward dataflow to a fixpoint over a CFG, with a
//   caller-supplied state, transfer and join. Every CFG edge carries the
//   state, GeneratorResume included (SaveGenerator saves the frame registers,
//   so they are intact at the resume point).
#pragma once

#include "hbc/hbc.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace hbc {

inline bool isRegisterOperand(const Operand& op) {
    return op.type == Operand::Type::Reg8 || op.type == Operand::Type::Reg32;
}

// Instructions whose first register operand is read, not written.
inline bool firstRegisterIsRead(const std::string& mn) {
    return mn.rfind("Put", 0) == 0 || mn.rfind("TryPut", 0) == 0 ||
           mn.rfind("Store", 0) == 0 || mn.rfind("J", 0) == 0 ||
           mn == "Ret" || mn == "Throw" || mn == "SwitchImm" || mn == "IteratorClose";
}

inline std::vector<uint32_t> writtenRegisters(const Instruction& instr) {
    std::vector<uint32_t> out;
    const auto& ops = instr.operands;
    const std::string& mn = instr.mnemonic;
    auto add = [&](size_t i) {
        if (i < ops.size() && isRegisterOperand(ops[i])) out.push_back(ops[i].u32);
    };
    if (ops.empty() || firstRegisterIsRead(mn)) return out;
    add(0);
    if (mn == "GetPNameList") { add(2); add(3); }
    else if (mn == "GetNextPName") { add(3); }
    else if (mn == "ResumeGenerator" || mn == "IteratorBegin" || mn == "IteratorNext") { add(1); }
    return out;
}

// Runs a forward dataflow over `cfg` and returns the IN state of every block
// (indexed by block id; nullopt for blocks never reached from the entry).
//   transfer(block, state&)   applies the block's instructions to `state`
//   join(into&, from) -> bool merges `from` into `into`, true if `into` changed
// `join` must be monotone (e.g. keep only entries equal on both sides) so the
// iteration terminates.
template <typename State, typename Transfer, typename Join>
std::vector<std::optional<State>> forwardDataflow(
    const CFGData& cfg, const State& entryState, Transfer&& transfer, Join&& join)
{
    std::vector<std::optional<State>> in(cfg.blocks.size());
    if (cfg.blocks.empty()) return in;

    std::vector<size_t> indexOf;  // block id -> index in cfg.blocks
    for (size_t i = 0; i < cfg.blocks.size(); ++i) {
        const uint32_t id = cfg.blocks[i].id;
        if (indexOf.size() <= id) indexOf.resize(id + 1, SIZE_MAX);
        indexOf[id] = i;
    }
    auto idx = [&](uint32_t id) -> size_t {
        return id < indexOf.size() ? indexOf[id] : SIZE_MAX;
    };

    const size_t entry = idx(cfg.entryId);
    if (entry == SIZE_MAX) return in;
    in[entry] = entryState;

    std::deque<size_t> work{entry};
    std::vector<bool> queued(cfg.blocks.size(), false);
    queued[entry] = true;
    while (!work.empty()) {
        const size_t b = work.front();
        work.pop_front();
        queued[b] = false;

        State out = *in[b];
        transfer(cfg.blocks[b], out);
        for (const auto& edge : cfg.blocks[b].successors) {
            const size_t s = idx(edge.targetId);
            if (s == SIZE_MAX) continue;
            bool changed;
            if (!in[s]) { in[s] = out; changed = true; }
            else        { changed = join(*in[s], out); }
            if (changed && !queued[s]) { work.push_back(s); queued[s] = true; }
        }
    }
    // Reorder by block id (blocks are usually already in id order).
    std::vector<std::optional<State>> byId(indexOf.size());
    for (size_t i = 0; i < cfg.blocks.size(); ++i) byId[cfg.blocks[i].id] = std::move(in[i]);
    return byId;
}

// Join for map-like states: keep only the keys present on both sides with equal
// values. Returns true if `into` lost an entry.
template <typename Map>
bool intersectEqual(Map& into, const Map& from) {
    bool changed = false;
    for (auto it = into.begin(); it != into.end();) {
        auto f = from.find(it->first);
        if (f == from.end() || !(f->second == it->second)) {
            it = into.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

} // namespace hbc
