// =============================================================================
// test_encoder.cpp - Catch2 tests for the instruction encoder
// =============================================================================
// hbc::encode_instruction() is the inverse of the disassembler's decodeOperand().
// Strong guarantee: for every "clean" function in the corpus,
//   encode_instruction(version, ins.opcode, values) == the instruction's raw bytes
// The file paths are injected by CMake via HBC_CORPUS_DIR.
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "hbc/hbc.h"
#include "test_util.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using hbc::Operand;
using testutil::opValue;

// Round-trip over every eligible function in a file: no "???" opcode and
// no SwitchImm (which has an appended jump table not represented by Instruction).
static void checkRoundTrip(const char* path) {
    auto f = hbc::HBCFile::open(path);
    const uint8_t* base = f.raw_data();
    const uint32_t version = f.version();
    const auto& funcs = f.functions();

    size_t checkedFns = 0, checkedInsns = 0;
    for (uint32_t idx = 0; idx < f.header().functionCount; ++idx) {
        const auto& fi = funcs[idx];
        auto instrs = f.disasm(idx);
        if (instrs.empty()) continue;

        bool clean = true;
        for (const auto& in : instrs) {
            if (in.mnemonic == "???" || in.mnemonic == "SwitchImm") { clean = false; break; }
        }
        if (!clean) continue;

        for (size_t i = 0; i < instrs.size(); ++i) {
            const auto& in = instrs[i];

            std::vector<double> vals;
            vals.reserve(in.operands.size());
            for (const auto& op : in.operands) vals.push_back(opValue(op));

            std::vector<uint8_t> got = hbc::encode_instruction(version, in.opcode, vals);

            INFO(path << " func " << idx << " @0x" << std::hex << in.offset
                      << " " << in.mnemonic);

            // The produced bytes must match the instruction's original ones.
            const uint8_t* p = base + fi.offset + in.offset;
            std::vector<uint8_t> expected(p, p + got.size());
            REQUIRE(got == expected);

            // For every instruction except the last we know the next one's
            // offset: the encoded length must match it exactly.
            if (i + 1 < instrs.size())
                REQUIRE(got.size() == instrs[i + 1].offset - in.offset);

            ++checkedInsns;
        }
        ++checkedFns;
    }
    INFO("functions checked: " << checkedFns << " · instructions: " << checkedInsns);
    REQUIRE(checkedFns > 0);
    REQUIRE(checkedInsns > 0);
}

TEST_CASE("encoder round-trips hello.hbc", "[patcher]") {
    checkRoundTrip(HBC_CORPUS_DIR "/hello.hbc");
}

TEST_CASE("encoder round-trips control_flow.hbc", "[patcher]") {
    checkRoundTrip(HBC_CORPUS_DIR "/control_flow.hbc");
}

TEST_CASE("encoder round-trips medium.hbc", "[patcher]") {
    checkRoundTrip(HBC_CORPUS_DIR "/medium.hbc");
}

// A backward jump (negative Addr8) must re-encode into a single signed
// byte, identical to the original.
TEST_CASE("encoder handles backward Addr8 jump", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/control_flow.hbc");
    const uint32_t version = f.version();

    bool found = false;
    for (uint32_t idx = 0; idx < f.header().functionCount && !found; ++idx) {
        for (const auto& in : f.disasm(idx)) {
            int slot = -1;
            for (size_t j = 0; j < in.operands.size(); ++j)
                if (in.operands[j].type == Operand::Type::Addr8 && in.operands[j].i32 < 0)
                    slot = static_cast<int>(j);
            if (slot < 0) continue;

            std::vector<double> vals;
            for (const auto& op : in.operands) vals.push_back(opValue(op));
            auto got = hbc::encode_instruction(version, in.opcode, vals);

            // byte at the Addr8 operand's position: 1 + the offset of the preceding ones
            uint32_t at = 1;
            for (int j = 0; j < slot; ++j) at += hbc::operand_size(in.operands[j].type);
            REQUIRE(static_cast<int8_t>(got[at]) == in.operands[slot].i32);
            found = true;
            break;
        }
    }
    if (!found) WARN("no negative Addr8 operand in the corpus");
    SUCCEED();
}

TEST_CASE("encoder rejects wrong operand count", "[patcher]") {
    auto table = hbc::opcode_table(96);
    REQUIRE_FALSE(table.empty());

    const hbc::OpcodeDesc* one = nullptr;
    for (const auto& d : table)
        if (d.operands.size() == 1) { one = &d; break; }
    REQUIRE(one != nullptr);

    REQUIRE_THROWS_AS(hbc::encode_instruction(96, one->opcode, {}), std::invalid_argument);
}

TEST_CASE("encoder rejects unsupported version", "[patcher]") {
    REQUIRE_THROWS_AS(hbc::encode_instruction(999, 0, {}), std::invalid_argument);
}

TEST_CASE("encoder rejects out-of-range operand", "[patcher]") {
    auto table = hbc::opcode_table(96);
    const hbc::OpcodeDesc* reg = nullptr;
    for (const auto& d : table)
        if (d.operands.size() == 1 && d.operands[0] == Operand::Type::Reg8) { reg = &d; break; }
    REQUIRE(reg != nullptr);

    REQUIRE_THROWS_AS(hbc::encode_instruction(96, reg->opcode, { 999.0 }), std::range_error);
}

// --- HBCFile::from_bytes (re-parsing a patched image) ---------

TEST_CASE("from_bytes matches open", "[patcher]") {
    auto a = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    std::string raw(reinterpret_cast<const char*>(a.raw_data()), a.raw_size());

    auto b = hbc::HBCFile::from_bytes(raw);
    REQUIRE(b.version() == a.version());
    REQUIRE(b.header().fileLength == a.header().fileLength);
    REQUIRE(b.functions().size() == a.functions().size());
    REQUIRE(b.strings() == a.strings());
    REQUIRE(b.serialize() == a.serialize());

    // handles a second round: re-parsing its own serialized bytes
    auto s = b.serialize();
    auto c = hbc::HBCFile::from_bytes(
        std::string(reinterpret_cast<const char*>(s.data()), s.size()));
    REQUIRE(c.serialize() == a.serialize());
}

TEST_CASE("from_bytes rejects garbage", "[patcher]") {
    REQUIRE_THROWS(hbc::HBCFile::from_bytes(std::string(200, '\0')));   // wrong magic
    REQUIRE_THROWS(hbc::HBCFile::from_bytes("short"));                  // too short
}
