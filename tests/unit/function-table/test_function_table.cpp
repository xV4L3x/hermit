// =============================================================================
// test_function_table.cpp - Catch2 tests for the function table parser
// =============================================================================
// Tests parseFunctionTable() (via HBCFile::functions()) and
// parseStringKinds() (via HBCFile::stringKinds()) on hello.hbc.
//
// hello.hbc is compiled from seeder/samples/hello.ts, which contains:
//   function greet(message: string) { console.log("Hello, " + message); }
//   greet("world");
//
// Expected structure in hello.hbc:
//   - 2 functions: the global function (index = globalCodeIndex) and "greet"
//   - 8 strings: "!", "Hello, ", "global", "hermit", "use strict",
//                 "log", "console", "greet"
//   - String kinds: "log", "console", "greet" -> Identifier; "!" -> String
// =============================================================================

// Catch2 v3: CATCH_CONFIG_MAIN is not needed with Catch2WithMain.
// The `Catch2::Catch2WithMain` CMake target provides main() automatically.
#include <catch2/catch_test_macros.hpp>

#include "hbc/hbc.h"   // HBCFile, FunctionInfo, StringKind

#include <algorithm>   // std::find_if
#include <string>

// HBC_CORPUS_DIR is injected by CMake as a preprocessor define:
//   target_compile_definitions(hermit_tests PRIVATE HBC_CORPUS_DIR="...")
// Converting it to std::string lets us use operator+ for paths.
static const std::string HELLO       = std::string(HBC_CORPUS_DIR) + "/hello.hbc";
static const std::string SHOW_SOURCE = std::string(HBC_CORPUS_DIR) + "/show_source.hbc";

static const hbc::FunctionInfo& findFunc(const hbc::HBCFile& f, const std::string& name) {
    auto it = std::find_if(
        f.functions().begin(), f.functions().end(),
        [&](const hbc::FunctionInfo& fi) { return fi.name == name; }
    );
    REQUIRE(it != f.functions().end());
    return *it;
}

// =============================================================================
// Test 1: the function count matches the header
// =============================================================================
TEST_CASE("function count matches header.functionCount", "[function-table][functions]") {
    hbc::HBCFile f = hbc::HBCFile::open(HELLO);
    // functions() must return exactly functionCount elements.
    // If the parser's loop stops too early or too late, this fails.
    REQUIRE(f.functions().size() == f.header().functionCount);
}

// =============================================================================
// Test 2: the global function exists and has a valid offset
// =============================================================================
TEST_CASE("global function exists and has valid offset", "[function-table][functions]") {
    hbc::HBCFile f = hbc::HBCFile::open(HELLO);
    const auto& funcs = f.functions();
    const uint32_t gi = f.header().globalCodeIndex;

    // globalCodeIndex must be a valid index into the vector.
    REQUIRE(gi < funcs.size());

    // The global function's offset must be > 0: the bytecode can't
    // start at offset 0 (that's where the file header is).
    REQUIRE(funcs[gi].offset > 0);

    // The global function has index == globalCodeIndex (sanity check field).
    REQUIRE(funcs[gi].index == gi);
}

// =============================================================================
// Test 3: no function has name == "" (anonymous ones use "anonymous_func_N")
// =============================================================================
TEST_CASE("no function has empty name", "[function-table][functions]") {
    hbc::HBCFile f = hbc::HBCFile::open(HELLO);

    // std::find_if looks for the first element satisfying the predicate.
    // If none satisfies it, it returns end() - which is what we want.
    auto it = std::find_if(
        f.functions().begin(), f.functions().end(),
        [](const hbc::FunctionInfo& fi) { return fi.name.empty(); }
    );
    // Check that no empty name was found.
    REQUIRE(it == f.functions().end());
}

// =============================================================================
// Test 4: hello.hbc contains the function "greet"
// =============================================================================
TEST_CASE("hello.hbc contains function named greet", "[function-table][functions]") {
    hbc::HBCFile f = hbc::HBCFile::open(HELLO);

    auto it = std::find_if(
        f.functions().begin(), f.functions().end(),
        [](const hbc::FunctionInfo& fi) { return fi.name == "greet"; }
    );
    REQUIRE(it != f.functions().end());

    // FINDING: paramCount includes the implicit `this` parameter.
    // greet(message) has 1 JS parameter, but Hermes also counts `this`,
    // so paramCount == 2. To compute the number of user-visible JS
    // parameters: paramCount - 1.
    REQUIRE(it->paramCount == 2);
}

// =============================================================================
// Test 5: functions() is idempotent (the cache works)
// =============================================================================
TEST_CASE("functions() is idempotent (cache)", "[function-table][functions]") {
    hbc::HBCFile f = hbc::HBCFile::open(HELLO);

    // Consecutive calls must return the same object in memory.
    // &a == &b means both const& point to the same vector -
    // the parser didn't re-run parsing, it returned the cache.
    const auto& a = f.functions();
    const auto& b = f.functions();
    REQUIRE(&a == &b);
}

// =============================================================================
// Test 6: stringKinds() has the same size as strings() (parallel vectors)
// =============================================================================
TEST_CASE("stringKinds().size() == strings().size()", "[function-table][stringkinds]") {
    hbc::HBCFile f = hbc::HBCFile::open(HELLO);
    // The two vectors must be parallel: kinds[i] describes strings()[i].
    REQUIRE(f.stringKinds().size() == f.strings().size());
}

