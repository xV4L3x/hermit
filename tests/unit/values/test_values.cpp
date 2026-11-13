// =============================================================================
// test_values.cpp - value analysis: value_flow(), env_slot_value(),
// function_module(), AbstractValue
// =============================================================================
// hermitvault.hbc, module 0 (index.js), Metro JS:
//   var _interopRequireDefault = _$$_REQUIRE(_dependencyMap[0]);   // module 1
//   var _reactNative = _$$_REQUIRE(_dependencyMap[1]);             // module 2
//   var _App = _interopRequireDefault(_$$_REQUIRE(_dependencyMap[2]));  // 513
//   _reactNative.AppRegistry.registerComponent(
//       _$$_REQUIRE(_dependencyMap[3]).name, () => _App.default);  // 533
// factory = func 83 (`_App` in env slot 0), the arrow function = func 84.
// The whole bundle is checked against the JS by
// tests/integration/metro-modules/test_metro_truth.py.
// =============================================================================
#include <catch2/catch_test_macros.hpp>

#include "hbc/hbc.h"
#include "test_util.h"

#include <map>
#include <optional>
#include <string>

namespace {

const std::string CORPUS = HBC_CORPUS_DIR;

// offset -> InstrValues
std::map<uint32_t, hbc::InstrValues> flowOf(const hbc::HBCFile& f, uint32_t fn) {
    std::map<uint32_t, hbc::InstrValues> m;
    for (auto& iv : f.value_flow(fn)) m[iv.offset] = iv;
    return m;
}

std::string str(const std::optional<hbc::AbstractValue>& v) { return v ? v->toString() : "-"; }

} // namespace

TEST_CASE("values: hermitvault module 0 (index.js)", "[values]") {
    auto f = testutil::openOrSkip(CORPUS + "/hermitvault.hbc");
    REQUIRE(f.function_module(83) == std::optional<uint32_t>(0));
    REQUIRE(f.function_module(84) == std::optional<uint32_t>(0));

    auto flow = flowOf(f, 83);
    // results of every Call2 / GetById, in order
    std::vector<std::string> calls, props;
    for (const auto& instr : f.disasm(83)) {
        auto it = flow.find(instr.offset);
        if (it == flow.end()) continue;
        if (instr.mnemonic == "Call2") calls.push_back(str(it->second.result));
        if (instr.mnemonic.rfind("GetById", 0) == 0) props.push_back(str(it->second.result));
    }
    CHECK(calls == std::vector<std::string>{"module_1", "module_2", "module_513",
                                            "importDefaultOf(module_513)", "module_533"});
    CHECK(props == std::vector<std::string>{"module_2.AppRegistry",
                                            "module_2.AppRegistry.registerComponent",
                                            "module_533.name"});

    // _App: env slot 0 of the factory, read by the arrow function
    const auto env = f.environments(83).at(0).env;
    CHECK(str(f.env_slot_value(env, 0)) == "importDefaultOf(module_513)");
    bool sawDefault = false;
    for (const auto& iv : f.value_flow(84))
        if (str(iv.result) == "importDefaultOf(module_513).default") sawDefault = true;
    CHECK(sawDefault);
}

TEST_CASE("values: the interop helper module exports a closure", "[values]") {
    auto f = testutil::openOrSkip(CORPUS + "/hermitvault.hbc");
    // module 1 = _interopRequireDefault; its factory is func 85, the helper 86
    REQUIRE(f.function_module(86) == std::optional<uint32_t>(1));
    bool created = false;
    for (const auto& iv : f.value_flow(85))
        if (str(iv.result) == "func_86") created = true;
    CHECK(created);
}

TEST_CASE("values: functions outside modules", "[values]") {
    auto f = hbc::HBCFile::open(CORPUS + "/closure_scope_g.hbc");
    for (const auto& fi : f.functions()) CHECK_FALSE(f.function_module(fi.index).has_value());
    // makeCounter is created in the global function: CreateClosure -> func_1
    bool closure = false;
    for (const auto& iv : f.value_flow(f.header().globalCodeIndex))
        if (str(iv.result) == "func_1") closure = true;
    CHECK(closure);
}

TEST_CASE("values: AbstractValue rendering and equality", "[values]") {
    hbc::AbstractValue m;
    m.kind = hbc::AbstractValue::Kind::Module;
    m.index = 7;
    hbc::AbstractValue p;
    p.kind = hbc::AbstractValue::Kind::Prop;
    p.name = "foo";
    p.base = std::make_shared<const hbc::AbstractValue>(m);
    CHECK(p.toString() == "module_7.foo");
    hbc::AbstractValue q = p;
    q.name = "a-b";
    CHECK(q.toString() == "module_7[\"a-b\"]");
    CHECK(p != q);
    hbc::AbstractValue p2 = p;
    p2.base = std::make_shared<const hbc::AbstractValue>(m);  // distinct pointer, same value
    CHECK(p == p2);
    hbc::AbstractValue fp;
    fp.kind = hbc::AbstractValue::Kind::FactoryParam;
    fp.index = 7;
    CHECK(fp.toString() == "dependencyMap");
}

