// =============================================================================
// test_xrefs.cpp - Unit tests for String XREFs + Call Graph + Modules
// =============================================================================
// Ground truth derived from disassembling hello.hbc:
//
//   func 0 (global):
//     0x0000  DeclareGlobalVar  "greet"      [identifier]
//     0x000E  PutById           ..., "greet" [identifier]
//     0x0014  GetByIdShort      ..., "greet" [identifier]
//     0x001B  LoadConstString   "hermit"     [string]
//     0x0024  LoadConstString   "use strict" [string]
//
//   func 1 (greet):
//     0x0000  LoadConstString  "Hello, "  [string]
//     0x000B  LoadConstString  "!"        [string]
//     0x0015  TryGetById       "console"  [identifier]
//     0x001B  GetByIdShort     "log"      [identifier]
//
//   Total: 7 referenced strings, 9 sites.
// =============================================================================
#include <catch2/catch_test_macros.hpp>
#include "hbc/hbc.h"
#include <limits>
#include <set>
#include <string>

static const std::string HELLO        = std::string(HBC_CORPUS_DIR) + "/hello.hbc";
static const std::string CTRL_FLOW    = std::string(HBC_CORPUS_DIR) + "/control_flow.hbc";
static const std::string FUNCTIONS    = std::string(HBC_CORPUS_DIR) + "/functions.hbc";

static const hbc::StringXRef* findXRef(
    const std::vector<hbc::StringXRef>& xs, const std::string& val)
{
    for (const auto& x : xs)
        if (x.stringValue == val) return &x;
    return nullptr;
}

static bool hasSite(const hbc::StringXRef& xr, uint32_t funcIdx, uint32_t offset)
{
    for (const auto& s : xr.sites)
        if (s.functionIndex == funcIdx && s.instructionOffset == offset)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Test 1 - Non-empty result
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: hello.hbc returns non-empty result", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto xs = f.string_xrefs();
    REQUIRE_FALSE(xs.empty());
}

// ---------------------------------------------------------------------------
// Test 2 - Every entry has at least one site (correct filtering)
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: every returned entry has at least one site", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto xs = f.string_xrefs();
    for (const auto& x : xs) {
        INFO("string [" << x.stringIndex << "] = " << x.stringValue);
        CHECK_FALSE(x.sites.empty());
    }
}

// ---------------------------------------------------------------------------
// Test 3 - "Hello, " -> 1 site in func 1 @ 0x0000, mnemonic LoadConstString
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: 'Hello, ' has one site in greet at LoadConstString", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto xs = f.string_xrefs();
    const auto* xr = findXRef(xs, "Hello, ");
    REQUIRE(xr != nullptr);
    CHECK(xr->sites.size() == 1u);
    CHECK(xr->sites[0].functionIndex     == 1u);
    CHECK(xr->sites[0].instructionOffset == 0x0000u);
    CHECK(xr->sites[0].mnemonic          == "LoadConstString");
}

// ---------------------------------------------------------------------------
// Test 4 - "!" -> 1 site in func 1 @ 0x000B, mnemonic LoadConstString
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: '!' has one site in greet at 0x000B", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto xs = f.string_xrefs();
    const auto* xr = findXRef(xs, "!");
    REQUIRE(xr != nullptr);
    CHECK(xr->sites.size() == 1u);
    CHECK(xr->sites[0].functionIndex     == 1u);
    CHECK(xr->sites[0].instructionOffset == 0x000Bu);
    CHECK(xr->sites[0].mnemonic          == "LoadConstString");
}

// ---------------------------------------------------------------------------
// Test 5 - "greet" -> 3 sites, all in func 0
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: 'greet' has three sites all in global function", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto xs = f.string_xrefs();
    const auto* xr = findXRef(xs, "greet");
    REQUIRE(xr != nullptr);
    CHECK(xr->sites.size() == 3u);
    for (const auto& s : xr->sites)
        CHECK(s.functionIndex == 0u);
    CHECK(hasSite(*xr, 0u, 0x0000u));
    CHECK(hasSite(*xr, 0u, 0x000Eu));
    CHECK(hasSite(*xr, 0u, 0x0014u));
}

