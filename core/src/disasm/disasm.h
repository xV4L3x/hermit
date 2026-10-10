// Private header. Clients use HBCFile::disasm(functionIndex).
#pragma once

#include "hbc/hbc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hbc {

// Decodes a function's bytecode into instructions. Instruction offsets are
// relative to the start of the function's bytecode, not the file.
//
//   bytecode / bytecodeSizeInBytes  the function body (raw_data() + fi.offset)
//   version                         94, 95 or 96, selects the opcode table
//   strings / kinds                 resolve StringID operands (resolvedString is
//                                   `value [identifier]` or `value [string]`)
//   fileData / fileSize             optional, the whole file; only needed to fill
//                                   Instruction::switchTargets. Without them
//                                   SwitchImm decodes normally with an empty table
//                                   (the patcher passes nullptr: it rejects SwitchImm).
//
// A SwitchImm jump table is NOT inside the function's declared byte range: it
// sits past bytecodeSizeInBytes, in the gap before the next function body, and
// its address is the instruction's table offset rounded up to 4 bytes. Described
// only in the Hermes source (lib/VM/Interpreter.cpp, CASE(SwitchImm), and
// BytecodeList.def at tag hermes-2023-09-26-RNv0.73.0).
//
// Unknown opcodes never throw: a warning goes to stderr, an instruction with
// mnemonic "???" is emitted, and decoding resumes at the next byte.
std::vector<Instruction> disassemble(
    const uint8_t*                   bytecode,
    uint32_t                         bytecodeSizeInBytes,
    uint32_t                         version,
    const std::vector<std::string>&  strings,
    const std::vector<StringKind>&   kinds,
    const uint8_t*                   fileData = nullptr,
    uint32_t                         fileSize = 0
);

} // namespace hbc
