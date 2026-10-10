// Private header (the public StringKind type is in hbc/hbc.h).
#pragma once

#include "hbc/hbc.h"
#include <cstdint>
#include <vector>

namespace hbc {

// Decodes the String Kind Table into a vector parallel to strings()
// (result.size() == hdr.stringCount). `data` is the start of the file.
// Throws std::runtime_error if the decoded size doesn't match.
//
// The table is run-length encoded: each uint32 is { count: bits 0-30,
// kind: bit 31 (0 = String, 1 = Identifier) }, e.g. 0x80000003 = the next 3
// strings are Identifiers.
std::vector<StringKind> parseStringKinds(const uint8_t* data, const HBCFileHeader& hdr);

} // namespace hbc