// ---------------------------------------------------------------------------
// Test 6 - "global" is NOT referenced (function name, not a used string)
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: unreferenced string 'global' is absent from result", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto xs = f.string_xrefs();
    CHECK(findXRef(xs, "global") == nullptr);
}

// ---------------------------------------------------------------------------
// Test 7 - "console" and "log" are referenced
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: identifier strings console and log have sites", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto xs = f.string_xrefs();
    CHECK(findXRef(xs, "console") != nullptr);
    CHECK(findXRef(xs, "log")     != nullptr);
}

// ---------------------------------------------------------------------------
// Test 8 - "hermit" and "use strict" are referenced in func 0
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: 'hermit' and 'use strict' referenced in global", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto xs = f.string_xrefs();
    const auto* hermit = findXRef(xs, "hermit");
    REQUIRE(hermit != nullptr);
    CHECK(hermit->sites.size() == 1u);
    CHECK(hermit->sites[0].functionIndex     == 0u);
    CHECK(hermit->sites[0].instructionOffset == 0x001Bu);
    CHECK(hermit->sites[0].mnemonic          == "LoadConstString");

    const auto* us = findXRef(xs, "use strict");
    REQUIRE(us != nullptr);
    CHECK(us->sites.size() == 1u);
    CHECK(us->sites[0].functionIndex     == 0u);
    CHECK(us->sites[0].instructionOffset == 0x0024u);
}

// ---------------------------------------------------------------------------
// Test 9 - Total sites = 9
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: total site count is 9 for hello.hbc", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto xs = f.string_xrefs();
    uint32_t total = 0;
    for (const auto& x : xs) total += static_cast<uint32_t>(x.sites.size());
    CHECK(total == 9u);
}

// ---------------------------------------------------------------------------
// Test 10 - functionIndex and instructionOffset in bounds
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: all sites have valid function indices and offsets", "[xrefs]") {
    auto f     = hbc::HBCFile::open(HELLO);
    auto xs    = f.string_xrefs();
    const auto& funcs = f.functions();
    for (const auto& x : xs) {
        for (const auto& s : x.sites) {
            INFO("string=" << x.stringValue << " func=" << s.functionIndex << " off=" << s.instructionOffset);
            REQUIRE(s.functionIndex < funcs.size());
            CHECK(s.instructionOffset < funcs[s.functionIndex].bytecodeSizeInBytes);
            CHECK_FALSE(s.mnemonic.empty());
        }
    }
}

// ---------------------------------------------------------------------------
// Test 11 - Determinism: two calls give identical results
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: result is deterministic", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto a  = f.string_xrefs();
    auto b  = f.string_xrefs();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].stringIndex      == b[i].stringIndex);
        CHECK(a[i].stringValue      == b[i].stringValue);
        CHECK(a[i].sites.size()     == b[i].sites.size());
    }
}

// ---------------------------------------------------------------------------
// Test 12 - All stringIndex values are < strings().size()
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: all stringIndex values are within strings() range", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto xs = f.string_xrefs();
    size_t n = f.strings().size();
    for (const auto& x : xs) {
        INFO("stringIndex=" << x.stringIndex);
        CHECK(x.stringIndex < n);
    }
}

// ---------------------------------------------------------------------------
// Test 13 - stringValue matches strings()[stringIndex]
// ---------------------------------------------------------------------------
TEST_CASE("string_xrefs: stringValue matches strings()[stringIndex]", "[xrefs]") {
    auto f       = hbc::HBCFile::open(HELLO);
    auto xs      = f.string_xrefs();
    const auto& strs = f.strings();
    for (const auto& x : xs) {
        INFO("stringIndex=" << x.stringIndex);
        CHECK(x.stringValue == strs[x.stringIndex]);
    }
}

// =============================================================================
// Tests 14–22: call_graph() + recover_modules()
// =============================================================================

// ---------------------------------------------------------------------------
// Test 14 - call_graph() is not empty on hello.hbc (has 1 CreateClosure)
// ---------------------------------------------------------------------------
TEST_CASE("call_graph: non-empty on hello.hbc", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto cg = f.call_graph();
    CHECK_FALSE(cg.empty());
}

