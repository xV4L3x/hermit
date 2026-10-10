#include "format/opcodes.h"

namespace hbc {

// Defined in the generated opcodes_vNN.cpp files.
namespace v94 { extern const OpcodeInfo OPCODES[256]; }
namespace v95 { extern const OpcodeInfo OPCODES[256]; }
namespace v96 { extern const OpcodeInfo OPCODES[256]; }

const OpcodeInfo* getOpcodeTable(uint32_t version) {
    switch (version) {
        case 96: return v96::OPCODES;
        case 95: return v95::OPCODES;
        case 94: return v94::OPCODES;
        default: return nullptr;
    }
}

// Sizes follow DEFINE_OPERAND_TYPE in Hermes's BytecodeList.def (Addr* and
// Imm32 are signed).
uint32_t operandSize(OperandType t) {
    using T = OperandType;
    switch (t) {
        case T::Reg8:     return 1;
        case T::Reg32:    return 4;
        case T::UInt8:    return 1;
        case T::UInt16:   return 2;
        case T::UInt32:   return 4;
        case T::Imm32:    return 4;
        case T::Double:   return 8;
        case T::Addr8:    return 1;
        case T::Addr32:   return 4;
        case T::StringID: return 4;  // never in OpcodeInfo::operands, kept for completeness
    }
    return 1;  // unreachable
}

} // namespace hbc
