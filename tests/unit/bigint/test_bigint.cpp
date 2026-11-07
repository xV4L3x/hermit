// =============================================================================
// test_bigint.cpp - BigInt Table decoding (HBCFile::bigints())
// =============================================================================
// An entry is little-endian two's complement with the redundant sign bytes
// dropped (include/hermes/Support/BigIntSupport.h): 0n is empty, 255n is `ff 00`,
// -1n is `ff`. bigint.hbc comes from seeder/samples/bigint.js, whose print()
// lines are the expected values, one per table entry in first-use order (a value
// used twice has one entry).
// =============================================================================
#include <catch2/catch_test_macros.hpp>

#include "hbc/hbc.h"
#include "parser/bigint_table.h"

#include <cstring>
#include <string>
#include <vector>

using hbc::bigIntBytesToDecimal;

static const std::string HELLO  = std::string(HBC_CORPUS_DIR) + "/hello.hbc";
static const std::string BIGINT = std::string(HBC_CORPUS_DIR) + "/bigint.hbc";

TEST_CASE("bigint: bytes to decimal, non-negative values", "[bigint]") {
    CHECK(bigIntBytesToDecimal({}) == "0");
    CHECK(bigIntBytesToDecimal({0x01}) == "1");
    CHECK(bigIntBytesToDecimal({0x7f}) == "127");
    CHECK(bigIntBytesToDecimal({0x80, 0x00}) == "128");       // sign byte kept
    CHECK(bigIntBytesToDecimal({0xff, 0x00}) == "255");
    CHECK(bigIntBytesToDecimal({0x00, 0x01}) == "256");
    CHECK(bigIntBytesToDecimal({0xff, 0xff, 0x00}) == "65535");
    // 2^64
    CHECK(bigIntBytesToDecimal({0, 0, 0, 0, 0, 0, 0, 0, 1}) == "18446744073709551616");
    // 10^9 and 10^18 straddle the 10^9 chunk boundary of the conversion
    CHECK(bigIntBytesToDecimal({0x00, 0xca, 0x9a, 0x3b}) == "1000000000");
    CHECK(bigIntBytesToDecimal({0x00, 0x00, 0x64, 0xa7, 0xb3, 0xb6, 0xe0, 0x0d}) == "1000000000000000000");
}

TEST_CASE("bigint: bytes to decimal, negative values", "[bigint]") {
    CHECK(bigIntBytesToDecimal({0xff}) == "-1");
    CHECK(bigIntBytesToDecimal({0x80}) == "-128");            // most negative of one byte
    CHECK(bigIntBytesToDecimal({0x7f, 0xff}) == "-129");
    CHECK(bigIntBytesToDecimal({0x00, 0xff}) == "-256");
    CHECK(bigIntBytesToDecimal({0x00, 0x80}) == "-32768");
    // -(2^64)
    CHECK(bigIntBytesToDecimal({0, 0, 0, 0, 0, 0, 0, 0, 0xff}) == "-18446744073709551616");
}

TEST_CASE("bigint: a table entry outside the storage is reported, the others are read",
          "[bigint]") {
    // table at 8 (offset 0 means "section missing"): 3 entries; storage at 40, 4 bytes:
    // 05 | 01 02 | entry 2 reaches past the end
    std::vector<uint8_t> f(48, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(&f[at], &v, 4); };
    put(8, 0);   put(12, 1);    // entry 0: [0, +1)  -> 05
    put(16, 1);  put(20, 2);    // entry 1: [1, +2)  -> 01 02
    put(24, 3);  put(28, 9);    // entry 2: [3, +9)  outside the 4-byte storage
    f[40] = 0x05; f[41] = 0x01; f[42] = 0x02; f[43] = 0x00;

    auto r = hbc::parseBigIntTable(f.data(), 8, 3, 40, 4);
    REQUIRE(r.size() == 3);
    CHECK(r[0].ok);  CHECK(r[0].decimal == "5");
    CHECK(r[1].ok);  CHECK(r[1].decimal == "513");   // 0x0201
    CHECK_FALSE(r[2].ok);
    CHECK_FALSE(r[2].error.empty());
    CHECK(r[2].decimal.empty());
}

TEST_CASE("bigint: offset + length overflow does not wrap", "[bigint]") {
    std::vector<uint8_t> f(32, 0);
    const uint32_t off = 0xFFFFFFFFu, len = 2;
    std::memcpy(&f[8], &off, 4);
    std::memcpy(&f[12], &len, 4);
    auto r = hbc::parseBigIntTable(f.data(), 8, 1, 16, 8);
    REQUIRE(r.size() == 1);
    CHECK_FALSE(r[0].ok);
}

TEST_CASE("bigint: a missing table section is reported", "[bigint]") {
    std::vector<uint8_t> f(16, 0);
    auto r = hbc::parseBigIntTable(f.data(), 0, 2, 0, 0);
    REQUIRE(r.size() == 2);
    CHECK_FALSE(r[0].ok);
    CHECK_FALSE(r[1].ok);
}

TEST_CASE("bigint: a file without BigInts has an empty table", "[bigint]") {
    auto f = hbc::HBCFile::open(HELLO);
    CHECK(f.header().bigIntCount == 0);
    CHECK(f.bigints().empty());
}

TEST_CASE("bigint: bigint.hbc entries equal the printed values", "[bigint]") {
    auto f = hbc::HBCFile::open(BIGINT);
    // distinct literals in source order (the 30-digit one is printed twice)
    const std::vector<std::string> expected = {
        "0", "1", "127", "128", "255", "256", "65535",
        "9223372036854775807", "9223372036854775808", "18446744073709551616",
        "123456789012345678901234567890",
        "79228162514264337593543950335",
        "1606938044258990275541962092341162602522202993782792835301376",
    };
    const auto& b = f.bigints();
    REQUIRE(f.header().bigIntCount == expected.size());
    REQUIRE(b.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        INFO("entry " << i);
        CHECK(b[i].ok);
        CHECK(b[i].index == i);
        CHECK(b[i].decimal == expected[i]);
        CHECK_FALSE(b[i].isNegative);
    }
    // 255n keeps its sign byte; 0n is empty
    CHECK(b[0].length == 0);
    CHECK(b[4].bytes == std::vector<uint8_t>{0xff, 0x00});
}

TEST_CASE("bigint: the decompiler prints the real value", "[bigint][decompiler]") {
    auto f = hbc::HBCFile::open(BIGINT);
    const std::string js = f.decompile(f.header().globalCodeIndex);
    CHECK(js.find("r1 = 127n;") != std::string::npos);
    CHECK(js.find("= 123456789012345678901234567890n;") != std::string::npos);
    CHECK(js.find("= 1606938044258990275541962092341162602522202993782792835301376n;")
          != std::string::npos);
    CHECK(js.find("not resolved") == std::string::npos);
}
