#include "parser/function_table.h"
#include "hbc/format.h"
#include "hbc/format_v98.h"        // kFuncKind*
#include "hbc/format_version.h"

#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
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
    const FormatVersion* fmt = formatForVersion(hdr.version);
    if (!fmt)
        throw std::runtime_error("parseFunctionTable: unsupported version " +
                                 std::to_string(hdr.version));
    const uint8_t* fhBase = data + sizeof(HBCFileHeader);

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
        const FuncHeaderFields f = fmt->readFuncHeader(data, hdr.fileLength, fhBase, i);

        if (!f.valid) {
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
        info.overflowed          = f.overflowed;
        info.offset              = f.offset;
        info.bytecodeSizeInBytes = f.bytecodeSizeInBytes;
        info.paramCount          = f.paramCount;
        info.frameSize           = f.frameSize;
        info.infoOffset          = f.infoOffset;
        info.name                = resolveName(f.functionName, i);
        info.prohibitCall        = f.prohibitInvoke == kProhibitInvokeCall;
        info.prohibitConstruct   = f.prohibitInvoke == kProhibitInvokeConstruct;
        info.isStrictMode        = f.strictMode;
        info.hasExceptionHandler = f.hasExceptionHandler;
        info.hasDebugInfo        = f.hasDebugInfo;
        // v98 records the function kind in the header; v94-96 leave it 0 (the
        // generator/async recovery happens from the opcodes at disasm time).
        info.isGenerator         = f.kind == kFuncKindGenerator;
        info.isAsync             = f.kind == kFuncKindAsync;

        result.push_back(std::move(info));
    }

    markSharedBodies(result);
    return result;
}

} // namespace hbc
