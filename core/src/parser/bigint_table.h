// Private header: BigInt Table parsing.
//
// header.bigIntCount entries of {offset, length} (8 bytes each, offsets
// relative to the BigInt storage). An entry's bytes are the value as
// little-endian two's complement with the redundant sign-extension bytes
// dropped, so 0n has length 0, 255n is `ff 00` and -1n is `ff`. Described in the
// Hermes source: include/hermes/Support/BigIntSupport.h (ParsedBigInt),
// lib/Support/BigIntSupport.cpp (dropExtraSignBits). The runtime builds the value
// with BigIntPrimitive::fromBytes.
#pragma once

#include "hbc/hbc.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hbc {

// One BigIntInfo per table entry, in on-disk order. An entry that does not fit
// in the storage is returned with ok = false and the reason in `error`; the
// others are unaffected. Empty if count == 0.
// `tableOffset` and `storageOffset` are absolute file offsets (from section_map()).
std::vector<BigIntInfo> parseBigIntTable(
    const uint8_t* fileData,
    uint32_t       tableOffset,
    uint32_t       count,
    uint32_t       storageOffset,
    uint32_t       storageSize);

// Signed decimal text of a little-endian two's complement byte string
// ("0" for an empty one).
std::string bigIntBytesToDecimal(const std::vector<uint8_t>& bytes);

} // namespace hbc
