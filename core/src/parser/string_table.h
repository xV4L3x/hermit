// Private header: string table parsing (public API is in hbc/hbc.h).
#pragma once

#include "hbc/format.h"
#include "hbc/hbc.h"

#include <string>
#include <vector>

namespace hbc {

// Decodes every string of the bundle to UTF-8, in index order
// (result.size() == hdr.stringCount), including those stored as UTF-16.
// `data` is the start of the file. Called by HBCFile::strings(), which caches it.
std::vector<std::string> parseStringTable(const uint8_t* data, const HBCFileHeader& hdr);

// Raw per-string metadata (location, length, encoding, kind), in index order.
// storageOffset is an ABSOLUTE file offset. `kinds` comes from parseStringKinds().
std::vector<StringEntry> parseStringEntries(const uint8_t* data,
                                            const HBCFileHeader& hdr,
                                            const std::vector<StringKind>& kinds);

} // namespace hbc
