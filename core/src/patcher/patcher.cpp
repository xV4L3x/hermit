// Public helpers built on the opcode tables: opcode_table(), operand_size()
// and encode_instruction(). The structural serializer is in module.cpp.

#include "hbc/hbc.h"
#include "format/opcodes.h"

#include <cstdint>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <string>

namespace hbc {

static std::string jumpKindName(JumpKind k) {
    switch (k) {
        case JumpKind::None:          return "None";
        case JumpKind::Unconditional: return "Unconditional";
        case JumpKind::Conditional:   return "Conditional";
        case JumpKind::Switch:        return "Switch";
        case JumpKind::Return:        return "Return";
        case JumpKind::Throw:         return "Throw";
    }
    return "None";
}

std::vector<OpcodeDesc> opcode_table(uint32_t version) {
    std::vector<OpcodeDesc> out;
    const OpcodeInfo* table = getOpcodeTable(version);
    if (!table) return out;

    for (int i = 0; i < 256; ++i) {
        const OpcodeInfo& info = table[i];
        if (!info.mnemonic) continue;   // undefined / reserved

        OpcodeDesc d;
        d.opcode        = static_cast<uint8_t>(i);
        d.mnemonic      = info.mnemonic;
        d.stringRefMask = info.stringRefMask;
        d.jumpKind      = jumpKindName(info.jumpKind);
        for (uint8_t k = 0; k < info.numOperands; ++k)
            d.operands.push_back(info.operands[k]);
        out.push_back(std::move(d));
    }
    return out;
}

uint32_t operand_size(Operand::Type t) {
    return operandSize(t);
}

// Appends the raw bytes of a host value. HBC is little-endian, like the
// supported hosts (x86-64, ARM64).
static void appendBytes(std::vector<uint8_t>& out, const void* p, std::size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

// Operand value as an integer in [lo, hi]; throws std::range_error on
// NaN/inf/fractional or out-of-range values.
static int64_t requireIntInRange(double v, const char* mnemonic, int64_t lo, int64_t hi,
                                 const char* typeName) {
    if (std::isnan(v) || std::isinf(v) || std::floor(v) != v)
        throw std::range_error(std::string(mnemonic) +
                               ": non-integer operand (" + std::to_string(v) + ")");
    const int64_t i = static_cast<int64_t>(v);
    if (i < lo || i > hi)
        throw std::range_error(std::string(mnemonic) + ": operand out of " + typeName +
                               " range (" + std::to_string(lo) + ".." + std::to_string(hi) + ")");
    return i;
}

std::vector<uint8_t> encode_instruction(uint32_t version, uint8_t opcode,
                                        const std::vector<double>& operands) {
    const OpcodeInfo* table = getOpcodeTable(version);
    if (!table)
        throw std::invalid_argument("encode_instruction: unsupported HBC version: " +
                                    std::to_string(version));

    const OpcodeInfo& info = table[opcode];
    if (!info.mnemonic)
        throw std::invalid_argument("encode_instruction: unknown opcode for v" +
                                    std::to_string(version) + ": " +
                                    std::to_string(static_cast<unsigned>(opcode)));
    if (operands.size() != info.numOperands)
        throw std::invalid_argument(std::string(info.mnemonic) + ": expected " +
                                    std::to_string(info.numOperands) + " operands, got " +
                                    std::to_string(operands.size()));

    std::vector<uint8_t> out;
    out.reserve(1 + 8 * operands.size());
    out.push_back(opcode);

    for (uint8_t i = 0; i < info.numOperands; ++i) {
        const double raw = operands[i];
        const char* mn = info.mnemonic;
        switch (info.operands[i]) {
            case OperandType::Reg8:
            case OperandType::UInt8:
                out.push_back(static_cast<uint8_t>(requireIntInRange(raw, mn, 0, 0xFF, "UInt8")));
                break;
            case OperandType::UInt16: {
                const uint16_t b = static_cast<uint16_t>(requireIntInRange(raw, mn, 0, 0xFFFF, "UInt16"));
                appendBytes(out, &b, 2);
                break;
            }
            case OperandType::UInt32:
            case OperandType::Reg32:
            case OperandType::StringID: {
                const uint32_t b = static_cast<uint32_t>(requireIntInRange(raw, mn, 0, 0xFFFFFFFFLL, "UInt32"));
                appendBytes(out, &b, 4);
                break;
            }
            case OperandType::Imm32:
            case OperandType::Addr32: {
                const int32_t b = static_cast<int32_t>(requireIntInRange(raw, mn, INT32_MIN, INT32_MAX, "Int32"));
                appendBytes(out, &b, 4);
                break;
            }
            case OperandType::Addr8:
                out.push_back(static_cast<uint8_t>(
                    static_cast<int8_t>(requireIntInRange(raw, mn, -128, 127, "Addr8"))));
                break;
            case OperandType::Double:
                appendBytes(out, &raw, 8);
                break;
        }
    }
    return out;
}

} // namespace hbc
