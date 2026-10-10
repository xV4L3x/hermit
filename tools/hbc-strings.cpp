// hbc_strings <file.hbc>: every string with its kind from the String Kind
// Table: [string] is a runtime literal, [identifier] a property/method name.

#include "hbc/hbc.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: hbc-strings <file.hbc>\n");
        return EXIT_FAILURE;
    }
    try {
        hbc::HBCFile f = hbc::HBCFile::open(argv[1]);
        const auto& strings = f.strings();
        const auto& kinds   = f.stringKinds();

        std::printf("String count: %zu\n\n", strings.size());

        for (size_t i = 0; i < strings.size(); ++i) {
            const char* kindLabel =
                (kinds[i] == hbc::StringKind::Identifier) ? "identifier" : "string";

            // Quote first, then pad, so the padding falls outside the quotes.
            std::string quoted = "\"" + strings[i] + "\"";
            std::printf("[%4zu] %-24s [%s]\n", i, quoted.c_str(), kindLabel);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
