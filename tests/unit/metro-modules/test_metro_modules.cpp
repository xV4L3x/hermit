// =============================================================================
// test_metro_modules.cpp - Metro module recovery: recover_modules() and
// metro_entry_modules()
// =============================================================================
// A Metro bundle registers each module in the global function with
// `__d(factory, moduleId, dependencyMap)` and starts it with `__r(moduleId)`.
//
//   metro_module.hbc  seeder/bundle_metro.py over seeder/samples/metro/
//                     (math_utils = 0, string_utils = 1, app = 2): every
//                     dependencyMap is `[]` (the seeder writes require ids into
//                     the factory body), entry `__r(2)`.
//   hermitvault.hbc   real Metro bundle (React Native 0.86, HBC 96); the
//                     expected values below come from its Metro JS
//                     (hermit-vault/build/main.js), the whole bundle is
//                     checked by tests/integration/metro-modules/.
//   ridemovi.hbc      App Store bundle: 3422 modules. Its prelude runs three
//                     polyfill closures (funcs 1, 16, 63) with Call2 before the
//                     first __d: none of them is a module.
// =============================================================================
#include <catch2/catch_test_macros.hpp>

#include "hbc/hbc.h"
#include "test_util.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

const std::string CORPUS = HBC_CORPUS_DIR;

using Deps = std::vector<std::optional<uint32_t>>;

const hbc::ModuleInfo* findModule(const std::vector<hbc::ModuleInfo>& mods, uint32_t id) {
    for (const auto& m : mods)
        if (m.moduleId == id) return &m;
    return nullptr;
}

} // namespace

TEST_CASE("metro modules: seeder bundle has three modules and entry 2", "[metro-modules]") {
    auto f    = testutil::openOrSkip(CORPUS + "/metro_module.hbc");
    auto mods = f.recover_modules();

    REQUIRE(mods.size() == 3);
    for (uint32_t id = 0; id < 3; ++id) {
        CHECK(mods[id].moduleId == id);  // __d calls are in module-id order
        CHECK(mods[id].dependenciesKnown);
        CHECK(mods[id].dependencies.empty());
    }
    CHECK(f.metro_entry_modules() == std::vector<uint32_t>{2});
}

TEST_CASE("metro modules: hermitvault deps match the Metro JS", "[metro-modules]") {
    auto f    = testutil::openOrSkip(CORPUS + "/hermitvault.hbc");
    auto mods = f.recover_modules();

    CHECK(mods.size() == 534);
    CHECK(f.metro_entry_modules() == std::vector<uint32_t>{106, 0});

    const auto* m0 = findModule(mods, 0);
    REQUIRE(m0);
    CHECK(m0->functionIndex == 83);
    CHECK(m0->dependencies == Deps{1, 2, 513, 533});

    const auto* m1 = findModule(mods, 1);
    REQUIRE(m1);
    CHECK(m1->dependenciesKnown);
    CHECK(m1->dependencies.empty());  // `NewArray r, 0`
}

TEST_CASE("metro modules: ridemovi", "[metro-modules]") {
    auto f    = testutil::openOrSkip(CORPUS + "/ridemovi.hbc");
    auto mods = f.recover_modules();

    REQUIRE(mods.size() == 3422);
    std::set<uint32_t> ids;
    for (const auto& m : mods) {
        ids.insert(m.moduleId);
        CHECK(m.dependenciesKnown);
    }
    CHECK(ids.size() == mods.size());

    // Every dependency is a module of the bundle.
    for (const auto& m : mods)
        for (const auto& d : m.dependencies)
            if (d) CHECK(ids.count(*d) == 1);

    // Prelude polyfill closures are not modules (func 63 used to get id 0 from
    // the LoadConstZero of the following __d).
    for (const auto& m : mods) {
        CHECK(m.functionIndex != 1);
        CHECK(m.functionIndex != 16);
        CHECK(m.functionIndex != 63);
    }

    const auto* m0 = findModule(mods, 0);
    REQUIRE(m0);
    CHECK(m0->functionIndex == 74);
    CHECK(m0->dependencies == Deps{1, 2, 3, 472, 997, 3421, 1208});
    CHECK(m0->defineOffset == 0x215);

    CHECK(f.metro_entry_modules() == std::vector<uint32_t>{54, 0});
}

TEST_CASE("metro modules: non-Metro file has no entry points", "[metro-modules]") {
    auto f = hbc::HBCFile::open(CORPUS + "/hello.hbc");
    CHECK(f.recover_modules().empty());
    CHECK(f.metro_entry_modules().empty());
}
