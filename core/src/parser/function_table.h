// Private header (FunctionInfo is public, in hbc/hbc.h).
#pragma once

#include "hbc/hbc.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hbc {

// Reads the function table (right after the 128-byte header) into one
// FunctionInfo per function, following overflowed entries to their
// FunctionHeader. `strings` resolves function names; anonymous functions get
// "anonymous_func_N". isAsync/isGenerator stay false here (HBCFile::disasm
// detects them). `data` is the start of the file.
std::vector<FunctionInfo> parseFunctionTable(
    const uint8_t*                   data,
    const HBCFileHeader&             hdr,
    const std::vector<std::string>&  strings);

} // namespace hbc
