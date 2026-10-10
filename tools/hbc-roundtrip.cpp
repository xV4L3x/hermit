// hbc_roundtrip <input.hbc> <output.hbc>: parses and re-serializes a file.
// The output must be byte-identical to the input (check with `cmp`).

#include "hbc/hbc.h"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <ios>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: hbc_roundtrip <input.hbc> <output.hbc>\n");
        return EXIT_FAILURE;
    }
    try {
        hbc::HBCFile f = hbc::HBCFile::open(argv[1]);
        auto bytes = f.serialize();

        std::ofstream out(argv[2], std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());

        std::printf("Written %zu bytes to %s\n", bytes.size(), argv[2]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
