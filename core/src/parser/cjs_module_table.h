// Private header: CJS Module Table parsing.
//
// The table (header.cjsModuleCount) holds (first, second) pairs, 8 bytes
// each, present only when the file was compiled with Hermes's native
// CommonJS mode (`hermes -commonjs`), never in a Metro-bundled bundle. See
// doc/hbc-format/v96-format.md §9 for the on-disk format, confirmed against
// the Hermes source (BytecodeStream.cpp, Domain.cpp) and against compiled
// samples.
//
// header.options bit 1 (cjsModulesStaticallyResolved) selects the pair's
// meaning:
//   0 (dynamic / "slow require"): (stringID, functionID) -- stringID is a
//     String Table index holding the module's source file path.
//   1 (static / "fast require"):  (moduleID, functionID) -- moduleID is a
//     small compiler-assigned integer, not a string index. As of the public
//     Hermes source this project targets, this shape can never actually be
//     emitted by the compiler (ResolveStaticRequire::runOnModule is gated on
//     a getUseCJSModules() that is hardcoded to return false), so this
//     branch is decoded for format fidelity but has never been observed.
#pragma once

#include "hbc/hbc.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hbc {

// Returns one CJSModuleInfo per entry, in on-disk order. Empty if
// entryCount == 0 or sectionOffset == 0 (section absent). `sectionOffset` is
// the absolute offset of the "cjs module table" section (from section_map()).
std::vector<CJSModuleInfo> parseCJSModuleTable(
    const uint8_t*                  fileData,
    uint32_t                        sectionOffset,
    uint32_t                        entryCount,
    bool                            staticallyResolved,
    const std::vector<std::string>& strings);

} // namespace hbc
