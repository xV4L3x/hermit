// =============================================================================
// test_literals.cpp - literal-buffer sites + buffer accessors
// =============================================================================
// hbc::HBCFile::literal_sites() enumerates the NewArrayWithBuffer[Long] /
// NewObjectWithBuffer[Long] sites (function + bufferIdx). array_buffer() /
// obj_key_buffer() / obj_value_buffer() expose the three buffers' raw bytes
// (the SLP decode/encode lives in Python, cli/literals.py).
// HBC_CORPUS_DIR is injected by CMake.
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "hbc/hbc.h"
#include "test_util.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using testutil::openOrSkip;

static const std::string MEDIUM = std::string(HBC_CORPUS_DIR) + "/medium.hbc";

// ---------------------------------------------------------------------------

TEST_CASE("literal_sites enumerates New{Array,Object}WithBuffer sites", "[patcher]") {
    auto f = hbc::HBCFile::open(MEDIUM);
    const auto& sites = f.literal_sites();

    REQUIRE(sites.size() == 2);
    bool sawArray = false, sawObject = false;
    for (const auto& s : sites) {
        REQUIRE(s.functionIndex < f.functions().size());
        if (s.isObject) {
            sawObject = true;
            REQUIRE(s.mnemonic == "NewObjectWithBuffer");
            REQUIRE(s.count == 3);
        } else {
            sawArray = true;
            REQUIRE(s.mnemonic == "NewArrayWithBuffer");
            REQUIRE(s.count == 4);
            REQUIRE(s.keyBufferIdx == 0);
        }
    }
    REQUIRE(sawArray);
    REQUIRE(sawObject);
}

TEST_CASE("array_buffer/obj_key_buffer/obj_value_buffer match the raw file bytes", "[patcher]") {
    auto f = hbc::HBCFile::open(MEDIUM);
    const auto& h = f.header();

    REQUIRE(f.array_buffer().size()     == h.arrayBufferSize);
    REQUIRE(f.obj_key_buffer().size()   == h.objKeyBufferSize);
    REQUIRE(f.obj_value_buffer().size() == h.objValueBufferSize);

    const uint8_t* raw = f.raw_data();
    REQUIRE(std::string(reinterpret_cast<const char*>(raw + f.array_buffer_offset()),
                        h.arrayBufferSize) == f.array_buffer());
    REQUIRE(std::string(reinterpret_cast<const char*>(raw + f.obj_key_buffer_offset()),
                        h.objKeyBufferSize) == f.obj_key_buffer());
    REQUIRE(std::string(reinterpret_cast<const char*>(raw + f.obj_value_buffer_offset()),
                        h.objValueBufferSize) == f.obj_value_buffer());
}

TEST_CASE("literal_sites cache is idempotent", "[patcher]") {
    auto f = hbc::HBCFile::open(MEDIUM);
    REQUIRE(&f.literal_sites() == &f.literal_sites());
}

TEST_CASE("literal_sites survive a serialize round-trip", "[patcher]") {
    auto f = hbc::HBCFile::open(MEDIUM);
    auto bytes = f.serialize();
    auto g = hbc::HBCFile::from_bytes(std::string(bytes.begin(), bytes.end()));

    const auto& a = f.literal_sites();
    const auto& b = g.literal_sites();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].functionIndex     == b[i].functionIndex);
        REQUIRE(a[i].instructionOffset == b[i].instructionOffset);
        REQUIRE(a[i].mnemonic          == b[i].mnemonic);
        REQUIRE(a[i].isObject          == b[i].isObject);
        REQUIRE(a[i].count             == b[i].count);
        REQUIRE(a[i].bufferIdx         == b[i].bufferIdx);
        REQUIRE(a[i].keyBufferIdx      == b[i].keyBufferIdx);
    }
    REQUIRE(g.array_buffer() == f.array_buffer());
}

TEST_CASE("literals_g0.hbc has the expected single array site", "[patcher]") {
    auto f = openOrSkip(std::string(HBC_CORPUS_DIR) + "/literals_g0.hbc");
    const auto& sites = f.literal_sites();
    if (sites.size() != 1 || sites[0].isObject)
        SKIP("literals_g0.hbc: unexpected compiler layout");

    REQUIRE(sites[0].mnemonic == "NewArrayWithBuffer");
    REQUIRE(sites[0].count == 3);
    REQUIRE(sites[0].bufferIdx == 0);

    // [10, 20, 30] -> tag Integer (7), run 3, then 3 int32 LE (verified
    // manually against `hermes -dump-bytecode`).
    static const uint8_t kExpected[] = {
        0x73, 0x0a, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00,
    };
    const auto& ab = f.array_buffer();
    REQUIRE(ab.size() == sizeof(kExpected));
    REQUIRE(std::string(reinterpret_cast<const char*>(kExpected), sizeof(kExpected)) == ab);
}

TEST_CASE("literal_sites on a real bundle stays in bounds", "[patcher]") {
    auto f = openOrSkip(std::string(HBC_CORPUS_DIR) + "/ridemovi.hbc");
    const auto& sites = f.literal_sites();
    REQUIRE(sites.size() > 20000);
    for (const auto& s : sites)
        REQUIRE(s.functionIndex < f.functions().size());
}