TEST_CASE("values: module exports", "[values]") {
    auto f = testutil::openOrSkip(CORPUS + "/hermitvault.hbc");
    auto names = [](const hbc::ModuleExports& e) {
        std::map<std::string, std::string> m;
        for (const auto& n : e.names) m[n.name] = str(n.value) + (n.viaGetter ? " (get)" : "");
        return m;
    };

    // module 1: module.exports = _interopRequireDefault, __esModule, default
    auto m1 = f.module_exports(1);
    CHECK(m1.reassigned);
    CHECK(str(m1.value) == "func_86");
    CHECK(m1.esModule);
    CHECK(names(m1).count("default") == 1);

    // module 533 (app.json): module.exports = {name: "HermitDemo", displayName: ...}
    auto m533 = f.module_exports(533);
    CHECK(names(m533) == std::map<std::string, std::string>{
        {"displayName", "\"HermitDemo\""}, {"name", "\"HermitDemo\""}});

    // module 2 (react-native): module.exports = { get ActivityIndicator() {
    //   return _$$_REQUIRE(_dependencyMap[0]).default; }, ... } -> 99 getters
    auto m2 = f.module_exports(2);
    CHECK(m2.names.size() == 99);
    CHECK(names(m2)["ActivityIndicator"] == "module_3.default (get)");

    // module 60: defineProperty(exports, "default", {get}) + export * (forEach)
    auto m60 = f.module_exports(60);
    CHECK(m60.esModule);
    CHECK(m60.dynamicNames);
    CHECK(names(m60).count("default") == 1);

    // not a module
    CHECK(f.module_exports(99999).names.empty());
}

TEST_CASE("values: parameters of a closure called from one place", "[values]") {
    auto f = testutil::openOrSkip(CORPUS + "/hermitvault.hbc");
    // module 49: _callSuper(this, RCTDeviceEventEmitterImpl, arguments) is the
    // only call of _callSuper (func 405): its parameter `o` (index 2) is the
    // class constructor func 412.
    REQUIRE(f.functions()[405].name == "_callSuper");
    bool found = false;
    for (const auto& iv : f.value_flow(405))
        if (str(iv.result) == "func_412") found = true;
    CHECK(found);
}

TEST_CASE("values: call resolution", "[values]") {
    auto f = testutil::openOrSkip(CORPUS + "/hermitvault.hbc");
    using Via = hbc::ResolvedCall::Via;

    // index.js: `_interopRequireDefault(_$$_REQUIRE(_dependencyMap[2]))` calls
    // module 1's module.exports, the helper func 86.
    bool interop = false;
    for (const auto& rc : f.resolved_calls(83))
        if (rc.offset == 0x2d) {
            interop = true;
            CHECK(rc.callee == 86);
            CHECK(rc.via == Via::Export);
            CHECK(rc.calleeValue.toString() == "module_1");
        }
    CHECK(interop);

    // `() => _App.default`: the App component (module 513's default export).
    hbc::AbstractValue app;
    for (const auto& iv : f.value_flow(84))
        if (iv.result && iv.result->toString() == "importDefaultOf(module_513).default") app = *iv.result;
    const auto resolved = f.resolve_value(app);
    REQUIRE(resolved.kind == hbc::AbstractValue::Kind::Closure);
    CHECK(f.functions()[resolved.index].name == "App");

    // react/jsx-runtime: `exports.jsx = jsxProd` -> calls of module_X.jsx reach jsxProd
    size_t jsx = 0;
    for (const auto& fi : f.functions())
        for (const auto& rc : f.resolved_calls(fi.index))
            if (rc.calleeValue.kind == hbc::AbstractValue::Kind::Prop && rc.calleeValue.name == "jsx") {
                CHECK(f.functions()[rc.callee].name == "jsxProd");
                ++jsx;
            }
    CHECK(jsx > 100);
}

TEST_CASE("values: function_callers across modules", "[values]") {
    auto f = testutil::openOrSkip(CORPUS + "/hermitvault.hbc");
    // _interopRequireDefault (func 86) is called from every module importing
    // module 1; index.js (func 83) is one of them.
    const auto callers = f.function_callers(86);
    CHECK(callers.size() > 1000);
    bool fromIndex = false;
    for (const auto& c : callers)
        if (c.caller == 83 && c.offset == 0x2d) fromIndex = true;
    CHECK(fromIndex);
    // every reported caller really resolves to it
    for (size_t i = 0; i < callers.size(); i += 97) {
        bool ok = false;
        for (const auto& rc : f.resolved_calls(callers[i].caller))
            if (rc.offset == callers[i].offset && rc.callee == 86) ok = true;
        CHECK(ok);
    }
}
