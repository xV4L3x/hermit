#include "parser/function_table.h"
#include "hbc/format.h"

#include <cstdio>
#include <map>
#include <utility>
#include <vector>

namespace hbc {

// Marks functions whose (offset, size) coincide. Metro/Hermes emits one copy
// of byte-identical bodies, so several SmallFuncHeaders point to the same
// bytes and editing them changes all of the functions in the group
// (doc/hbc-format/shared-bytecode.md).
static void markSharedBodies(std::vector<FunctionInfo>& functions) {
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> byBody;
    for (const auto& fi : functions)
        byBody[{fi.offset, fi.bytecodeSizeInBytes}].push_back(fi.index);

    for (const auto& [body, indices] : byBody) {
        if (indices.size() < 2) continue;
        for (uint32_t idx : indices) {
            functions[idx].shared     = true;
            functions[idx].shareCount = static_cast<uint32_t>(indices.size());
        }
    }
}

std::vector<FunctionInfo> parseFunctionTable(
    const uint8_t*                  data,
    const HBCFileHeader&            hdr,
    const std::vector<std::string>& strings)
{
    const auto* funcTable = reinterpret_cast<const SmallFuncHeader*>(data + sizeof(HBCFileHeader));

    std::vector<FunctionInfo> result;
    result.reserve(hdr.functionCount);

    // Anonymous = empty or out-of-range name. Don't test functionName == 0:
    // string 0 is the bundle's first string, not necessarily the empty one.
    auto resolveName = [&](uint32_t nameIdx, uint32_t funcIndex) {
        if (nameIdx < strings.size() && !strings[nameIdx].empty())
            return strings[nameIdx];
        return "anonymous_func_" + std::to_string(funcIndex);
    };

    for (uint32_t i = 0; i < hdr.functionCount; ++i) {
        const SmallFuncHeader& fh = funcTable[i];

        FunctionInfo info;
        info.index      = i;
        info.overflowed = fh.flags.overflowed;
        info.isAsync    = false;
        info.isGenerator = false;

        uint32_t      nameIdx;
        FunctionFlags flags;

        if (info.overflowed) {
            // The real fields are in a FunctionHeader at largeOff (see format.h).
            const uint32_t largeOff = (fh.offset & 0xFFFF) | ((fh.infoOffset & 0xFFFF) << 16);

            if (largeOff + sizeof(FunctionHeader) > hdr.fileLength) {
                std::fprintf(stderr,
                    "[funcTable] Error: function %u overflow offset 0x%X out of file bounds\n",
                    i, largeOff);
                FunctionInfo broken{};
                broken.index = i;
                broken.name  = "anonymous_func_" + std::to_string(i);
                result.push_back(std::move(broken));
                continue;
            }

            const auto* lh = reinterpret_cast<const FunctionHeader*>(data + largeOff);
            info.offset              = lh->offset;
            info.bytecodeSizeInBytes = lh->bytecodeSizeInBytes;
            info.paramCount          = lh->paramCount;
            info.frameSize           = lh->frameSize;
            // lh->infoOffset is the largeOff sentinel: the real info section
            // starts right after the FunctionHeader.
            info.infoOffset          = largeOff + static_cast<uint32_t>(sizeof(FunctionHeader));
            nameIdx = lh->functionName;
            flags   = lh->flags;
        } else {
            info.offset              = fh.offset;
            info.bytecodeSizeInBytes = fh.bytecodeSizeInBytes;
            info.paramCount          = fh.paramCount;
            info.frameSize           = fh.frameSize;
            info.infoOffset          = fh.infoOffset;
            nameIdx = fh.functionName;
            flags   = fh.flags;
        }

        info.name                = resolveName(nameIdx, i);
        info.prohibitCall        = flags.prohibitInvoke == kProhibitInvokeCall;
        info.prohibitConstruct   = flags.prohibitInvoke == kProhibitInvokeConstruct;
        info.isStrictMode        = flags.strictMode;
        info.hasExceptionHandler = flags.hasExceptionHandler;
        info.hasDebugInfo        = flags.hasDebugInfo;

        result.push_back(std::move(info));
    }

    markSharedBodies(result);
    return result;
}

} // namespace hbc
