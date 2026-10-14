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

// ---------------------------------------------------------------------------
// String encodings in the file: an entry with isUTF16 = 0 holds ASCII (one byte
// per character), one with isUTF16 = 1 holds UTF-16LE (two bytes per code unit).
// The Hermes compiler picks UTF-16 as soon as a string has a non-ASCII character
// (StringTableBuilder: isAllASCII ? 8-bit : UTF-16). `length` counts code units.
// These tests build a minimal synthetic file (header + string kind run + small
// entries + storage) and check strings() (UTF-8) and string_entries().
// ---------------------------------------------------------------------------

namespace {

struct SynthString {
    bool                 utf16;
    std::vector<uint8_t> bytes;   // as stored
    uint32_t             length;  // code units
};

SynthString ascii(const std::string& s) {
    return {false, std::vector<uint8_t>(s.begin(), s.end()), static_cast<uint32_t>(s.size())};
}

SynthString utf16(std::initializer_list<uint16_t> units) {
    SynthString r{true, {}, static_cast<uint32_t>(units.size())};
    for (uint16_t u : units) {
        r.bytes.push_back(static_cast<uint8_t>(u & 0xFF));
        r.bytes.push_back(static_cast<uint8_t>(u >> 8));
    }
    return r;
}

hbc::HBCFile buildStringFile(const std::vector<SynthString>& strings) {
    std::vector<uint8_t> storage;
    std::vector<hbc::SmallStringTableEntry> table;
    for (const auto& s : strings) {
        hbc::SmallStringTableEntry e{};
        e.isUTF16 = s.utf16 ? 1 : 0;
        e.offset  = static_cast<uint32_t>(storage.size());
        e.length  = s.length;
        table.push_back(e);
        storage.insert(storage.end(), s.bytes.begin(), s.bytes.end());
    }

    hbc::HBCFileHeader hdr{};
    hdr.magic             = hbc::HBC_MAGIC;
    hdr.version           = 96;
    hdr.stringCount       = static_cast<uint32_t>(strings.size());
    hdr.stringKindCount   = 1;   // one run: all the strings are of kind String
    hdr.stringStorageSize = static_cast<uint32_t>(storage.size());

    const uint32_t kindRun = static_cast<uint32_t>(strings.size());   // bit 31 = 0: String
    std::string buf(sizeof(hdr) + 4 + table.size() * 4 + storage.size(), '\0');
    std::memcpy(buf.data() + sizeof(hdr), &kindRun, 4);
    std::memcpy(buf.data() + sizeof(hdr) + 4, table.data(), table.size() * 4);
    std::memcpy(buf.data() + sizeof(hdr) + 4 + table.size() * 4, storage.data(), storage.size());
    hdr.fileLength = static_cast<uint32_t>(buf.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    return hbc::HBCFile::from_bytes(buf);
}

const std::string REPLACEMENT = "\xEF\xBF\xBD";  // U+FFFD

} // namespace

TEST_CASE("ASCII entry: one byte per character, copied as it is",
          "[string-table][strings][encoding]") {
    auto f = buildStringFile({ascii("abc"), ascii("a b\tc/0:{}")});
    const auto& strs = f.strings();
    REQUIRE(strs[0] == "abc");
    REQUIRE(strs[1] == "a b\tc/0:{}");

    const auto entries = f.string_entries();
    REQUIRE_FALSE(entries[0].isUTF16);
    REQUIRE(entries[0].charLength == 3);
    REQUIRE(entries[0].byteLength == 3);
}

TEST_CASE("UTF-16 entry with accented characters: one code unit each, two bytes in the file",
          "[string-table][strings][encoding]") {
    // "caffè": five code units (63 61 66 66 E8), ten bytes in the file, six in UTF-8.
    auto f = buildStringFile({utf16({0x63, 0x61, 0x66, 0x66, 0xE8}),
                              utf16({0xE0, 0xE8, 0xE9, 0xEC, 0xF2, 0xF9})});
    const auto& strs = f.strings();
    REQUIRE(strs[0] == "caff\xC3\xA8");
    REQUIRE(strs[1] == "\xC3\xA0\xC3\xA8\xC3\xA9\xC3\xAC\xC3\xB2\xC3\xB9");

    const auto entries = f.string_entries();
    REQUIRE(entries[0].isUTF16);
    REQUIRE(entries[0].charLength == 5);      // code units
    REQUIRE(entries[0].byteLength == 10);     // bytes in the file
    REQUIRE(strs[0].size() == 6);             // bytes of the UTF-8 form
}

TEST_CASE("UTF-16 surrogate pair is one supplementary character and two code units",
          "[string-table][strings][encoding]") {
    // "x😀y": 78 D83D DE00 79, four code units, three characters.
    auto f = buildStringFile({utf16({0x78, 0xD83D, 0xDE00, 0x79}),
                              utf16({0xD835, 0xDCB3})});            // U+1D4B3 (mathematical script X)
    const auto& strs = f.strings();
    REQUIRE(strs[0] == "x\xF0\x9F\x98\x80y");
    REQUIRE(strs[1] == "\xF0\x9D\x92\xB3");

    const auto entries = f.string_entries();
    REQUIRE(entries[0].charLength == 4);
    REQUIRE(entries[0].byteLength == 8);
    REQUIRE(entries[1].charLength == 2);      // one character, two code units
}

TEST_CASE("UTF-16 isolated surrogates become U+FFFD, one per surrogate",
          "[string-table][strings][encoding]") {
    auto f = buildStringFile({
        utf16({0xD800}),                       // lone high
        utf16({0xDC00}),                       // lone low
        utf16({0x61, 0xD800, 0x62}),           // lone high between ASCII characters
        utf16({0xDC00, 0xD800}),               // low then high: not a pair
        utf16({0xD83D, 0xDE00, 0xD83D}),       // a pair followed by a lone high at the end
        utf16({0xD83D, 0x41}),                 // high followed by a non-surrogate
    });
    const auto& strs = f.strings();
    REQUIRE(strs[0] == REPLACEMENT);
    REQUIRE(strs[1] == REPLACEMENT);
    REQUIRE(strs[2] == "a" + REPLACEMENT + "b");
    REQUIRE(strs[3] == REPLACEMENT + REPLACEMENT);
    REQUIRE(strs[4] == "\xF0\x9F\x98\x80" + REPLACEMENT);
    REQUIRE(strs[5] == REPLACEMENT + "A");

    // The entry still reports the code units that are in the file.
    const auto entries = f.string_entries();
    REQUIRE(entries[3].charLength == 2);
    REQUIRE(entries[4].charLength == 3);
}

TEST_CASE("ASCII entry: a byte >= 0x80 is not decoded, it stays as stored",
          "[string-table][strings][encoding]") {
    // The compiler never writes one (a string with such a character is UTF-16);
    // a foreign file that does is returned byte for byte.
    auto f = buildStringFile({ascii(std::string("a\xC3\xA8", 3))});
    REQUIRE(f.strings()[0] == std::string("a\xC3\xA8", 3));
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
