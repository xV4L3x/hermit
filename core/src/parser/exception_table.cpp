#include "parser/exception_table.h"
#include "hbc/format.h"

#include <cstdio>
#include <cstring>

namespace hbc {

void parseExceptionHandlers(
    const uint8_t*             fileData,
    uint32_t                   fileSize,
    std::vector<FunctionInfo>& functions)
{
    for (auto& fi : functions) {
        if (!fi.hasExceptionHandler && !fi.hasDebugInfo) continue;

        // The section starts at infoOffset, NOT after the function's bytecode.
        uint32_t pos = fi.infoOffset;

        if (fi.hasExceptionHandler) {
            if (pos + sizeof(ExceptionHandlerTableHeader) > fileSize) {
                std::fprintf(stderr,
                    "[excTable] function %u: infoOffset 0x%X out of bounds\n",
                    fi.index, pos);
                continue;
            }

            ExceptionHandlerTableHeader hdr;
            std::memcpy(&hdr, fileData + pos, sizeof(hdr));
            pos += static_cast<uint32_t>(sizeof(hdr));

            // Larger than any real table: wrong infoOffset, garbage count.
            if (hdr.count > 65535) continue;

            const uint32_t needed = hdr.count * static_cast<uint32_t>(sizeof(HBCExceptionHandlerInfo));
            if (pos + needed > fileSize) {
                std::fprintf(stderr,
                    "[excTable] function %u: %u handlers at 0x%X exceed file bounds\n",
                    fi.index, hdr.count, fi.infoOffset);
                continue;
            }

            fi.exceptionHandlers.reserve(hdr.count);
            for (uint32_t j = 0; j < hdr.count; ++j) {
                HBCExceptionHandlerInfo raw;
                std::memcpy(&raw, fileData + pos + j * sizeof(raw), sizeof(raw));
                fi.exceptionHandlers.push_back({raw.start, raw.end, raw.target});
            }
            pos += needed;
        }

        // Hermes DebugOffsets (DebugInfo.h at tag hermes-*-RNv0.73.0). The
        // layout differs at tag v0.12.0, which predates this format.
        if (fi.hasDebugInfo && pos + 12 <= fileSize) {
            std::memcpy(&fi.debugOffset,           fileData + pos,     4);
            std::memcpy(&fi.scopeDescOffset,       fileData + pos + 4, 4);
            std::memcpy(&fi.textifiedCalleesOffset, fileData + pos + 8, 4);
        }
    }
}

} // namespace hbc
