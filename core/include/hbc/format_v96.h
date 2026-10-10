#pragma once

// HBC v96 - primary target (React Native 0.73–0.84 and Hermes main as of 2024-2026).
// The binary layout of BytecodeFileHeader is identical to v94 and v95; this alias
// will be replaced with a distinct struct when a future version diverges.

#include "hbc/format.h"

namespace hbc {

using HBCFileHeader_v96 = HBCFileHeader;
static_assert(sizeof(HBCFileHeader_v96) == 128);

} // namespace hbc