// ---------------------------------------------------------------------------
// Test 15 - hello.hbc: exactly 1 resolved site, caller=0, callee=1 (greet)
// ---------------------------------------------------------------------------
TEST_CASE("call_graph: hello.hbc has exactly one resolved site from global to greet", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto cg = f.call_graph();

    std::vector<hbc::CallSite> resolved;
    for (const auto& cs : cg) {
        if (cs.calleeIndex != std::numeric_limits<uint32_t>::max())
            resolved.push_back(cs);
    }
    REQUIRE(resolved.size() == 1);
    CHECK(resolved[0].callerIndex == 0);
    CHECK(resolved[0].calleeIndex == 1);
    CHECK(resolved[0].isDirect    == false);
    CHECK(resolved[0].mnemonic    == "CreateClosure");
}

// ---------------------------------------------------------------------------
// Test 16 - hello.hbc: at least 1 unresolved site (Call* in the functions)
// ---------------------------------------------------------------------------
TEST_CASE("call_graph: hello.hbc has at least one unresolved indirect call site", "[xrefs]") {
    auto f     = hbc::HBCFile::open(HELLO);
    auto cg    = f.call_graph();
    const auto UNRESOLVED = std::numeric_limits<uint32_t>::max();
    bool found = false;
    for (const auto& cs : cg) {
        if (cs.calleeIndex == UNRESOLVED) { found = true; break; }
    }
    CHECK(found);
}

// ---------------------------------------------------------------------------
// Test 17 - hello.hbc: no site has isDirect=true (corpus has no CallDirect)
// ---------------------------------------------------------------------------
TEST_CASE("call_graph: hello.hbc has no direct call sites (CallDirect)", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto cg = f.call_graph();
    for (const auto& cs : cg) {
        CHECK(cs.isDirect == false);
    }
}

// ---------------------------------------------------------------------------
// Test 18 - All callerIndex and resolved calleeIndex values are in bounds
// ---------------------------------------------------------------------------
TEST_CASE("call_graph: all callerIndex and resolved calleeIndex are in bounds", "[xrefs]") {
    auto f     = hbc::HBCFile::open(HELLO);
    auto cg    = f.call_graph();
    size_t n   = f.functions().size();
    const auto UNRESOLVED = std::numeric_limits<uint32_t>::max();
    for (const auto& cs : cg) {
        INFO("callerIndex=" << cs.callerIndex);
        CHECK(cs.callerIndex < n);
        if (cs.calleeIndex != UNRESOLVED) {
            INFO("calleeIndex=" << cs.calleeIndex);
            CHECK(cs.calleeIndex < n);
        }
    }
}

// ---------------------------------------------------------------------------
// Test 19 - control_flow.hbc: exactly 3 resolved sites, all caller=0, CreateClosure
// ---------------------------------------------------------------------------
TEST_CASE("call_graph: control_flow.hbc has 3 resolved CreateClosure from global", "[xrefs]") {
    auto f  = hbc::HBCFile::open(CTRL_FLOW);
    auto cg = f.call_graph();

    const auto UNRESOLVED = std::numeric_limits<uint32_t>::max();
    std::vector<hbc::CallSite> resolved;
    for (const auto& cs : cg) {
        if (cs.calleeIndex != UNRESOLVED) resolved.push_back(cs);
    }

    REQUIRE(resolved.size() == 3);
    for (const auto& cs : resolved) {
        CHECK(cs.callerIndex == 0);
        CHECK(cs.isDirect    == false);
        CHECK(cs.mnemonic    == "CreateClosure");
    }
    // callee indices 1,2,3 (all distinct)
    std::set<uint32_t> callees;
    for (const auto& cs : resolved) callees.insert(cs.calleeIndex);
    CHECK(callees.size() == 3);
}

