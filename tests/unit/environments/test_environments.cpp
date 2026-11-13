// =============================================================================
// test_environments.cpp - closure environment model: env_accesses(),
// environments(), captured_environment(), env_slot_uses()
// =============================================================================
// Ground truth: files compiled with debug info, where scope_variables(f)[slot]
// names slot `slot` of the environment function f creates. A resolved access
// (env.function, slot) must name the variable the source uses.
//
//   env_chain_g.hbc      seeder/samples/env_chain.js (-g): 3-level chain
//                        outer > middle > inner, and a generator whose closure
//                        is created after a yield (resume edge).
//   closure_scope_g.hbc  makeCounter / increment.
//   hermitvault[_g3].hbc same Metro JS compiled -g1 (shipped) and -g3: the
//                        resolution must be identical, and on -g3 the factory
//                        of module 0 (func 83) owns `_App`, read by func 84.
//                        hermitvault_g3.hbc is generated from the HermitVault
//                        build (`npm run build:hbc` writes build/main.js):
//     hermit-vault/node_modules/hermes-compiler-legacy/hermesc/osx-bin/hermesc \
//       -emit-binary -O -g3 -out hermit/seeder/corpus/hermitvault_g3.hbc \
//       hermit-vault/build/main.js
//   ridemovi.hbc         every access resolved (generator resume edges).
// =============================================================================
#include <catch2/catch_test_macros.hpp>

#include "hbc/hbc.h"
#include "test_util.h"

#include <string>
#include <tuple>
#include <vector>

namespace {

const std::string CORPUS = HBC_CORPUS_DIR;

// (isStore, variable name) of every access of `f`, names from the debug info.
std::vector<std::pair<bool, std::string>> namedAccesses(const hbc::HBCFile& file, uint32_t f) {
    std::vector<std::pair<bool, std::string>> out;
    for (const auto& a : file.env_accesses(f)) {
        REQUIRE(a.resolved);
        const auto vars = file.scope_variables(a.env.function);
        REQUIRE(a.slot < vars.size());
        out.emplace_back(a.isStore, vars[a.slot].name);
    }
    return out;
}

uint32_t funcNamed(const hbc::HBCFile& file, const std::string& name) {
    for (const auto& fi : file.functions())
        if (fi.name == name) return fi.index;
    FAIL("no function " << name);
    return 0;
}

using Named = std::vector<std::pair<bool, std::string>>;

} // namespace

TEST_CASE("environments: three-level chain resolves outer slots", "[environments]") {
    auto f = testutil::openOrSkip(CORPUS + "/env_chain_g.hbc");
    const uint32_t outer = funcNamed(f, "outer"), middle = funcNamed(f, "middle"),
                   inner = funcNamed(f, "inner");

    // inner reads x (2 levels up, outer) and y (1 level up, middle), writes x
    CHECK(namedAccesses(f, inner) == Named{{false, "x"}, {false, "y"}, {true, "x"}});
    for (const auto& a : f.env_accesses(inner))
        CHECK((a.env.function == outer || a.env.function == middle));

    // captured environments follow the creation tree
    auto envOf = [&](uint32_t fn) { return f.environments(fn).at(0).env; };
    REQUIRE(f.captured_environment(inner));
    CHECK(*f.captured_environment(inner) == envOf(middle));
    REQUIRE(f.captured_environment(middle));
    CHECK(*f.captured_environment(middle) == envOf(outer));

    // parent links: middle's environment -> outer's
    const auto me = f.environments(middle).at(0);
    CHECK(me.parentKnown);
    CHECK(me.hasParent);
    CHECK(me.parent == envOf(outer));
}

TEST_CASE("environments: closure created after a generator resume", "[environments]") {
    auto f = testutil::openOrSkip(CORPUS + "/env_chain_g.hbc");
    // The arrow function returned by the second `yield` reads g (generator
    // scope) and x (outer): its creation site is only reachable through the
    // SaveGenerator resume edge.
    uint32_t closure = UINT32_MAX;
    for (const auto& fi : f.functions()) {
        auto named = f.env_accesses(fi.index).size() == 2 ? namedAccesses(f, fi.index) : Named{};
        if (named == Named{{false, "g"}, {false, "x"}}) closure = fi.index;
    }
    REQUIRE(closure != UINT32_MAX);
    CHECK(f.captured_environment(closure).has_value());
}

TEST_CASE("environments: makeCounter / increment", "[environments]") {
    auto f = testutil::openOrSkip(CORPUS + "/closure_scope_g.hbc");
    const uint32_t mk = funcNamed(f, "makeCounter"), inc = funcNamed(f, "increment");
    CHECK(namedAccesses(f, inc) == Named{{false, "count"}, {true, "count"}});

    // env_slot_uses: makeCounter stores `count`, increment loads and stores it
    const auto env = f.environments(mk).at(0).env;
    const auto uses = f.env_slot_uses(env, 0);
    REQUIRE(uses.size() == 3);
    CHECK(uses[0].functionIndex == mk);
    CHECK(uses[0].isStore);
    CHECK(uses[1].functionIndex == inc);
    CHECK_FALSE(uses[1].isStore);
    CHECK(uses[2].functionIndex == inc);
    CHECK(uses[2].isStore);
}

TEST_CASE("environments: hermitvault -g1 and -g3 resolve identically", "[environments]") {
    auto g1 = testutil::openOrSkip(CORPUS + "/hermitvault.hbc");
    auto g3 = testutil::openOrSkip(CORPUS + "/hermitvault_g3.hbc");
    REQUIRE(g1.functions().size() == g3.functions().size());

    // Register allocation and offsets differ (-g3 adds AsyncBreakCheck), the
    // environment structure does not.
    auto key = [](const hbc::HBCFile& file, uint32_t fn) {
        std::vector<std::tuple<bool, uint32_t, bool, uint32_t>> k;
        for (const auto& a : file.env_accesses(fn))
            k.emplace_back(a.isStore, a.slot, a.resolved, a.env.function);
        return k;
    };
    size_t total = 0;
    for (const auto& fi : g1.functions()) {
        const auto k1 = key(g1, fi.index);
        CHECK(k1 == key(g3, fi.index));
        for (const auto& a : k1) {
            CHECK(std::get<2>(a));  // resolved
            ++total;
        }
    }
    CHECK(total > 16000);

    // Module 0 (index.js): func 83 is the factory, `_App` its slot 0, read by
    // the arrow function func 84 (`() => _App.default`).
    CHECK(namedAccesses(g3, 84) == Named{{false, "_App"}});
}

TEST_CASE("environments: every access of ridemovi resolved", "[environments]") {
    auto f = testutil::openOrSkip(CORPUS + "/ridemovi.hbc");
    size_t total = 0, unresolved = 0;
    for (const auto& fi : f.functions())
        for (const auto& a : f.env_accesses(fi.index)) {
            ++total;
            if (!a.resolved) ++unresolved;
        }
    CHECK(total > 130000);
    CHECK(unresolved == 0);
}

TEST_CASE("environments: out of range function index throws", "[environments]") {
    auto f = hbc::HBCFile::open(CORPUS + "/hello.hbc");
    CHECK_THROWS_AS(f.env_accesses(9999), std::out_of_range);
}
