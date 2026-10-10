// hbc_section_map <file.hbc>: every section with offset, size and alignment,
// then checks that the sections cover the whole file with no gap.

#include "hbc/hbc.h"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: hbc_section_map <file.hbc>\n");
        return EXIT_FAILURE;
    }
    try {
        hbc::HBCFile f = hbc::HBCFile::open(argv[1]);
        auto smap = f.section_map();
        const auto& h = f.header();

        std::printf("%-28s  %10s  %8s  %5s\n", "section", "offset", "size", "align");
        std::printf("%s\n", std::string(57, '-').c_str());

        for (const auto& s : smap) {
            std::printf("%-28s  0x%08X  %8u  %5u\n",
                s.name.c_str(), s.offset, s.size, s.align);
        }

        std::printf("%s\n", std::string(57, '-').c_str());
        std::printf("fileLength (from header): %u\n", h.fileLength);

        const auto& last = smap.back();
        std::printf("last section end:         %u\n", last.offset + last.size);

        if (last.offset + last.size == h.fileLength)
            std::printf("Coverage: OK (sections cover the whole file)\n");
        else
            std::printf("Coverage: GAP of %d bytes\n",
                        (int)h.fileLength - (int)(last.offset + last.size));

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
