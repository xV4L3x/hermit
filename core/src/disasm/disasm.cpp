#include "disasm/disasm.h"
#include "format/opcodes.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace hbc {

// Unaligned little-endian read (memcpy avoids undefined behavior).
template <typename T>
static T readLE(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// Decodes one operand at `pos` and advances it. Small integers are normalized
// into `u32`, Addr8 is sign-extended into `i32`. If `isStringRef`, the value is
// an index into `strings` and the operand becomes StringID. On truncated
// bytecode `pos` jumps to `limit` and the operand is left zeroed.
static Operand decodeOperand(
    OperandType                       physicalType,
    bool                              isStringRef,
    const uint8_t*                    bytecode,
    uint32_t&                         pos,
    uint32_t                          limit,
    const std::vector<std::string>&   strings,
    const std::vector<StringKind>&    kinds)
{
    Operand op{};
    op.type = physicalType;

    const uint32_t sz = operandSize(physicalType);
    if (pos + sz > limit) {
        pos = limit;
        return op;
    }

    const uint8_t* p = bytecode + pos;
    switch (physicalType) {
        case OperandType::Reg8:
        case OperandType::UInt8:
            op.u32 = readLE<uint8_t>(p);
            break;
        case OperandType::UInt16:
            op.u32 = readLE<uint16_t>(p);
            break;
        case OperandType::UInt32:
        case OperandType::Reg32:
        case OperandType::StringID:
            op.u32 = readLE<uint32_t>(p);
            break;
        case OperandType::Imm32:
        case OperandType::Addr32:
            op.i32 = readLE<int32_t>(p);
            break;
        case OperandType::Addr8:
            op.i32 = readLE<int8_t>(p);
            break;
        case OperandType::Double:
            op.f64 = readLE<double>(p);
            break;
    }
    pos += sz;

    if (isStringRef) {
        const uint32_t idx = op.u32;
        op.type = OperandType::StringID;

        if (idx < strings.size()) {
            op.resolvedString = strings[idx];
            if (idx < kinds.size()) {
                op.resolvedString += (kinds[idx] == StringKind::Identifier)
                                     ? " [identifier]"
                                     : " [string]";
            }
        } else {
            op.resolvedString = "<out-of-range string index " + std::to_string(idx) + ">";
        }
    }

    return op;
}

// SwitchImm operands: [1] table offset from the instruction, [3] min case
// value, [4] max case value. Each table entry is an int32 relative to the
// instruction; targets are returned relative to the function's bytecode.
static std::vector<uint32_t> readSwitchTargets(
    const Instruction& instr, const uint8_t* bytecode,
    const uint8_t* fileData, uint32_t fileSize)
{
    std::vector<uint32_t> targets;

    const uint32_t tableOff = instr.operands[1].u32;
    const uint32_t minVal   = instr.operands[3].u32;
    const uint32_t maxVal   = instr.operands[4].u32;
    if (maxVal < minVal) return targets;

    const uint32_t numCases = maxVal - minVal + 1;
    const uint32_t instrAbs = static_cast<uint32_t>(bytecode - fileData) + instr.offset;
    const uint32_t tableAbs = (instrAbs + tableOff + 3u) & ~3u;  // aligned 4 at runtime

    targets.reserve(numCases);
    for (uint32_t c = 0; c < numCases; c++) {
        const uint32_t entryAbs = tableAbs + c * 4;
        if (entryAbs + 4 > fileSize) break;  // corrupt/truncated table
        const int32_t entryOff = readLE<int32_t>(fileData + entryAbs);
        targets.push_back(static_cast<uint32_t>(static_cast<int32_t>(instr.offset) + entryOff));
    }
    return targets;
}

std::vector<Instruction> disassemble(
    const uint8_t*                   bytecode,
    uint32_t                         bytecodeSizeInBytes,
    uint32_t                         version,
    const std::vector<std::string>&  strings,
    const std::vector<StringKind>&   kinds,
    const uint8_t*                   fileData,
    uint32_t                         fileSize)
{
    std::vector<Instruction> result;

    const OpcodeInfo* table = getOpcodeTable(version);
    if (!table) {
        std::fprintf(stderr,
            "[disasm] ERROR: no opcode table for HBC v%u - "
            "the bytecode cannot be decoded.\n", version);
        return result;
    }

    uint32_t pos = 0;
    while (pos < bytecodeSizeInBytes) {
        const uint32_t instrOffset = pos;
        const uint8_t  opcode      = bytecode[pos++];
        const OpcodeInfo& info     = table[opcode];

        Instruction instr{};
        instr.offset = instrOffset;
        instr.opcode = opcode;

        if (!info.mnemonic) {
            instr.mnemonic = "???";
            std::fprintf(stderr,
                "[disasm] WARNING: unknown opcode 0x%02X at offset 0x%04X "
                "(advancing 1 byte and continuing)\n",
                opcode, instrOffset);
            result.push_back(std::move(instr));
            continue;
        }

        instr.mnemonic = info.mnemonic;
        for (uint8_t i = 0; i < info.numOperands; i++) {
            const bool isStringRef = (info.stringRefMask & (1u << i)) != 0;
            instr.operands.push_back(decodeOperand(
                info.operands[i], isStringRef,
                bytecode, pos, bytecodeSizeInBytes,
                strings, kinds));
        }

        if (info.jumpKind == JumpKind::Switch && instr.operands.size() == 5 && fileData)
            instr.switchTargets = readSwitchTargets(instr, bytecode, fileData, fileSize);

        result.push_back(std::move(instr));
    }

    return result;
}

} // namespace hbc
