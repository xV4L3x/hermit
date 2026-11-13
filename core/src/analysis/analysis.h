// Private header: whole-file analyses behind HBCFile::call_graph(),
// recover_modules() and literal_sites(). Each disassembles every function
// (O(N)), so HBCFile memoizes them in Impl (parser.cpp).
#pragma once

#include "hbc/hbc.h"

#include <vector>

namespace hbc {

std::vector<CallSite>    computeCallGraph(const HBCFile& self);
struct MetroModules {
    std::vector<ModuleInfo> modules;  // one per __d call, in global-function order
    std::vector<uint32_t>   entries;  // __r arguments, in call order
};

MetroModules             computeModules(const HBCFile& self);
std::vector<LiteralSite> computeLiteralSites(const HBCFile& self);

} // namespace hbc
