// FormatVersion module for HBC v99 (Static Hermes / static_h, bytecode-version
// bump 42235b8d, 2026-02-12; hermit targets commit c8dab081f). v99 ships in no
// React Native stable release yet, but real production bundles already use it
// (verified against one, and against a v99 compiler built from static_h).
//
// v99 is the v98 wire format with one layout change: the overflow (large)
// function header is 36 bytes, not 37. `CacheNewObject` / its `NumCacheNewObject`
// header field were removed (Hermes 7193d4485, 2026-01-21, before the v99 bump),
// shrinking the large header and moving its flag byte from offset 36 to 35.
// FormatV98 reads the flag byte at largeHeaderSize()-1 and bounds-checks against
// largeHeaderSize(), so FormatV99 only overrides that one size. The file header
// (HBCFileHeader_v98, incl. numStringSwitchImms), the 12-byte small header, the
// {sourceLocations} debug offsets, the object shape table and the literal-buffer
// sections are all unchanged from v98. The opcode table is renumbered (220 ops:
// +NewTypedObjectWithBuffer, +ToUint32, -CacheNewObject, NewFastArray arity 3)
// and lives in opcodes_v99.cpp, selected by getOpcodeTable(99).

#include "format_v98_class.h"

namespace hbc {

namespace {
class FormatV99 final : public FormatV98 {
public:
    uint32_t largeHeaderSize() const override { return kV99LargeFuncHeaderSize; }  // 36
};
const FormatV99 kV99;
}

const FormatVersion* formatV99() { return &kV99; }

} // namespace hbc
