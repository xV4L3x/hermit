// Private header (the CFG types are public, in hbc/hbc.h).
#pragma once

#include "hbc/hbc.h"

#include <cstdint>
#include <vector>

namespace hbc {

// Builds a function's CFG from its disassembled instructions (HBCFile::disasm).
//
//   instrs            must come from disassemble() with the file buffer, so that
//                     SwitchImm instructions carry their switchTargets; otherwise
//                     a SwitchImm only gets its SwitchDefault edge
//   funcBytecodeSize  end of the last block
//   version           selects the opcode table (jumpKind)
//   handlers          the function's exception handler table, may be empty
CFGData buildCFG(
    const std::vector<Instruction>&          instrs,
    uint32_t                                 funcBytecodeSize,
    uint32_t                                 version,
    const std::vector<ExceptionHandlerInfo>& handlers = {});

} // namespace hbc
