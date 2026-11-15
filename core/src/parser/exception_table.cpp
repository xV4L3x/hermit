#include "parser/exception_table.h"
#include "hbc/format.h"
#include "hbc/format_version.h"

#include <cstdio>
#include <cstring>

namespace hbc {

void parseExceptionHandlers(
    const uint8_t*             fileData,
    uint32_t                   fileSize,
    uint32_t                   version,
    std::vector<FunctionInfo>& functions)
{
    const FormatVersion* fmt = formatForVersion(version);
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

        // Per-function debug offsets, read by the format module: v94-96 store
        // three uint32 { source locations, scope descriptor, textified callees },
        // v98 the older two-uint32 form { source locations, lexical data }. The EH
        // table above is always a multiple of 4 bytes, so `pos` is already aligned
        // (the serializer's pad(4) is a no-op here).
        if (fi.hasDebugInfo && pos + fmt->debugOffsetsSize() <= fileSize) {
            const DebugOffsetsFields d = fmt->readDebugOffsets(fileData + pos);
            fi.debugOffset            = d.debugOffset;
            fi.scopeDescOffset        = d.scopeDescOffset;
            fi.textifiedCalleesOffset = d.textifiedCalleesOffset;
        }
    }
}

} // namespace hbc
