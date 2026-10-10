// Private header: exception handler table parsing.
#pragma once

#include "hbc/hbc.h"
#include <cstdint>
#include <vector>

namespace hbc {

// For every function with hasExceptionHandler or hasDebugInfo, reads its
// function-info section (at FunctionInfo::infoOffset) and fills
// exceptionHandlers and the debugOffset / scopeDescOffset /
// textifiedCalleesOffset fields in place. `fileSize` is used for bounds checks.
//
// Function-info section layout:
//   if hasExceptionHandler: uint32 count, then count x HBCExceptionHandlerInfo
//   if hasDebugInfo:        3 x uint32 { debugOffset, scopeDescOffset, textifiedCalleesOffset }
// Handler offsets are relative to the start of the function's bytecode.
void parseExceptionHandlers(
    const uint8_t*             fileData,
    uint32_t                   fileSize,
    std::vector<FunctionInfo>& functions);

} // namespace hbc
