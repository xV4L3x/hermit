// Private header: per-version opcode tables (public view: opcode_table() in hbc/hbc.h).
#pragma once

#include "hbc/hbc.h"

#include <cstdint>

namespace hbc {

using OperandType = Operand::Type;

// Control-flow behavior of an opcode, so the CFG builder doesn't compare mnemonics.
// Table entries that omit it are zero-initialized to None.
enum class JumpKind : uint8_t {
    None         = 0,  // falls through to the next instruction
    Unconditional,     // Jmp, JmpLong: 1 successor (the target)
    Conditional,       // JmpTrue/False/Undefined, JLess/Equal/...: taken + fall-through
    Switch,            // SwitchImm: one successor per jump-table case, plus default
    Return,            // Ret: exit node
    Throw,             // Throw, Unreachable: exit node
};

// One entry of a 256-slot table indexed by opcode byte.
struct OpcodeInfo {
    const char* mnemonic;       // nullptr = unknown/reserved opcode (disassembler emits "???")
    uint8_t     numOperands;    // 0-6 (Call4 in v96 has 6)
    OperandType operands[6];    // PHYSICAL types, which fix each operand's size; only the first numOperands are meaningful

    // Bit N set: operand N is a string table index. The physical type
    // (UInt8/16/32, e.g. GetByIdShort/GetById/GetByIdLong) says how many bytes
    // to read; the disassembler then resolves strings()[value] and sets the
    // Operand type to StringID. StringID never appears in `operands`.
    uint8_t     stringRefMask;
    JumpKind    jumpKind;
};

// The 256-entry table for an HBC version, or nullptr if unsupported.
const OpcodeInfo* getOpcodeTable(uint32_t version);

// Size in bytes of an operand type.
uint32_t operandSize(OperandType t);

} // namespace hbc
