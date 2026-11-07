// =============================================================================
// test_slp.cpp - Unit tests for the SLP (SerializedLiteral) buffer decoder
// =============================================================================
// Hand-built buffers, one per tag and per run-header shape. Format: a tag byte
// [ext:1][tag:3][len:4] introduces `len` elements of one tag, each followed by
// its little-endian value bytes (Null/True/False: none, Number: 8, Integer and
// LongString: 4, ShortString: 2, ByteString: 1). With ext=1 the length is
// [len:4] << 8 | next byte.
// =============================================================================
#include <catch2/catch_test_macros.hpp>

#include "util/slp.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

using hbc::SLPTag;
using hbc::decodeSLP;

static std::string bytes(std::initializer_list<int> b) {
    std::string s;
    for (int v : b) s.push_back(static_cast<char>(v));
    return s;
}

TEST_CASE("slp: zero elements decode to an empty vector, even on an empty buffer", "[slp]") {
    auto r = decodeSLP("", 0, 0);
    REQUIRE(r.has_value());
    CHECK(r->empty());
}

TEST_CASE("slp: zero-width tags", "[slp]") {
    // Null x2, True x1, False x3
    auto r = decodeSLP(bytes({0x02, 0x11, 0x23}), 0, 6);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 6);
    CHECK((*r)[0].tag == SLPTag::Null);
    CHECK((*r)[1].tag == SLPTag::Null);
    CHECK((*r)[2].tag == SLPTag::True);
    CHECK((*r)[3].tag == SLPTag::False);
    CHECK((*r)[5].tag == SLPTag::False);
}

TEST_CASE("slp: Number is an 8-byte little-endian double", "[slp]") {
    std::string buf = bytes({0x31});
    double d = 1.5;
    buf.append(reinterpret_cast<const char*>(&d), 8);
    auto r = decodeSLP(buf, 0, 1);
    REQUIRE(r.has_value());
    CHECK((*r)[0].tag == SLPTag::Number);
    CHECK((*r)[0].number == 1.5);
}

TEST_CASE("slp: Integer is a signed int32", "[slp]") {
    auto r = decodeSLP(bytes({0x72, 0xFE, 0xFF, 0xFF, 0xFF, 0x2A, 0x00, 0x00, 0x00}), 0, 2);
    REQUIRE(r.has_value());
    CHECK((*r)[0].tag == SLPTag::Integer);
    CHECK((*r)[0].integer == -2);
    CHECK((*r)[1].integer == 42);
}

TEST_CASE("slp: string tags carry a String Table index of width 4, 2 and 1", "[slp]") {
    SECTION("LongString (4 bytes)") {
        auto r = decodeSLP(bytes({0x41, 0x78, 0x56, 0x34, 0x12}), 0, 1);
        REQUIRE(r.has_value());
        CHECK((*r)[0].tag == SLPTag::LongString);
        CHECK((*r)[0].stringIndex == 0x12345678u);
        CHECK(hbc::isSLPStringTag((*r)[0].tag));
    }
    SECTION("ShortString (2 bytes)") {
        auto r = decodeSLP(bytes({0x52, 0x02, 0x01, 0x05, 0x00}), 0, 2);
        REQUIRE(r.has_value());
        CHECK((*r)[0].tag == SLPTag::ShortString);
        CHECK((*r)[0].stringIndex == 0x0102u);
        CHECK((*r)[1].stringIndex == 5u);
    }
    SECTION("ByteString (1 byte)") {
        auto r = decodeSLP(bytes({0x61, 0xFF}), 0, 1);
        REQUIRE(r.has_value());
        CHECK((*r)[0].tag == SLPTag::ByteString);
        CHECK((*r)[0].stringIndex == 255u);
    }
}

TEST_CASE("slp: non-string tags are not string tags", "[slp]") {
    CHECK_FALSE(hbc::isSLPStringTag(SLPTag::Null));
    CHECK_FALSE(hbc::isSLPStringTag(SLPTag::Number));
    CHECK_FALSE(hbc::isSLPStringTag(SLPTag::Integer));
}

TEST_CASE("slp: extended run header (ext=1) carries up to 4095 elements", "[slp]") {
    // ext=1, tag=True (1), len high nibble 1, low byte 0x2C -> 300 elements
    auto r = decodeSLP(bytes({0x91, 0x2C}), 0, 300);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 300);
    CHECK((*r)[299].tag == SLPTag::True);
}

TEST_CASE("slp: several runs and a non-zero start offset", "[slp]") {
    // 3 junk bytes, then: Integer x1 (7), ShortString x1 (idx 9)
    auto r = decodeSLP(bytes({0xAA, 0xBB, 0xCC, 0x71, 0x07, 0x00, 0x00, 0x00, 0x51, 0x09, 0x00}), 3, 2);
    REQUIRE(r.has_value());
    CHECK((*r)[0].integer == 7);
    CHECK((*r)[1].stringIndex == 9u);
}

TEST_CASE("slp: stops at count even in the middle of a run", "[slp]") {
    auto r = decodeSLP(bytes({0x05}), 0, 2);   // run of 5 Nulls, only 2 wanted
    REQUIRE(r.has_value());
    CHECK(r->size() == 2);
}

TEST_CASE("slp: truncated or wrong-offset buffers return nullopt", "[slp]") {
    SECTION("fewer elements than count") {
        CHECK_FALSE(decodeSLP(bytes({0x02}), 0, 3).has_value());
    }
    SECTION("value bytes cut short") {
        CHECK_FALSE(decodeSLP(bytes({0x71, 0x01, 0x00}), 0, 1).has_value());
    }
    SECTION("Number cut short") {
        CHECK_FALSE(decodeSLP(bytes({0x31, 1, 2, 3, 4, 5, 6, 7}), 0, 1).has_value());
    }
    SECTION("extended header cut short") {
        CHECK_FALSE(decodeSLP(bytes({0x91}), 0, 1).has_value());
    }
    SECTION("start beyond the buffer") {
        CHECK_FALSE(decodeSLP(bytes({0x01}), 5, 1).has_value());
    }
    SECTION("absurd count does not allocate") {
        CHECK_FALSE(decodeSLP(bytes({0x01}), 0, 0xFFFFFFFFu).has_value());
    }
}
