// =============================================================================
// test_disassembler.cpp - Catch2 tests for the Disassembler
// =============================================================================
// All tests use hello.hbc (2 functions: global, greet) and
// control_flow.hbc (max, factorial with conditional jumps).
// The file paths are injected by CMake via HBC_CORPUS_DIR.
// =============================================================================

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "hbc/hbc.h"

#include <string>
#include <vector>

using namespace Catch::Matchers;

// ---------------------------------------------------------------------------
// Helper: finds the first instruction with a given mnemonic
// ---------------------------------------------------------------------------
static const hbc::Instruction* findInstr(
    const std::vector<hbc::Instruction>& instrs,
    const std::string& mnemonic)
{
    for (const auto& i : instrs)
        if (i.mnemonic == mnemonic) return &i;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Test 1: disasm(0) on hello.hbc is not empty
// ---------------------------------------------------------------------------
TEST_CASE("disasm returns non-empty for global function", "[disassembler]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto instrs = f.disasm(f.header().globalCodeIndex);
    REQUIRE(!instrs.empty());
}

// ---------------------------------------------------------------------------
// Test 2: first instruction at offset 0
// ---------------------------------------------------------------------------
TEST_CASE("first instruction has offset zero", "[disassembler]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto instrs = f.disasm(f.header().globalCodeIndex);
    REQUIRE(!instrs.empty());
    REQUIRE(instrs.front().offset == 0u);
}

// ---------------------------------------------------------------------------
// Test 3: no "???" instruction in hello.hbc (all opcodes known)
// ---------------------------------------------------------------------------
TEST_CASE("no unknown opcodes in hello.hbc", "[disassembler]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    for (uint32_t idx = 0; idx < f.header().functionCount; idx++) {
        auto instrs = f.disasm(idx);
        for (const auto& i : instrs) {
            INFO("Function " << idx << " offset 0x" << std::hex << i.offset
                 << ": opcode 0x" << (int)i.opcode);
            REQUIRE(i.mnemonic != "???");
        }
    }
}

// ---------------------------------------------------------------------------
// Test 4: at least one StringID operand resolves to "Hello, " or "!"
// ---------------------------------------------------------------------------
TEST_CASE("string operands resolve correctly", "[disassembler]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    // The greet function (index 1) uses LoadConstString with "Hello, " and "!"
    auto instrs = f.disasm(1);

    bool foundHello = false;
    bool foundBang  = false;
    for (const auto& i : instrs) {
        for (const auto& op : i.operands) {
            if (op.type == hbc::Operand::Type::StringID) {
                if (op.resolvedString.find("Hello, ") != std::string::npos) foundHello = true;
                if (op.resolvedString.find("!")       != std::string::npos) foundBang  = true;
            }
        }
    }
    REQUIRE(foundHello);
    REQUIRE(foundBang);
}

// ---------------------------------------------------------------------------
// Test 5: disasm is deterministic (two calls produce the same result)
// ---------------------------------------------------------------------------
TEST_CASE("disasm is deterministic", "[disassembler]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto a = f.disasm(1);
    auto b = f.disasm(1);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); i++) {
        REQUIRE(a[i].mnemonic == b[i].mnemonic);
        REQUIRE(a[i].offset   == b[i].offset);
        REQUIRE(a[i].operands.size() == b[i].operands.size());
    }
}

// ---------------------------------------------------------------------------
// Test 6: greet (index 1) contains at least one LoadParam
// ---------------------------------------------------------------------------
TEST_CASE("greet function contains LoadParam", "[disassembler]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    // greet function: paramCount == 2 (this + name). Must have LoadParam.
    auto instrs = f.disasm(1);
    REQUIRE(findInstr(instrs, "LoadParam") != nullptr);
}

// ---------------------------------------------------------------------------
// Test 7: greet ends with Ret
// ---------------------------------------------------------------------------
TEST_CASE("greet function ends with Ret", "[disassembler]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto instrs = f.disasm(1);
    REQUIRE(!instrs.empty());
    REQUIRE(instrs.back().mnemonic == "Ret");
}

// ---------------------------------------------------------------------------
// Test 8: control_flow.hbc - max() contains a conditional jump
// ---------------------------------------------------------------------------
TEST_CASE("control flow function contains conditional jump", "[disassembler]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/control_flow.hbc");
    // Search all functions (except the global one) for at least one conditional jump
    // (JmpTrue, JmpFalse, JLess, JGreater, etc. prefixed with J but not plain Jmp)
    bool foundCondJump = false;
    for (uint32_t idx = 0; idx < f.header().functionCount; idx++) {
        auto instrs = f.disasm(idx);
        for (const auto& i : instrs) {
            const std::string& m = i.mnemonic;
            // A conditional jump starts with "J" but isn't "Jmp" or "JmpLong"
            if (m.size() > 1 && m[0] == 'J' &&
                m != "Jmp" && m != "JmpLong") {
                foundCondJump = true;
                break;
            }
        }
        if (foundCondJump) break;
    }
    REQUIRE(foundCondJump);
}

// ---------------------------------------------------------------------------
// Test 9: isGenerator gets updated after disasm (stub -> real value)
// ---------------------------------------------------------------------------
TEST_CASE("isGenerator detection via opcode scan", "[disassembler]") {
    // hello.hbc has no generators - functions must have isGenerator=false
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    for (uint32_t idx = 0; idx < f.header().functionCount; idx++) {
        f.disasm(idx);  // trigger detection
        REQUIRE_FALSE(f.functions()[idx].isGenerator);
        REQUIRE_FALSE(f.functions()[idx].isAsync);
    }
}

// ---------------------------------------------------------------------------
// Test 10: out-of-range functionIndex throws an exception
// ---------------------------------------------------------------------------
TEST_CASE("disasm throws on invalid function index", "[disassembler]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    REQUIRE_THROWS_AS(f.disasm(999), std::out_of_range);
}
