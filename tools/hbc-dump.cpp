// hbc_dump <file.hbc>: prints every field of the HBC header.

#include "hbc/hbc.h"
#include "hbc/format.h"

#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

// {0x7E, 0xED, 0x64} -> "7E ED 64"
static std::string hex_bytes(const uint8_t* data, size_t len) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) ss << ' ';
        ss << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return ss.str();
}

static void dump(const hbc::HBCFile& f) {
    const auto& h = f.header();
    hbc::BytecodeOptions opts{h.options};

    std::printf("%-28s 0x%016llX  ✓\n", "Magic:",
                static_cast<unsigned long long>(h.magic));
    std::printf("%-28s %u\n",           "Version:",              h.version);
    std::printf("%-28s %s\n",           "Source SHA-1:",
                hex_bytes(h.sourceHash, 20).c_str());
    std::printf("%-28s %u bytes\n",     "File length:",          h.fileLength);
    std::printf("%-28s %u\n",           "Global code index:",    h.globalCodeIndex);
    std::printf("%-28s %u\n",           "Function count:",       h.functionCount);
    std::printf("%-28s %u\n",           "String kind count:",    h.stringKindCount);
    std::printf("%-28s %u\n",           "Identifier count:",     h.identifierCount);
    std::printf("%-28s %u\n",           "String count:",         h.stringCount);
    std::printf("%-28s %u\n",           "Overflow string count:", h.overflowStringCount);
    std::printf("%-28s %u bytes\n",     "String storage:",       h.stringStorageSize);
    std::printf("%-28s %u\n",           "BigInt count:",         h.bigIntCount);
    std::printf("%-28s %u bytes\n",     "BigInt storage:",       h.bigIntStorageSize);
    std::printf("%-28s %u\n",           "RegExp count:",         h.regExpCount);
    std::printf("%-28s %u bytes\n",     "RegExp storage:",       h.regExpStorageSize);
    std::printf("%-28s %u bytes\n",     "Array buffer:",         h.arrayBufferSize);
    std::printf("%-28s %u bytes\n",     "Object key buffer:",    h.objKeyBufferSize);
    std::printf("%-28s %u bytes\n",     "Object value buffer:",  h.objValueBufferSize);
    std::printf("%-28s %u\n",           "Segment ID:",           h.segmentID);
    std::printf("%-28s %u\n",           "CJS module count:",     h.cjsModuleCount);
    std::printf("%-28s %u\n",           "Function source count:", h.functionSourceCount);
    std::printf("%-28s 0x%08X\n",       "Debug info offset:",    h.debugInfoOffset);
    std::printf("%-28s 0x%02X  (staticBuiltins=%d cjsStatic=%d hasAsync=%d)\n",
                "Options:", h.options,
                opts.bits.staticBuiltins,
                opts.bits.cjsModulesStaticallyResolved,
                opts.bits.hasAsync);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: hbc-dump <file.hbc>\n");
        return EXIT_FAILURE;
    }
    try {
        dump(hbc::HBCFile::open(argv[1]));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
