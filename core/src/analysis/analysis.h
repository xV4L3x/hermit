// Private header: whole-file analyses behind HBCFile::call_graph(),
// recover_modules() and literal_sites(). Each disassembles every function
// (O(N)), so HBCFile memoizes them in Impl (parser.cpp).
#pragma once

#include "hbc/hbc.h"

#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace hbc {

std::vector<CallSite>    computeCallGraph(const HBCFile& self);
struct MetroModules {
    std::vector<MetroModule> modules;  // one per __d call, in global-function order
    std::vector<uint32_t>   entries;  // __r arguments, in call order
};

MetroModules             computeModules(const HBCFile& self);

// Closure environments (environments.cpp), indexed by function.
struct EnvModel {
    std::vector<std::vector<EnvAccess>>       accesses;
    std::vector<std::vector<EnvironmentInfo>> environments;
    std::vector<std::optional<EnvKey>>        captured;
    std::map<std::pair<EnvKey, uint32_t>, std::vector<EnvSlotUse>> slotUses;
};

EnvModel                 computeEnvironments(const HBCFile& self);

// Value analysis (values.cpp). Functions are grouped by Metro module (factory +
// nested functions) or, for the functions outside every module, in group -1.
struct ValueGroup {
    std::map<uint32_t, std::vector<InstrValues>> flow;  // function -> value_flow()
    std::map<std::pair<EnvKey, uint32_t>, AbstractValue> slots;  // agreed slot values
    std::optional<AbstractValue> moduleExports;  // value stored to module.exports, if agreed
    ModuleExports exports;                       // module_exports()
};

// HBCFile::resolve_value(): `v` followed through module exports. `group`
// returns a module's (memoized) analysis, nullptr if unavailable.
AbstractValue            resolveThroughExports(
    const AbstractValue& v, const std::function<const ValueGroup*(int64_t)>& group);

// Group of every function: module id, or -1.
std::vector<int64_t>     computeFunctionGroups(const HBCFile& self);
// Analyzes one group. `groupOf` from computeFunctionGroups(); `groupResult`
// returns another group's (memoized) result, for the interop-helper check.
ValueGroup               computeValueGroup(
    const HBCFile& self, int64_t group, const std::vector<int64_t>& groupOf,
    const std::function<const ValueGroup*(int64_t)>& groupResult);
std::vector<LiteralSite> computeLiteralSites(const HBCFile& self);

} // namespace hbc
