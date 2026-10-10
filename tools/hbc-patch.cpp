// hbc_patch <input.hbc> --offset <hex> --value <hex> -o <output.hbc>
//
// Overwrites a single byte of an HBC file. It does NOT update the SHA-1
// footer on purpose: the Hermes runtime doesn't verify it, which is how that
// was established. Structural edits go through `Module`, which recomputes it.

#include "hbc/hbc.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 8) {
        std::fprintf(stderr,
            "Usage: hbc_patch <input.hbc> --offset <hex> --value <hex> -o <output.hbc>\n");
        return EXIT_FAILURE;
    }

    const char* input_path  = argv[1];
    uint32_t    offset      = static_cast<uint32_t>(std::stoul(argv[3], nullptr, 0));
    uint8_t     value       = static_cast<uint8_t> (std::stoul(argv[5], nullptr, 0));
    const char* output_path = argv[7];

    try {
        hbc::HBCFile f = hbc::HBCFile::open(input_path);
        auto bytes = f.serialize();

        if (offset >= bytes.size())
            throw std::out_of_range("offset 0x" + std::to_string(offset) +
                                    " is beyond file size " + std::to_string(bytes.size()));

        uint8_t old_value = bytes[offset];
        bytes[offset] = value;
        std::printf("Patched offset 0x%08X: 0x%02X -> 0x%02X\n", offset, old_value, value);

        std::ofstream out(output_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::printf("Written %zu bytes to %s\n", bytes.size(), output_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
