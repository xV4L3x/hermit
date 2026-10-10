#pragma once

// HBC v95 - validation target (no RN release shipped v95; produced by building
// Hermes at commit f6b56d334467). The header layout is identical to v96;
// the only change in v95 was adding an isStrict argument to the DirectEval opcode.

#include "hbc/format.h"

namespace hbc {

using HBCFileHeader_v95 = HBCFileHeader;
static_assert(sizeof(HBCFileHeader_v95) == 128);

} // namespace hbc
