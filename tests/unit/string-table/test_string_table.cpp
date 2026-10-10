#include <catch2/catch_test_macros.hpp>
#include "hbc/hbc.h"
#include <algorithm>
#include <cstring>

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
