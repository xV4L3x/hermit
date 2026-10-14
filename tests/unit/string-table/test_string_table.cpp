#include <catch2/catch_test_macros.hpp>
#include "hbc/hbc.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

static const std::string HELLO = std::string(HBC_CORPUS_DIR) + "/hello.hbc";

TEST_CASE("string count matches header stringCount", "[string-table][strings]") {
    auto f = hbc::HBCFile::open(HELLO);
    REQUIRE(f.strings().size() == f.header().stringCount);
}

TEST_CASE("hello.hbc contains expected strings", "[string-table][strings]") {
    auto f = hbc::HBCFile::open(HELLO);
    const auto& strs = f.strings();
    auto has = [&](const std::string& s) {
        return std::find(strs.begin(), strs.end(), s) != strs.end();
    };
    REQUIRE(has("greet"));
    REQUIRE(has("console"));
    REQUIRE(has("log"));
    REQUIRE(has("global"));
    REQUIRE(has("use strict"));
}

TEST_CASE("strings() is idempotent (cache)", "[string-table][strings]") {
    auto f = hbc::HBCFile::open(HELLO);
    const auto& a = f.strings();
    const auto& b = f.strings();
    REQUIRE(a.size() == b.size());
    REQUIRE(&a == &b);  // same reference: the cache works
}

// Narrow (isUTF16=0) string storage is CESU-8, not plain UTF-8: a
// supplementary character (U+10000+, e.g. most emoji) is split into two
// independently UTF-8-encoded UTF-16 surrogate halves instead of UTF-8's
// single 4-byte sequence. This builds a minimal synthetic file (just
// enough for HBCFile::from_bytes() + strings(): header + one
// SmallStringTableEntry + its storage bytes, no other section) containing
// the CESU-8 encoding of U+1F600 (high surrogate D83D, low surrogate
// DE00 - each a 3-byte CESU-8 sequence: ED A0 BD / ED B8 80) and checks
// that strings() recombines them into the real 4-byte UTF-8 encoding
// (F0 9F 98 80), not two separate invalid/mangled sequences.
TEST_CASE("narrow CESU-8 surrogate pair decodes to real UTF-8", "[string-table][strings][cesu8]") {
    hbc::HBCFileHeader hdr{};
    hdr.magic             = hbc::HBC_MAGIC;
    hdr.version           = 96;
    hdr.stringCount       = 1;
    hdr.stringStorageSize = 6;

    hbc::SmallStringTableEntry entry{};
    entry.isUTF16 = 0;
    entry.offset  = 0;
    entry.length  = 6;  // byte count for narrow strings, not character count

    const uint8_t cesu8[6] = {0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80};

    std::string buf(sizeof(hdr) + sizeof(entry) + sizeof(cesu8), '\0');
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), &entry, sizeof(entry));
    std::memcpy(buf.data() + sizeof(hdr) + sizeof(entry), cesu8, sizeof(cesu8));

    hdr.fileLength = static_cast<uint32_t>(buf.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));  // rewrite header with fileLength set

    auto f = hbc::HBCFile::from_bytes(buf);
    const auto& strs = f.strings();
    REQUIRE(strs.size() == 1);
    REQUIRE(strs[0] == std::string("\xF0\x9F\x98\x80", 4));
}

// A small entry with length == 255 carries in `offset` the index of its entry in
// the Overflow String Table (Hermes' BytecodeDataProvider::getStringTableEntry
// reads overflowStringTableEntries_[small.offset]). The synthetic files below
// name the slots in descending order, name the same slot twice and name a slot
// outside the table.
static std::string buildOverflowFile(const std::vector<uint32_t>& sentinelIndices,
                                     uint32_t overflowCount) {
    // String table: [sentinel, small "x", sentinel ...]; storage "alphabravo!x".
    //   overflow slot 0 = "alpha"  (offset 0, length 5)
    //   overflow slot 1 = "bravo!" (offset 5, length 6)
    const uint8_t storage[] = {'a','l','p','h','a','b','r','a','v','o','!','x'};
    const uint32_t stringCount = static_cast<uint32_t>(sentinelIndices.size()) + 1;

    hbc::HBCFileHeader hdr{};
    hdr.magic                = hbc::HBC_MAGIC;
    hdr.version              = 96;
    hdr.stringCount          = stringCount;
    hdr.overflowStringCount  = overflowCount;
    hdr.stringStorageSize    = sizeof(storage);

    std::vector<hbc::SmallStringTableEntry> small;
    for (size_t i = 0; i < sentinelIndices.size(); ++i) {
        hbc::SmallStringTableEntry e{};
        e.isUTF16 = 0;
        e.offset  = sentinelIndices[i];
        e.length  = 255;
        small.push_back(e);
        if (i == 0) {                       // a plain small entry between the sentinels
            hbc::SmallStringTableEntry s{};
            s.isUTF16 = 0; s.offset = 11; s.length = 1;
            small.push_back(s);
        }
    }
    const hbc::OverflowStringTableEntry ovf[2] = {{0, 5}, {5, 6}};

    std::string buf(sizeof(hdr) + small.size() * 4 + overflowCount * 8 + sizeof(storage), '\0');
    size_t pos = sizeof(hdr);
    std::memcpy(buf.data() + pos, small.data(), small.size() * 4);   pos += small.size() * 4;
    std::memcpy(buf.data() + pos, ovf, overflowCount * 8);           pos += overflowCount * 8;
    std::memcpy(buf.data() + pos, storage, sizeof(storage));
    hdr.fileLength = static_cast<uint32_t>(buf.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    return buf;
}

TEST_CASE("overflow entry is addressed by the small entry's offset",
          "[string-table][strings][overflow]") {
    // [0] -> slot 1, [1] = plain "x", [2] -> slot 0
    auto f = hbc::HBCFile::from_bytes(buildOverflowFile({1, 0}, 2));
    const auto& strs = f.strings();
    REQUIRE(strs.size() == 3);
    REQUIRE(strs[0] == "bravo!");
    REQUIRE(strs[1] == "x");
    REQUIRE(strs[2] == "alpha");
}

TEST_CASE("two entries may refer to the same overflow slot",
          "[string-table][strings][overflow]") {
    auto f = hbc::HBCFile::from_bytes(buildOverflowFile({1, 1}, 2));
    const auto& strs = f.strings();
    REQUIRE(strs[0] == "bravo!");
    REQUIRE(strs[2] == "bravo!");
}

TEST_CASE("overflow index outside the table is rejected",
          "[string-table][strings][overflow]") {
    auto f = hbc::HBCFile::from_bytes(buildOverflowFile({2, 0}, 2));
    REQUIRE_THROWS_AS(f.strings(), std::runtime_error);
}
