#include <catch2/catch_test_macros.hpp>
#include "hbc/hbc.h"
#include "hbc/format.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// HBC_CORPUS_DIR is injected by CMake so tests can find corpus files regardless
// of the working directory from which the test binary is run.
#ifndef HBC_CORPUS_DIR
#  error "HBC_CORPUS_DIR must be set by CMake"
#endif

static const std::string HELLO_HBC = std::string(HBC_CORPUS_DIR) + "/hello.hbc";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Write a minimal synthetic HBC file with the given magic and version so we
// can test error paths without depending on real HBC files.
static std::string make_synthetic(uint64_t magic, uint32_t version) {
    // Allocate a 128-byte buffer (minimum header size) and fill with zeros.
    std::vector<uint8_t> buf(128, 0);
    std::memcpy(buf.data() + 0, &magic,   sizeof(magic));
    std::memcpy(buf.data() + 8, &version, sizeof(version));

    auto path = std::filesystem::temp_directory_path() /
                ("hermit_test_" + std::to_string(version) + ".hbc");
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return path.string();
}

// ---------------------------------------------------------------------------
// Smoke tests - basic open + version
// ---------------------------------------------------------------------------

TEST_CASE("hello.hbc has the correct magic", "[header][smoke]") {
    auto f = hbc::HBCFile::open(HELLO_HBC);
    REQUIRE(f.header().magic == hbc::HBC_MAGIC);
}

TEST_CASE("hello.hbc has version 96", "[header][smoke]") {
    auto f = hbc::HBCFile::open(HELLO_HBC);
    REQUIRE(f.version() == 96u);
    REQUIRE(f.header().version == 96u);
}

// ---------------------------------------------------------------------------
// Header field values - cross-checked against hello.annotated.hex
// ---------------------------------------------------------------------------

TEST_CASE("hello.hbc header fields match annotated hex", "[header][header]") {
    auto f = hbc::HBCFile::open(HELLO_HBC);
    const auto& h = f.header();

    // From hello.annotated.hex: file is 564 bytes (0x234)
    REQUIRE(h.fileLength == 564u);

    // Function[0] is the top-level "global" function
    REQUIRE(h.globalCodeIndex == 0u);

    // hello.ts compiles to 2 functions: global + greet
    REQUIRE(h.functionCount == 2u);

    // String table: 8 entries total (verified in annotated hex)
    REQUIRE(h.stringCount == 8u);

    // No overflow strings in this simple file
    REQUIRE(h.overflowStringCount == 0u);

    // Debug info section starts at 0x16C = 364
    REQUIRE(h.debugInfoOffset == 0x16Cu);

    // Simple file: no BigInts, no regexps, no array/object literals
    REQUIRE(h.bigIntCount == 0u);
    REQUIRE(h.regExpCount == 0u);
    REQUIRE(h.arrayBufferSize == 0u);

    // No CJS modules in a plain TS file
    REQUIRE(h.cjsModuleCount == 0u);

    // Segment 0 for a monolithic bundle
    REQUIRE(h.segmentID == 0u);
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST_CASE("non-HBC file throws InvalidMagicError", "[header][error]") {
    // Use this source file itself as a non-HBC input
    REQUIRE_THROWS_AS(
        hbc::HBCFile::open(__FILE__),
        hbc::InvalidMagicError
    );
}

TEST_CASE("unsupported version throws UnsupportedVersionError", "[header][error]") {
    auto path = make_synthetic(hbc::HBC_MAGIC, 999u);
    REQUIRE_THROWS_AS(
        hbc::HBCFile::open(path),
        hbc::UnsupportedVersionError
    );
    std::filesystem::remove(path);
}

TEST_CASE("file too small throws FileTooSmallError", "[header][error]") {
    // Create a 4-byte file (shorter than the 128-byte header)
    auto path = (std::filesystem::temp_directory_path() / "hermit_too_small.hbc").string();
    {
        std::ofstream f(path, std::ios::binary);
        uint64_t m = hbc::HBC_MAGIC;
        f.write(reinterpret_cast<const char*>(&m), 4);
    }
    REQUIRE_THROWS_AS(
        hbc::HBCFile::open(path),
        hbc::FileTooSmallError
    );
    std::filesystem::remove(path);
}
