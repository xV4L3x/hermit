#include <catch2/catch_test_macros.hpp>
#include "hbc/hbc.h"
#include <cstring>
#include <fstream>

static const std::string HELLO = std::string(HBC_CORPUS_DIR) + "/hello.hbc";

TEST_CASE("serialize size matches fileLength", "[roundtrip][roundtrip]") {
    auto f = hbc::HBCFile::open(HELLO);
    auto bytes = f.serialize();
    REQUIRE(bytes.size() == f.header().fileLength);
}

TEST_CASE("serialize is byte-identical to source", "[roundtrip][roundtrip]") {
    auto f = hbc::HBCFile::open(HELLO);
    auto bytes = f.serialize();
    REQUIRE(bytes.size() == f.raw_size());
    REQUIRE(std::memcmp(bytes.data(), f.raw_data(), f.raw_size()) == 0);
}

TEST_CASE("section_map covers whole file", "[roundtrip][section_map]") {
    auto f = hbc::HBCFile::open(HELLO);
    auto smap = f.section_map();
    REQUIRE_FALSE(smap.empty());
    const auto& last = smap.back();
    REQUIRE(last.offset + last.size == f.header().fileLength);
}

TEST_CASE("section_map has no gaps between contiguous sections", "[roundtrip][section_map]") {
    auto f = hbc::HBCFile::open(HELLO);
    auto smap = f.section_map();
    for (size_t i = 1; i < smap.size(); ++i) {
        uint32_t prev_end  = smap[i-1].offset + smap[i-1].size;
        uint32_t curr_start = smap[i].offset;
        // There can be alignment padding between sections (< 4 bytes), but never overlap
        REQUIRE(curr_start >= prev_end);
        REQUIRE(curr_start - prev_end < 4);
    }
}