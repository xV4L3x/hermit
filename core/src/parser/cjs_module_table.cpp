#include "parser/cjs_module_table.h"

#include <cstdio>
#include <cstring>

namespace hbc {

std::vector<CJSModuleInfo> parseCJSModuleTable(
    const uint8_t*                  fileData,
    uint32_t                        sectionOffset,
    uint32_t                        entryCount,
    bool                            staticallyResolved,
    const std::vector<std::string>& strings)
{
    std::vector<CJSModuleInfo> result;
    if (entryCount == 0 || sectionOffset == 0) return result;

    // Bounds are only checked against sectionOffset's uint32_t domain here;
    // callers pass sectionOffset from section_map(), which already validated
    // it against the mapped file.
    result.reserve(entryCount);
    for (uint32_t i = 0; i < entryCount; ++i) {
        uint32_t first, second;
        std::memcpy(&first,  fileData + sectionOffset + i * 8,     4);
        std::memcpy(&second, fileData + sectionOffset + i * 8 + 4, 4);

        CJSModuleInfo info;
        info.functionIndex        = second;
        info.isStaticallyResolved = staticallyResolved;
        info.moduleId             = first;
        if (!staticallyResolved && first < strings.size())
            info.modulePath = strings[first];
        else if (!staticallyResolved) {
            std::fprintf(stderr,
                "[cjsModuleTable] entry %u: stringID %u out of range (stringCount=%zu)\n",
                i, first, strings.size());
        }
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace hbc
