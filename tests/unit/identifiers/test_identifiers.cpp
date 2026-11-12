// Identifier strings: the Identifier Hash Table and Module::add_identifier.
//
// Oracle: the hash table the Hermes compiler wrote. identifier_hash() recomputed
// over the bytes of every Identifier entry must give the stored value, on toy
// samples and on the real bundles (ridemovi.hbc has UTF-16 identifiers).

#include "test_util.h"

#include "hbc/hbc.h"
#include "hbc/module.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

using namespace hbc;

namespace {

const std::string CORPUS = HBC_CORPUS_DIR;

uint32_t hashOf(const std::string& s) {
    return identifier_hash(reinterpret_cast<const uint8_t*>(s.data()), s.size(), false);
}

// identifier_hash() of every Identifier entry, in string-table order.
std::vector<uint32_t> recomputedHashes(const HBCFile& f) {
    std::vector<uint32_t> out;
    const auto& entries = f.string_entries();
    for (const StringEntry& e : entries) {
        if (e.kind != StringKind::Identifier) continue;
        out.push_back(identifier_hash(f.raw_data() + e.storageOffset, e.byteLength, e.isUTF16));
    }
    return out;
}

HBCFile reparse(const std::vector<uint8_t>& bytes) {
    return HBCFile::from_bytes(std::string(bytes.begin(), bytes.end()));
}

}  // namespace

TEST_CASE("identifier_hash: Jenkins one-at-a-time, no final mix", "[identifiers]") {
    CHECK(hashOf("") == 0u);

    // h = 0; h += 'a' (97); h += h << 10; h ^= h >> 6
    uint32_t h = 97;
    h += h << 10;
    h ^= h >> 6;
    CHECK(hashOf("a") == h);

    // An ASCII string and its UTF-16LE form hash the same (code units, not bytes).
    const std::vector<uint8_t> u16 = {'a', 0, 'b', 0, 'c', 0};
    CHECK(identifier_hash(u16.data(), u16.size(), true) == hashOf("abc"));

    const std::vector<uint8_t> odd = {'a', 0, 'b'};
    CHECK_THROWS_AS(identifier_hash(odd.data(), odd.size(), true), std::invalid_argument);
}

TEST_CASE("identifier_hash matches the stored table on the corpus", "[identifiers]") {
    for (const char* name : {"hello.hbc", "identifiers_g0.hbc", "cjs_modules.hbc",
                             "hermitvault.hbc", "ridemovi.hbc"}) {
        INFO(name);
        HBCFile f = testutil::openOrSkip(CORPUS + "/" + name);
        const auto stored = f.identifier_hashes();
        CHECK(stored.size() == f.header().identifierCount);
        CHECK(recomputedHashes(f) == stored);
    }
}

TEST_CASE("add_identifier appends entry, Identifier run and hash", "[identifiers]") {
    HBCFile f = testutil::openOrSkip(CORPUS + "/identifiers_g0.hbc");
    const uint32_t n0  = f.header().stringCount;
    const uint32_t id0 = f.header().identifierCount;
    const auto hashes0 = f.identifier_hashes();
    const auto kinds0  = f.stringKinds();
    REQUIRE(kinds0.back() == StringKind::Identifier);   // the last run is Identifier

    Module m = Module::from(f);
    CHECK(m.add_identifier("newName") == n0);
    HBCFile g = reparse(m.serialize());

    CHECK(g.header().stringCount == n0 + 1);
    CHECK(g.header().identifierCount == id0 + 1);
    // Extends the last run: no new kind entry.
    CHECK(g.header().stringKindCount == f.header().stringKindCount);
    CHECK(g.strings()[n0] == "newName");
    CHECK(g.stringKinds()[n0] == StringKind::Identifier);

    const auto hashes1 = g.identifier_hashes();
    REQUIRE(hashes1.size() == hashes0.size() + 1);
    CHECK(std::vector<uint32_t>(hashes1.begin(), hashes1.end() - 1) == hashes0);
    CHECK(hashes1.back() == hashOf("newName"));
    CHECK(recomputedHashes(g) == hashes1);

    // Existing strings and kinds unchanged.
    for (uint32_t i = 0; i < n0; ++i) {
        CHECK(g.strings()[i] == f.strings()[i]);
        CHECK(g.stringKinds()[i] == kinds0[i]);
    }
}

TEST_CASE("add_string + add_identifier: runs alternate, hashes only for identifiers",
          "[identifiers]") {
    HBCFile f = testutil::openOrSkip(CORPUS + "/identifiers_g0.hbc");
    const uint32_t n0 = f.header().stringCount;
    const auto hashes0 = f.identifier_hashes();

    Module m = Module::from(f);
    CHECK(m.add_string("plain one") == n0);
    CHECK(m.add_identifier("idOne") == n0 + 1);
    CHECK(m.add_identifier("idTwo") == n0 + 2);
    CHECK(m.add_string("plain two") == n0 + 3);
    HBCFile g = reparse(m.serialize());

    // Last run was Identifier: +String, +Identifier(2), +String = 3 new runs.
    CHECK(g.header().stringKindCount == f.header().stringKindCount + 3);
    CHECK(g.header().identifierCount == f.header().identifierCount + 2);
    CHECK(g.stringKinds()[n0]     == StringKind::String);
    CHECK(g.stringKinds()[n0 + 1] == StringKind::Identifier);
    CHECK(g.stringKinds()[n0 + 2] == StringKind::Identifier);
    CHECK(g.stringKinds()[n0 + 3] == StringKind::String);

    auto expected = hashes0;
    expected.push_back(hashOf("idOne"));
    expected.push_back(hashOf("idTwo"));
    CHECK(g.identifier_hashes() == expected);
    CHECK(recomputedHashes(g) == expected);
}

