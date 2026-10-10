#include "parser/string_kind.h"
#include "parser/section_layout.h"

#include <cstring>
#include <stdexcept>

namespace hbc {

std::vector<StringKind> parseStringKinds(const uint8_t* data, const HBCFileHeader& hdr) {
    const uint8_t* kindTable = data + stringSectionOffsets(hdr).kindTable;

    std::vector<StringKind> result;
    result.reserve(hdr.stringCount);

    for (uint32_t i = 0; i < hdr.stringKindCount; ++i) {
        uint32_t entry;
        std::memcpy(&entry, kindTable + i * sizeof(entry), sizeof(entry));

        const uint32_t count = entry & 0x7FFFFFFF;
        const StringKind kind = static_cast<StringKind>((entry >> 31) & 1);
        result.insert(result.end(), count, kind);
    }

    if (result.size() != hdr.stringCount) {
        throw std::runtime_error(
            "String Kind Table decode error: expected " +
            std::to_string(hdr.stringCount) + " kinds, got " +
            std::to_string(result.size())
        );
    }

    return result;
}

} // namespace hbc
