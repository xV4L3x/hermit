#include "parser/function_table.h"
#include "hbc/format.h"
#include "hbc/format_v98.h"

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
    const bool isV98 = hdr.version >= 97;

    std::vector<FunctionInfo> result;
    result.reserve(hdr.functionCount);

    // Anonymous = empty or out-of-range name. Don't test functionName == 0:
    // string 0 is the bundle's first string, not necessarily the empty one.
    auto resolveName = [&](uint32_t nameIdx, uint32_t funcIndex) {
        if (nameIdx < strings.size() && !strings[nameIdx].empty())
            return strings[nameIdx];
        return "anonymous_func_" + std::to_string(funcIndex);
    };

    // Per-function fields pulled out of the on-disk header, independent of the
    // version-specific layout. `ok == false` marks an out-of-bounds overflow
    // offset (a corrupt entry).
    struct Raw {
        uint32_t offset = 0, bytecodeSizeInBytes = 0, paramCount = 0, frameSize = 0;
        uint32_t infoOffset = 0, nameIdx = 0;
        uint8_t  prohibitInvoke = kProhibitInvokeNone, kind = kFuncKindNormal;
        bool strictMode = false, hasEH = false, hasDebug = false, overflowed = false;
        bool ok = true;
    };

    // v98 (Static Hermes): 12-byte SmallFuncHeader_v98, no infoOffset field. A
    // function has an info section iff overflowed; the large header sits at
    // (functionName << 16) | offset and the info section (EH + debug) follows
    // it, aligned to 4 (INFO_ALIGNMENT).
    auto extractV98 = [&](uint32_t i) -> Raw {
        const auto* table = reinterpret_cast<const SmallFuncHeader_v98*>(data + sizeof(HBCFileHeader));
        const SmallFuncHeader_v98& fh = table[i];
        Raw r;
        r.overflowed = fh.flags.overflowed;
        if (!r.overflowed) {
            r.offset = fh.offset;
            r.bytecodeSizeInBytes = fh.bytecodeSizeInBytes;
            r.paramCount = fh.paramCount;
            r.frameSize = fh.frameSize;
            r.infoOffset = 0;  // no info section when not overflowed
            r.nameIdx = fh.functionName;
            r.prohibitInvoke = fh.flags.prohibitInvoke;
            r.kind = fh.flags.kind;
            r.strictMode = fh.flags.strictMode;
            r.hasEH = fh.flags.hasExceptionHandler;
            r.hasDebug = fh.flags.hasDebugInfo;
            return r;
        }
        const uint32_t largeOff = (static_cast<uint32_t>(fh.functionName) << 16) | fh.offset;
        if (static_cast<uint64_t>(largeOff) + sizeof(FunctionHeader_v98) > hdr.fileLength) {
            r.ok = false;
            return r;
        }
        const auto* lh = reinterpret_cast<const FunctionHeader_v98*>(data + largeOff);
        r.offset = lh->offset;
        r.bytecodeSizeInBytes = lh->bytecodeSizeInBytes;
        r.paramCount = lh->paramCount;
        r.frameSize = lh->frameSize;
        // The EH table / debug offsets follow the large header, aligned to 4.
        r.infoOffset = (largeOff + static_cast<uint32_t>(sizeof(FunctionHeader_v98)) + 3) & ~3u;
        r.nameIdx = lh->functionName;
        r.prohibitInvoke = lh->flags.prohibitInvoke;
        r.kind = lh->flags.kind;
        r.strictMode = lh->flags.strictMode;
        r.hasEH = lh->flags.hasExceptionHandler;
        r.hasDebug = lh->flags.hasDebugInfo;
        return r;
    };

    // v94-96: 16-byte SmallFuncHeader with a dedicated infoOffset; the overflow
    // offset is packed as (offset & 0xFFFF) | ((infoOffset & 0xFFFF) << 16) and
    // the info section starts right after the 32-byte FunctionHeader.
    auto extractLegacy = [&](uint32_t i) -> Raw {
        const auto* table = reinterpret_cast<const SmallFuncHeader*>(data + sizeof(HBCFileHeader));
        const SmallFuncHeader& fh = table[i];
        Raw r;
        r.overflowed = fh.flags.overflowed;
        if (!r.overflowed) {
            r.offset = fh.offset;
            r.bytecodeSizeInBytes = fh.bytecodeSizeInBytes;
            r.paramCount = fh.paramCount;
            r.frameSize = fh.frameSize;
            r.infoOffset = fh.infoOffset;
            r.nameIdx = fh.functionName;
            r.prohibitInvoke = fh.flags.prohibitInvoke;
            r.strictMode = fh.flags.strictMode;
            r.hasEH = fh.flags.hasExceptionHandler;
            r.hasDebug = fh.flags.hasDebugInfo;
            return r;
        }
        const uint32_t largeOff = (fh.offset & 0xFFFF) | ((fh.infoOffset & 0xFFFF) << 16);
        if (static_cast<uint64_t>(largeOff) + sizeof(FunctionHeader) > hdr.fileLength) {
            r.ok = false;
            return r;
        }
        const auto* lh = reinterpret_cast<const FunctionHeader*>(data + largeOff);
        r.offset = lh->offset;
        r.bytecodeSizeInBytes = lh->bytecodeSizeInBytes;
        r.paramCount = lh->paramCount;
        r.frameSize = lh->frameSize;
        r.infoOffset = largeOff + static_cast<uint32_t>(sizeof(FunctionHeader));
        r.nameIdx = lh->functionName;
        r.prohibitInvoke = lh->flags.prohibitInvoke;
        r.strictMode = lh->flags.strictMode;
        r.hasEH = lh->flags.hasExceptionHandler;
        r.hasDebug = lh->flags.hasDebugInfo;
        return r;
    };

    for (uint32_t i = 0; i < hdr.functionCount; ++i) {
        const Raw r = isV98 ? extractV98(i) : extractLegacy(i);

        if (!r.ok) {
            std::fprintf(stderr,
                "[funcTable] Error: function %u overflow offset out of file bounds\n", i);
            FunctionInfo broken{};
            broken.index = i;
            broken.name  = "anonymous_func_" + std::to_string(i);
            result.push_back(std::move(broken));
            continue;
        }

        FunctionInfo info;
        info.index               = i;
        info.overflowed          = r.overflowed;
        info.offset              = r.offset;
        info.bytecodeSizeInBytes = r.bytecodeSizeInBytes;
        info.paramCount          = r.paramCount;
        info.frameSize           = r.frameSize;
        info.infoOffset          = r.infoOffset;
        info.name                = resolveName(r.nameIdx, i);
        info.prohibitCall        = r.prohibitInvoke == kProhibitInvokeCall;
        info.prohibitConstruct   = r.prohibitInvoke == kProhibitInvokeConstruct;
        info.isStrictMode        = r.strictMode;
        info.hasExceptionHandler = r.hasEH;
        info.hasDebugInfo        = r.hasDebug;
        // v98 records the function kind in the header; v94-96 do not (left false
        // and recovered elsewhere).
        info.isGenerator         = isV98 && r.kind == kFuncKindGenerator;
        info.isAsync             = isV98 && r.kind == kFuncKindAsync;

        result.push_back(std::move(info));
    }

    markSharedBodies(result);
    return result;
}

} // namespace hbc
