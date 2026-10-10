#include "parser/function_source_table.h"

#include <cstdio>
#include <cstring>

namespace hbc {

void parseFunctionSourceTable(
    const uint8_t*                  fileData,
    uint32_t                        fileSize,
    uint32_t                        sectionOffset,
    uint32_t                        entryCount,
    const std::vector<std::string>& strings,
    std::vector<FunctionInfo>&      functions)
{
    if (entryCount == 0 || sectionOffset == 0) return;

    const uint64_t needed = static_cast<uint64_t>(entryCount) * 8;
    if (static_cast<uint64_t>(sectionOffset) + needed > fileSize) {
        std::fprintf(stderr,
            "[funcSourceTable] %u entries at 0x%X exceed file bounds\n",
            entryCount, sectionOffset);
        return;
    }

    for (uint32_t i = 0; i < entryCount; ++i) {
        uint32_t functionID, stringID;
        std::memcpy(&functionID, fileData + sectionOffset + i * 8,     4);
        std::memcpy(&stringID,   fileData + sectionOffset + i * 8 + 4, 4);

        if (functionID >= functions.size()) {
            std::fprintf(stderr,
                "[funcSourceTable] entry %u: functionID %u out of range (functionCount=%zu)\n",
                i, functionID, functions.size());
            continue;
        }

        FunctionInfo& fi = functions[functionID];
        fi.hasPreservedSource = true;
        if (stringID < strings.size() && !strings[stringID].empty()) {
            fi.sourceIsHidden  = false;
            fi.preservedSource = strings[stringID];
        } else {
            // Empty (or out-of-range, treated the same) = "hide source"/"sensitive" marker.
            fi.sourceIsHidden  = true;
            fi.preservedSource.clear();
        }
    }
}

} // namespace hbc