// ---------------------------------------------------------------------------
// Test 20 - functions.hbc: exactly 5 resolved sites, all caller=0, CreateClosure
// ---------------------------------------------------------------------------
TEST_CASE("call_graph: functions.hbc has 5 resolved CreateClosure from global", "[xrefs]") {
    auto f  = hbc::HBCFile::open(FUNCTIONS);
    auto cg = f.call_graph();

    const auto UNRESOLVED = std::numeric_limits<uint32_t>::max();
    std::vector<hbc::CallSite> resolved;
    for (const auto& cs : cg) {
        if (cs.calleeIndex != UNRESOLVED) resolved.push_back(cs);
    }

    REQUIRE(resolved.size() == 5);
    for (const auto& cs : resolved) {
        CHECK(cs.callerIndex == 0);
        CHECK(cs.isDirect    == false);
        CHECK(cs.mnemonic    == "CreateClosure");
    }
    std::set<uint32_t> callees;
    for (const auto& cs : resolved) callees.insert(cs.calleeIndex);
    CHECK(callees.size() == 5);
}

// ---------------------------------------------------------------------------
// Test 21 - Determinism: two calls to call_graph() give the same result
// ---------------------------------------------------------------------------
TEST_CASE("call_graph: result is deterministic", "[xrefs]") {
    auto f  = hbc::HBCFile::open(HELLO);
    auto a  = f.call_graph();
    auto b  = f.call_graph();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].callerIndex == b[i].callerIndex);
        CHECK(a[i].calleeIndex == b[i].calleeIndex);
        CHECK(a[i].offset      == b[i].offset);
        CHECK(a[i].isDirect    == b[i].isDirect);
        CHECK(a[i].mnemonic    == b[i].mnemonic);
    }
}

// ---------------------------------------------------------------------------
// Test 22 - recover_modules() on hello.hbc returns an empty list (no Metro bundle)
// ---------------------------------------------------------------------------
TEST_CASE("recover_modules: returns empty on non-Metro file", "[xrefs]") {
    auto f   = hbc::HBCFile::open(HELLO);
    auto mods = f.recover_modules();
    CHECK(mods.empty());
}

// =============================================================================
// Literal buffer references (xrefs_literals.hbc, from seeder/samples/xrefs_literals.js)
// =============================================================================
// Every string below is referenced from an array/object literal buffer. Sites
// point at the New{Array,Object}WithBuffer instruction. func 0 is the global
// function, 1 = first(), 2 = second() (same array literal: one shared buffer).
static const std::string XREFS_LITERALS = std::string(HBC_CORPUS_DIR) + "/xrefs_literals.hbc";

static std::vector<hbc::XRefSite> sitesOfKind(const hbc::StringXRef& xr, hbc::XRefKind kind) {
    std::vector<hbc::XRefSite> out;
    for (const auto& s : xr.sites)
        if (s.kind == kind) out.push_back(s);
    return out;
}

TEST_CASE("string_xrefs: strings only inside an array literal are found", "[xrefs][literal-xrefs]") {
    auto f  = hbc::HBCFile::open(XREFS_LITERALS);
    auto xs = f.string_xrefs();

    // The 3-string PoC array ["loca", "lho", "st"]: no operand references them.
    const char* parts[] = {"loca", "lho", "st"};
    for (uint32_t i = 0; i < 3; ++i) {
        const auto* xr = findXRef(xs, parts[i]);
        INFO(parts[i]);
        REQUIRE(xr != nullptr);
        REQUIRE(xr->sites.size() == 1);
        const auto& s = xr->sites[0];
        CHECK(s.kind == hbc::XRefKind::ArrayElement);
        CHECK(s.functionIndex == 0);
        CHECK(s.mnemonic == "NewArrayWithBuffer");
        CHECK(s.elementIndex == i);
        CHECK(s.elementCount == 1);
    }
}

TEST_CASE("string_xrefs: a string repeated in one literal is one site with its element count",
          "[xrefs][literal-xrefs]") {
    auto f  = hbc::HBCFile::open(XREFS_LITERALS);
    auto xs = f.string_xrefs();
    const auto* dup = findXRef(xs, "dup");
    REQUIRE(dup != nullptr);
    auto lits = sitesOfKind(*dup, hbc::XRefKind::ArrayElement);
    REQUIRE(lits.size() == 1);
    CHECK(lits[0].elementIndex == 0);
    CHECK(lits[0].elementCount == 2);

    const auto* other = findXRef(xs, "other");
    REQUIRE(other != nullptr);
    REQUIRE(other->sites.size() == 1);
    CHECK(other->sites[0].elementIndex == 2);
    // Same instruction as "dup"'s literal site.
    CHECK(other->sites[0].instructionOffset == lits[0].instructionOffset);
}