TEST_CASE("add_string alone leaves the hash table unchanged", "[identifiers]") {
    HBCFile f = testutil::openOrSkip(CORPUS + "/identifiers_g0.hbc");
    Module m = Module::from(f);
    m.add_string("just a string");
    HBCFile g = reparse(m.serialize());
    CHECK(g.header().identifierCount == f.header().identifierCount);
    CHECK(g.identifier_hashes() == f.identifier_hashes());
}

TEST_CASE("add_identifier on a real bundle with two Identifier runs", "[identifiers]") {
    HBCFile f = testutil::openOrSkip(CORPUS + "/hermitvault.hbc");
    const uint32_t n0 = f.header().stringCount;
    Module m = Module::from(f);
    CHECK(m.add_identifier("hermitPatched") == n0);
    HBCFile g = reparse(m.serialize());
    CHECK(g.strings()[n0] == "hermitPatched");
    CHECK(g.header().identifierCount == f.header().identifierCount + 1);
    CHECK(recomputedHashes(g) == g.identifier_hashes());
}

TEST_CASE("add_identifier refuses empty and non-ASCII values", "[identifiers]") {
    HBCFile f = testutil::openOrSkip(CORPUS + "/identifiers_g0.hbc");
    Module m = Module::from(f);
    CHECK_THROWS_AS(m.add_identifier(""), std::invalid_argument);
    CHECK_THROWS_AS(m.add_identifier("caf\xC3\xA9"), std::invalid_argument);
}

// --- rename (set_string_value on an Identifier entry) ----------------------

namespace {

// Index of the first Identifier entry after a String entry (start of a second
// Identifier run), or -1.
int64_t firstIdentifierOfSecondRun(const HBCFile& f) {
    const auto& kinds = f.stringKinds();
    bool seenId = false, seenStrAfter = false;
    for (size_t i = 0; i < kinds.size(); ++i) {
        if (kinds[i] == StringKind::Identifier) {
            if (seenStrAfter) return static_cast<int64_t>(i);
            seenId = true;
        } else if (seenId) {
            seenStrAfter = true;
        }
    }
    return -1;
}

uint32_t ordinalOf(const HBCFile& f, uint32_t idx) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < idx; ++i)
        if (f.stringKinds()[i] == StringKind::Identifier) ++n;
    return n;
}

void checkRename(const HBCFile& f, uint32_t idx, const std::string& newBytes, bool isUTF16) {
    REQUIRE(f.stringKinds()[idx] == StringKind::Identifier);
    const auto hashes0 = f.identifier_hashes();
    Module m = Module::from(f);
    m.set_string_value(idx, newBytes);
    HBCFile g = reparse(m.serialize());

    CHECK(g.header().identifierCount == f.header().identifierCount);
    CHECK(g.header().stringKindCount == f.header().stringKindCount);
    CHECK(g.stringKinds()[idx] == StringKind::Identifier);

    auto expected = hashes0;
    expected[ordinalOf(f, idx)] = identifier_hash(
        reinterpret_cast<const uint8_t*>(newBytes.data()), newBytes.size(), isUTF16);
    CHECK(g.identifier_hashes() == expected);
    CHECK(recomputedHashes(g) == expected);
}

}  // namespace

TEST_CASE("rename an Identifier: hash rewritten at its ordinal", "[identifiers]") {
    HBCFile f = testutil::openOrSkip(CORPUS + "/identifiers_g0.hbc");
    const auto& s = f.strings();
    const uint32_t alpha = static_cast<uint32_t>(
        std::find(s.begin(), s.end(), "alpha") - s.begin());
    checkRename(f, alpha, "zeta", false);
    checkRename(f, alpha, "a_much_longer_name", false);
}

TEST_CASE("rename an Identifier of the second run (real bundle)", "[identifiers]") {
    HBCFile f = testutil::openOrSkip(CORPUS + "/hermitvault.hbc");
    const int64_t idx = firstIdentifierOfSecondRun(f);
    REQUIRE(idx >= 0);
    REQUIRE(f.stringKinds()[idx - 1] == StringKind::String);
    checkRename(f, static_cast<uint32_t>(idx), "renamedProp", false);
    // Last Identifier of the table.
    uint32_t last = 0;
    for (uint32_t i = 0; i < f.stringKinds().size(); ++i)
        if (f.stringKinds()[i] == StringKind::Identifier) last = i;
    checkRename(f, last, "lastOne", false);
}

TEST_CASE("rename a UTF-16 Identifier (real bundle)", "[identifiers]") {
    HBCFile f = testutil::openOrSkip(CORPUS + "/ridemovi.hbc");
    const auto& entries = f.string_entries();
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < entries.size(); ++i)
        if (entries[i].kind == StringKind::Identifier && entries[i].isUTF16 &&
            !entries[i].isOverflow) { idx = i; break; }
    REQUIRE(idx != UINT32_MAX);
    // "è_x" in UTF-16LE.
    const std::string u16("\xE8\x00_\x00x\x00", 6);
    checkRename(f, idx, u16, true);
}

TEST_CASE("rename a String entry leaves the hash table unchanged", "[identifiers]") {
    HBCFile f = testutil::openOrSkip(CORPUS + "/identifiers_g0.hbc");
    const auto& s = f.strings();
    const uint32_t beta = static_cast<uint32_t>(
        std::find(s.begin(), s.end(), "beta value") - s.begin());
    REQUIRE(f.stringKinds()[beta] == StringKind::String);
    Module m = Module::from(f);
    m.set_string_value(beta, "other value");
    HBCFile g = reparse(m.serialize());
    CHECK(g.identifier_hashes() == f.identifier_hashes());
}
