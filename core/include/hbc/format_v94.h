#pragma once

// HBC v94 - validation target (React Native 0.72.x; prebuilt hermesc available
// from npm react-native@0.72.0). The header layout is identical to v95 and v96;
// the only change in v94 was adding block-scoping opcodes (let/const).

#include "hbc/format.h"

namespace hbc {

using HBCFileHeader_v94 = HBCFileHeader;
static_assert(sizeof(HBCFileHeader_v94) == 128);

} // namespace hbc
