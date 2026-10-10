// hbc_functions <file.hbc>: the function table, one row per function.
// `*` marks the global function (entry point); anonymous functions are shown
// as "anonymous_func_N".

#include "hbc/hbc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: hbc-functions <file.hbc>\n");
        return EXIT_FAILURE;
    }
    try {
        hbc::HBCFile f = hbc::HBCFile::open(argv[1]);
        const auto&    funcs  = f.functions();
        const uint32_t global = f.header().globalCodeIndex;

        std::printf("HBC version: %u\n", f.version());
        std::printf("Function count: %zu  |  Global index: %u\n\n",
            funcs.size(), global);

        std::printf("%-6s  %-30s  %-10s  %-6s  %-6s  %-5s  %s\n",
            "Index", "Name", "Offset", "Size", "Params", "Frame", "Flags");
        std::printf("%s\n",
            "------  ------------------------------  ----------  ------  ------  -----  -----");

        for (const auto& fi : funcs) {
            const char marker = (fi.index == global) ? '*' : ' ';

            char flags[32] = "";
            if (fi.isStrictMode) strncat(flags, "strict ", sizeof(flags) - strlen(flags) - 1);
            if (fi.isAsync)      strncat(flags, "async ",  sizeof(flags) - strlen(flags) - 1);
            if (fi.isGenerator)  strncat(flags, "gen ",    sizeof(flags) - strlen(flags) - 1);

            std::printf("[%3u]%c  %-30s  0x%08X  %6u  %6u  %5u  %s\n",
                fi.index,
                marker,
                fi.name.c_str(),
                fi.offset,
                fi.bytecodeSizeInBytes,
                fi.paramCount,
                fi.frameSize,
                flags);
        }

        std::printf("\n[*] = global function (entry point)\n");

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
