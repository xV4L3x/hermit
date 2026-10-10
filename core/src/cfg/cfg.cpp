// Algorithm:
//   1. Leaders: the first instruction, every jump target, and the instruction
//      after every jump or terminator.
//   2. Blocks: the ranges [leader_i, leader_{i+1}).
//   3. Successors, from the jumpKind of each block's last instruction.
//   4. Exception handler edges: every block overlapping a try range gets an
//      edge to its catch block.
//   5. DFS (White/Gray/Black): an edge to a Gray node is a back-edge (loop).

#include "cfg/cfg.h"
#include "format/opcodes.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hbc {

// Jump offsets are relative to the start of the jump instruction itself
// (ip + offset), NOT to the next instruction.
static uint32_t relativeTarget(const Instruction& instr, int32_t relOffset) {
    return static_cast<uint32_t>(static_cast<int32_t>(instr.offset) + relOffset);
}

// Taken target of Jmp/Jcc: operand 0 (Addr8 or Addr32).
static uint32_t jumpTarget(const Instruction& instr) {
    return relativeTarget(instr, instr.operands[0].i32);
}

// SwitchImm default target: operand 2 (Addr32). Case targets are in
// instr.switchTargets, already resolved by the disassembler.
static uint32_t switchDefaultTarget(const Instruction& instr) {
    return relativeTarget(instr, instr.operands[2].i32);
}

static bool isFullSwitch(const Instruction& instr) {
    return instr.operands.size() >= 5;
}

CFGData buildCFG(
    const std::vector<Instruction>&          instrs,
    uint32_t                                 funcBytecodeSize,
    uint32_t                                 version,
    const std::vector<ExceptionHandlerInfo>& handlers)
{
    CFGData cfg;
    if (instrs.empty()) return cfg;

    const OpcodeInfo* table = getOpcodeTable(version);

    // ---- 1. Leaders ---------------------------------------------------------
    std::unordered_set<uint32_t> leaders;
    leaders.insert(instrs[0].offset);

    for (uint32_t i = 0; i < instrs.size() && table; i++) {
        const Instruction& instr = instrs[i];
        const OpcodeInfo& info = table[instr.opcode];
        if (!info.mnemonic || info.jumpKind == JumpKind::None) continue;

        if (i + 1 < instrs.size())
            leaders.insert(instrs[i + 1].offset);

        switch (info.jumpKind) {
            case JumpKind::Unconditional:
            case JumpKind::Conditional:
                leaders.insert(jumpTarget(instr));
                break;

            case JumpKind::Switch:
                if (isFullSwitch(instr)) {
                    leaders.insert(switchDefaultTarget(instr));
                    leaders.insert(instr.switchTargets.begin(), instr.switchTargets.end());
                }
                break;

            case JumpKind::Return:
            case JumpKind::Throw:
            case JumpKind::None:
                break;
        }
    }

    // ---- 2. Blocks ----------------------------------------------------------
    std::vector<uint32_t> leaderVec(leaders.begin(), leaders.end());
    std::sort(leaderVec.begin(), leaderVec.end());

    std::unordered_map<uint32_t, uint32_t> offsetToBlockId;
    offsetToBlockId.reserve(leaderVec.size());
    for (uint32_t i = 0; i < leaderVec.size(); i++)
        offsetToBlockId[leaderVec[i]] = i;

    cfg.blocks.resize(leaderVec.size());
    for (uint32_t i = 0; i < leaderVec.size(); i++) {
        cfg.blocks[i].id          = i;
        cfg.blocks[i].startOffset = leaderVec[i];
        cfg.blocks[i].endOffset   = (i + 1 < leaderVec.size()) ? leaderVec[i + 1] : funcBytecodeSize;
    }

    uint32_t bIdx = 0;
    for (const auto& instr : instrs) {
        while (bIdx + 1 < cfg.blocks.size() && instr.offset >= cfg.blocks[bIdx + 1].startOffset)
            bIdx++;
        cfg.blocks[bIdx].instructions.push_back(instr);
    }

    // ---- 3. Successors ------------------------------------------------------
    // Edges to offsets that aren't a block start are dropped.
    auto addEdge = [&](uint32_t fromId, uint32_t targetOff, EdgeKind kind) {
        auto it = offsetToBlockId.find(targetOff);
        if (it == offsetToBlockId.end()) return;
        const uint32_t toId = it->second;
        cfg.blocks[fromId].successors.push_back({toId, kind});
        cfg.blocks[toId].predecessors.push_back(fromId);
    };

    for (uint32_t b = 0; b < cfg.blocks.size() && table; b++) {
        const BasicBlock& bb = cfg.blocks[b];
        if (bb.instructions.empty()) continue;

        const Instruction& last = bb.instructions.back();
        const OpcodeInfo& info = table[last.opcode];
        if (!info.mnemonic) continue;

        const bool hasNext = b + 1 < cfg.blocks.size();

        switch (info.jumpKind) {
            case JumpKind::Return:
            case JumpKind::Throw:
                break;  // exit node

            case JumpKind::None:
                if (hasNext)
                    addEdge(b, cfg.blocks[b + 1].startOffset, EdgeKind::FallThrough);
                break;

            case JumpKind::Unconditional:
                addEdge(b, jumpTarget(last), EdgeKind::Jump);
                break;

            case JumpKind::Conditional:
                addEdge(b, jumpTarget(last), EdgeKind::CondTaken);
                if (hasNext)
                    addEdge(b, cfg.blocks[b + 1].startOffset, EdgeKind::CondFallThru);
                break;

            case JumpKind::Switch:
                if (isFullSwitch(last)) {
                    addEdge(b, switchDefaultTarget(last), EdgeKind::SwitchDefault);
                    for (uint32_t caseTarget : last.switchTargets)
                        addEdge(b, caseTarget, EdgeKind::SwitchCase);
                }
                break;
        }
    }

    // ---- 4. Exception handler edges ----------------------------------------
    // Models the implicit transfer on throw; without it catch blocks would be
    // unreachable from the entry.
    for (const auto& eh : handlers) {
        auto catchIt = offsetToBlockId.find(eh.target);
        if (catchIt == offsetToBlockId.end()) continue;
        const uint32_t catchId = catchIt->second;

        for (uint32_t b = 0; b < cfg.blocks.size(); b++) {
            const BasicBlock& bb = cfg.blocks[b];
            if (bb.startOffset >= eh.end || bb.endOffset <= eh.start) continue;  // outside [start, end)
            if (b == catchId) continue;

            const bool already = std::any_of(bb.successors.begin(), bb.successors.end(),
                [&](const CFGEdge& e) {
                    return e.targetId == catchId && e.kind == EdgeKind::ExceptionHandler;
                });
            if (!already)
                addEdge(b, eh.target, EdgeKind::ExceptionHandler);
        }
    }

    // ---- 5. Back-edges ------------------------------------------------------
    enum class Color : uint8_t { White, Gray, Black };
    std::vector<Color> color(cfg.blocks.size(), Color::White);

    std::function<void(uint32_t)> dfs = [&](uint32_t bid) {
        color[bid] = Color::Gray;
        for (auto& edge : cfg.blocks[bid].successors) {
            if (color[edge.targetId] == Color::Gray) {
                edge.kind = EdgeKind::BackEdge;
                cfg.blocks[edge.targetId].isLoopHeader = true;
                cfg.hasLoops = true;
            } else if (color[edge.targetId] == Color::White) {
                dfs(edge.targetId);
            }
        }
        color[bid] = Color::Black;
    };

    dfs(cfg.entryId);

    return cfg;
}

} // namespace hbc