// =============================================================================
// Test 7: in hello.hbc, "log", "console", "greet" are Identifier; "!" is String
// =============================================================================
TEST_CASE("string kinds match expected values in hello.hbc", "[function-table][stringkinds]") {
    hbc::HBCFile f = hbc::HBCFile::open(HELLO);
    const auto& strs  = f.strings();
    const auto& kinds = f.stringKinds();

    // Helper lambda: looks up string s and returns its kind.
    // Throws std::logic_error if the string isn't in the bundle - this
    // would fail the test with a clear message.
    auto kind_of = [&](const std::string& s) -> hbc::StringKind {
        for (size_t i = 0; i < strs.size(); ++i) {
            if (strs[i] == s) return kinds[i];
        }
        throw std::logic_error("String not found in bundle: " + s);
    };

    // "log", "console", "greet" are property names -> Identifier.
    REQUIRE(kind_of("log")     == hbc::StringKind::Identifier);
    REQUIRE(kind_of("console") == hbc::StringKind::Identifier);
    REQUIRE(kind_of("greet")   == hbc::StringKind::Identifier);

    // "!" is a runtime string literal -> String.
    REQUIRE(kind_of("!")       == hbc::StringKind::String);
}

// =============================================================================
// Test 8: stringKinds() is idempotent (the cache works)
// =============================================================================
TEST_CASE("stringKinds() is idempotent (cache)", "[function-table][stringkinds]") {
    hbc::HBCFile f = hbc::HBCFile::open(HELLO);
    const auto& a = f.stringKinds();
    const auto& b = f.stringKinds();
    REQUIRE(&a == &b);
}

// =============================================================================
// Function Source Table (header 0x64 functionSourceCount) - addendum found
// while writing thesis chapter 5.6. show_source.hbc is compiled from
// seeder/samples/show_source.js, which exercises Hermes' three
// SourceVisibility directives (opt-in, first statement of the function body,
// same mechanism as "use strict" - see Sema/SemanticResolver.cpp in the
// Hermes source):
//   withComment() { "show source"; ... }   -> real source preserved verbatim
//   arrowShow     = () => { "show source"; ... }  -> same, arrow function
//   hidden()      { "hide source"; ... }   -> entry present, but EMPTY (marker)
//   sensitiveFn() { "sensitive"; ... }     -> same as hide source
//   normal()      { /* no directive */ }   -> NO entry at all (not even empty)
// Verified against `hermes -dump-bytecode` before writing this parser.
// =============================================================================

TEST_CASE("show source: real source is preserved verbatim, directive included", "[function-table][functionsource]") {
    hbc::HBCFile f = hbc::HBCFile::open(SHOW_SOURCE);
    const auto& fi = findFunc(f, "withComment");

    REQUIRE(fi.hasPreservedSource);
    REQUIRE_FALSE(fi.sourceIsHidden);
    // Byte-exact: the directive itself and the comment are part of the
    // preserved text, not stripped - this is NOT a re-serialized AST.
    REQUIRE(fi.preservedSource.find("function withComment()") != std::string::npos);
    REQUIRE(fi.preservedSource.find("\"show source\";") != std::string::npos);
    REQUIRE(fi.preservedSource.find("// a comment inside the function") != std::string::npos);
    REQUIRE(fi.preservedSource.find("return x + 1;") != std::string::npos);
}

TEST_CASE("show source: works on an arrow function too, range excludes the `var` wrapper", "[function-table][functionsource]") {
    hbc::HBCFile f = hbc::HBCFile::open(SHOW_SOURCE);
    const auto& fi = findFunc(f, "arrowShow");

    REQUIRE(fi.hasPreservedSource);
    REQUIRE_FALSE(fi.sourceIsHidden);
    // Starts at the function expression itself, not at "var arrowShow = ".
    REQUIRE(fi.preservedSource.rfind("() => {", 0) == 0);
    REQUIRE(fi.preservedSource.find("return 42;") != std::string::npos);
}

TEST_CASE("hide source / sensitive: entry present but resolves to an empty marker", "[function-table][functionsource]") {
    hbc::HBCFile f = hbc::HBCFile::open(SHOW_SOURCE);

    for (const char* name : {"hidden", "sensitiveFn"}) {
        const auto& fi = findFunc(f, name);
        REQUIRE(fi.hasPreservedSource);
        REQUIRE(fi.sourceIsHidden);
        REQUIRE(fi.preservedSource.empty());
    }
}

TEST_CASE("no directive: no entry at all in the Function Source Table", "[function-table][functionsource]") {
    hbc::HBCFile f = hbc::HBCFile::open(SHOW_SOURCE);
    const auto& fi = findFunc(f, "normal");
    // Not just "empty" like hide source/sensitive - genuinely absent.
    REQUIRE_FALSE(fi.hasPreservedSource);
    REQUIRE_FALSE(fi.sourceIsHidden);
    REQUIRE(fi.preservedSource.empty());
}

TEST_CASE("global function has no Function Source Table entry", "[function-table][functionsource]") {
    // Sanity check: functionSourceCount in show_source.hbc covers exactly the
    // 4 functions with a directive (withComment, hidden, sensitiveFn,
    // arrowShow) - the implicit global wrapper isn't one of them.
    hbc::HBCFile f = hbc::HBCFile::open(SHOW_SOURCE);
    const auto& funcs = f.functions();
    uint32_t withEntry = 0;
    for (const auto& fi : funcs) if (fi.hasPreservedSource) withEntry++;
    REQUIRE(withEntry == f.header().functionSourceCount);
    REQUIRE(withEntry == 4);
}
