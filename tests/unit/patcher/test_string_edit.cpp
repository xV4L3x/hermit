// =============================================================================
// test_string_edit.cpp - per-string metadata accessor
// =============================================================================
// hbc::HBCFile::string_entries() exposes (absolute offset, bytes, characters,
// encoding, overflow, kind) for every string: the `edit string:<idx>` patcher
// uses it to do a same-length splice on the string storage.
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

static const std::string HELLO = std::string(HBC_CORPUS_DIR) + "/hello.hbc";

// Index of the first string equal to `want`, or -1.
static int idxOf(const hbc::HBCFile& f, const std::string& want) {
    const auto& ss = f.strings();
    for (size_t i = 0; i < ss.size(); ++i)
        if (ss[i] == want) return static_cast<int>(i);
    return -1;
}

// True if a's and b's half-open storage ranges intersect.
static bool overlaps(const hbc::StringEntry& a, const hbc::StringEntry& b) {
    uint32_t a0 = a.storageOffset, a1 = a0 + a.byteLength;
    uint32_t b0 = b.storageOffset, b1 = b0 + b.byteLength;
    return a0 < b1 && b0 < a1;
}

// ---------------------------------------------------------------------------

TEST_CASE("string_entries parallel to strings/string_kinds", "[patcher]") {
    auto f = hbc::HBCFile::open(HELLO);
    const auto& ents  = f.string_entries();
    const auto& kinds = f.stringKinds();

    REQUIRE(ents.size() == f.header().stringCount);
    REQUIRE(ents.size() == kinds.size());
    for (size_t i = 0; i < ents.size(); ++i)
        REQUIRE(ents[i].kind == kinds[i]);
}

TEST_CASE("string_entries offsets and byte lengths are coherent", "[patcher]") {
    auto f = hbc::HBCFile::open(HELLO);
    const auto& ents = f.string_entries();
    const auto& ss   = f.strings();

    // start of the string storage from the section map
    uint32_t storageStart = 0;
    for (const auto& s : f.section_map())
        if (s.name == "string storage") storageStart = s.offset;
    REQUIRE(storageStart != 0);

    const uint32_t limit = f.header().fileLength - 20;  // footer excluded
    for (size_t i = 0; i < ents.size(); ++i) {
        const auto& e = ents[i];
        REQUIRE(e.storageOffset >= storageStart);
        REQUIRE(e.storageOffset + e.byteLength <= limit);
        if (!e.isOverflow)
            REQUIRE(e.byteLength == (e.isUTF16 ? e.charLength * 2 : e.charLength));
        if (!e.isUTF16)  // all-ASCII corpus: bytes == characters == len(std::string)
            REQUIRE(ss[i].size() == e.byteLength);
    }
}

TEST_CASE("hello.hbc \"Hello, \" is a standalone String entry", "[patcher]") {
    auto f = hbc::HBCFile::open(HELLO);
    const auto& ents = f.string_entries();
    int i = idxOf(f, "Hello, ");
    REQUIRE(i == 1);

    const auto& e = ents[i];
    REQUIRE_FALSE(e.isOverflow);
    REQUIRE_FALSE(e.isUTF16);
    REQUIRE(e.kind == hbc::StringKind::String);
    REQUIRE(e.byteLength == 7);

    for (size_t j = 0; j < ents.size(); ++j)
        if (static_cast<int>(j) != i && ents[j].byteLength != 0)
            REQUIRE_FALSE(overlaps(e, ents[j]));
}

TEST_CASE("hello.hbc \"log\" and \"global\" storage ranges overlap", "[patcher]") {
    auto f = hbc::HBCFile::open(HELLO);
    const auto& ents = f.string_entries();
    int a = idxOf(f, "log"), b = idxOf(f, "global");
    REQUIRE(a >= 0);
    REQUIRE(b >= 0);
    REQUIRE(overlaps(ents[a], ents[b]));
}

TEST_CASE("hello.hbc identifier strings carry kind Identifier", "[patcher]") {
    auto f = hbc::HBCFile::open(HELLO);
    const auto& ents = f.string_entries();
    for (const char* name : {"log", "console", "greet"}) {
        int i = idxOf(f, name);
        REQUIRE(i >= 0);
        REQUIRE(ents[i].kind == hbc::StringKind::Identifier);
    }
}

TEST_CASE("string_entries cache is idempotent", "[patcher]") {
    auto f = hbc::HBCFile::open(HELLO);
    REQUIRE(&f.string_entries() == &f.string_entries());
}

TEST_CASE("string_entries survive a serialize round-trip", "[patcher]") {
    auto f = hbc::HBCFile::open(HELLO);
    auto bytes = f.serialize();
    auto g = hbc::HBCFile::from_bytes(std::string(bytes.begin(), bytes.end()));

    const auto& a = f.string_entries();
    const auto& b = g.string_entries();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].storageOffset == b[i].storageOffset);
        REQUIRE(a[i].byteLength    == b[i].byteLength);
        REQUIRE(a[i].charLength    == b[i].charLength);
        REQUIRE(a[i].isUTF16       == b[i].isUTF16);
        REQUIRE(a[i].isOverflow    == b[i].isOverflow);
        REQUIRE(a[i].kind          == b[i].kind);
    }
}

TEST_CASE("greeting_g0.hbc \"Hello, world\" entry is editable-shaped", "[patcher]") {
    auto f = openOrSkip(std::string(HBC_CORPUS_DIR) + "/greeting_g0.hbc");
    const auto& ents = f.string_entries();
    int i = idxOf(f, "Hello, world");
    if (i < 0)
        SKIP("greeting_g0.hbc does not contain \"Hello, world\" (compiler layout)");

    const auto& e = ents[i];
    REQUIRE(e.kind == hbc::StringKind::String);
    REQUIRE_FALSE(e.isOverflow);
    REQUIRE_FALSE(e.isUTF16);
    REQUIRE(e.byteLength == 12);
    for (size_t j = 0; j < ents.size(); ++j)
        if (static_cast<int>(j) != i && ents[j].byteLength != 0)
            REQUIRE_FALSE(overlaps(e, ents[j]));
}
