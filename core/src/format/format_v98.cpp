// FormatVersion module for HBC v98 as shipped by Hermes V1 (React Native 0.84+),
// tag hermes-v250829098.0.14. See format_v98.h for the format summary. The
// 12-byte function header uses Hermes's BitField storage, which does not map to
// a portable C bitfield struct, so the fields are parsed/written bit by bit.
//
// The implementation lives in format_v98_class.h (class FormatV98), shared with
// the v99 module (format_v99.cpp), which differs only in largeHeaderSize().

#include "format_v98_class.h"

namespace hbc {

namespace {
const FormatV98 kV98;
}

const FormatVersion* formatV98() { return &kV98; }

} // namespace hbc