TEST_CASE("string_xrefs: strings inside a mixed literal keep their element positions",
          "[xrefs][literal-xrefs]") {
    auto f  = hbc::HBCFile::open(XREFS_LITERALS);
    auto xs = f.string_xrefs();
    // ["mx1", 7, "mx2", true, null, "mx3"]
    const std::pair<const char*, uint32_t> expect[] = {{"mx1", 0}, {"mx2", 2}, {"mx3", 5}};
    for (const auto& [value, idx] : expect) {
        const auto* xr = findXRef(xs, value);
        INFO(value);
        REQUIRE(xr != nullptr);
        REQUIRE(xr->sites.size() == 1);
        CHECK(xr->sites[0].elementIndex == idx);
    }
}

TEST_CASE("string_xrefs: object literal keys and values are told apart", "[xrefs][literal-xrefs]") {
    auto f  = hbc::HBCFile::open(XREFS_LITERALS);
    auto xs = f.string_xrefs();
    // { kx: "vx", ky: "vy", kz: "vz" }
    const char* keys[] = {"kx", "ky", "kz"};
    const char* vals[] = {"vx", "vy", "vz"};
    for (uint32_t i = 0; i < 3; ++i) {
        const auto* k = findXRef(xs, keys[i]);
        const auto* v = findXRef(xs, vals[i]);
        INFO(keys[i]);
        REQUIRE(k != nullptr);
        REQUIRE(v != nullptr);
        auto ks = sitesOfKind(*k, hbc::XRefKind::ObjectKey);
        auto vs = sitesOfKind(*v, hbc::XRefKind::ObjectValue);
        REQUIRE(ks.size() == 1);
        REQUIRE(vs.size() == 1);
        CHECK(ks[0].mnemonic == "NewObjectWithBuffer");
        CHECK(ks[0].elementIndex == i);
        CHECK(vs[0].elementIndex == i);
        CHECK(ks[0].instructionOffset == vs[0].instructionOffset);
        // keys are not values and vice versa
        CHECK(sitesOfKind(*k, hbc::XRefKind::ObjectValue).empty());
        CHECK(sitesOfKind(*v, hbc::XRefKind::ObjectKey).empty());
    }
}

TEST_CASE("string_xrefs: identical literals in two functions give one site each",
          "[xrefs][literal-xrefs]") {
    auto f  = hbc::HBCFile::open(XREFS_LITERALS);
    auto xs = f.string_xrefs();
    // "alpha" is in first() and second() (one shared buffer) and nowhere else.
    const auto* alpha = findXRef(xs, "alpha");
    REQUIRE(alpha != nullptr);
    REQUIRE(alpha->sites.size() == 2);
    CHECK(alpha->sites[0].functionIndex == 1);
    CHECK(alpha->sites[1].functionIndex == 2);
    for (const auto& s : alpha->sites) {
        CHECK(s.kind == hbc::XRefKind::ArrayElement);
        CHECK(s.elementIndex == 0);
    }
}

TEST_CASE("string_xrefs: a string used as an operand and in a literal reports both",
          "[xrefs][literal-xrefs]") {
    auto f  = hbc::HBCFile::open(XREFS_LITERALS);
    auto xs = f.string_xrefs();
    // "beta": LoadConstString in the global function + element 1 of both arrays.
    const auto* beta = findXRef(xs, "beta");
    REQUIRE(beta != nullptr);
    auto ops  = sitesOfKind(*beta, hbc::XRefKind::Operand);
    auto lits = sitesOfKind(*beta, hbc::XRefKind::ArrayElement);
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].mnemonic == "LoadConstString");
    CHECK(ops[0].elementCount == 1);
    REQUIRE(lits.size() == 2);
    CHECK(lits[0].elementIndex == 1);
}

TEST_CASE("string_xrefs: a file without literal buffers reports only operand sites",
          "[xrefs][literal-xrefs]") {
    auto f = hbc::HBCFile::open(HELLO);
    for (const auto& x : f.string_xrefs())
        for (const auto& s : x.sites)
            CHECK(s.kind == hbc::XRefKind::Operand);
}
