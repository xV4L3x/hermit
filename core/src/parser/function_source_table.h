// Private header: Function Source Table parsing.
//
// The table (header functionSourceCount) holds (functionID, stringID) pairs,
// 8 bytes each, for functions whose first statement is a `"show source"`,
// `"hide source"` or `"sensitive"` directive (opt-in, like `"use strict"`;
// Sema/SemanticResolver.cpp::scanDirectives() in the Hermes source). Hermes
// binary-searches it by functionID (VM/CodeBlock.cpp) for
// Function.prototype.toString(). Functions without a directive have no entry.
//   "show source":              stringID is the function's byte-exact original source
//   "hide source"/"sensitive":  stringID is the empty string, a marker and not source
#pragma once

#include "hbc/hbc.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hbc {

// Fills hasPreservedSource/sourceIsHidden/preservedSource of the covered
// functions in place. Doesn't assume the table is sorted. No-op if
// entryCount == 0 or sectionOffset == 0 (absent). `sectionOffset` is the
// absolute offset of the "function source table" section (from section_map()).
void parseFunctionSourceTable(
    const uint8_t*                  fileData,
    uint32_t                        fileSize,
    uint32_t                        sectionOffset,
    uint32_t                        entryCount,
    const std::vector<std::string>& strings,
    std::vector<FunctionInfo>&      functions);

} // namespace hbc
