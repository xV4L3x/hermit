// =============================================================================
// test_cfg.cpp - Catch2 tests for the Control Flow Graph
// =============================================================================
// Uses hello.hbc (linear function) and control_flow.hbc (if/else, loop).
// The file paths are injected by CMake via HBC_CORPUS_DIR.
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "hbc/hbc.h"

#include <string>
#include <vector>

static constexpr const char* HELLO_HBC        = HBC_CORPUS_DIR "/hello.hbc";
static constexpr const char* CONTROL_FLOW_HBC = HBC_CORPUS_DIR "/control_flow.hbc";
static constexpr const char* SWITCH_DENSE_HBC = HBC_CORPUS_DIR "/switch_dense.hbc";

// ---------------------------------------------------------------------------
// Helper: returns true if the CFG contains an edge of the given kind
// ---------------------------------------------------------------------------
static bool hasEdgeKind(const hbc::CFGData& cfg, hbc::EdgeKind kind) {
    for (const auto& bb : cfg.blocks)
        for (const auto& e : bb.successors)
            if (e.kind == kind) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Helper: number of blocks that are loop headers
// ---------------------------------------------------------------------------
static uint32_t countLoopHeaders(const hbc::CFGData& cfg) {
    uint32_t n = 0;
    for (const auto& bb : cfg.blocks)
        if (bb.isLoopHeader) n++;
    return n;
}

// ===========================================================================
// Test 1 - linear function (hello.hbc, func 0 = global)
// ===========================================================================
// global has sequential instructions and ends with Ret.
// Must produce exactly 1 basic block with no successors (exit node).
TEST_CASE("CFG: linear function -> 1 block, no successors", "[cfg]") {
    auto hbc = hbc::HBCFile::open(HELLO_HBC);
    // func 0 is the global one, which is linear: it calls greet and does Ret
    auto cfg = hbc.cfg(0);

    REQUIRE_FALSE(cfg.blocks.empty());
    // A linear function can have a single block
    CHECK(cfg.blocks.size() == 1);
    CHECK(cfg.hasLoops == false);
    // The entry block has no successors (ends with Ret)
    CHECK(cfg.blocks[cfg.entryId].successors.empty());
}

// ===========================================================================
// Test 2 - function with if/else (control_flow.hbc)
// ===========================================================================
// The max/factorial/sumTo functions use conditional jumps.
// We check: at least 2 blocks, presence of CondTaken + CondFallThru.
TEST_CASE("CFG: control_flow.hbc has conditional jumps", "[cfg]") {
    auto hbc = hbc::HBCFile::open(CONTROL_FLOW_HBC);
    const auto& funcs = hbc.functions();
    REQUIRE_FALSE(funcs.empty());

    bool foundConditional = false;
    for (uint32_t i = 0; i < funcs.size(); i++) {
        auto cfg = hbc.cfg(i);
        if (hasEdgeKind(cfg, hbc::EdgeKind::CondTaken) ||
            hasEdgeKind(cfg, hbc::EdgeKind::CondFallThru)) {
            foundConditional = true;
            CHECK(cfg.blocks.size() >= 2);
            break;
        }
    }
    CHECK(foundConditional);
}

// ===========================================================================
// Test 3 - function with a loop (control_flow.hbc)
// ===========================================================================
// factorial or sumTo has a loop -> back-edge DFS -> hasLoops + isLoopHeader.
TEST_CASE("CFG: control_flow.hbc has a loop (back-edge + loop header)", "[cfg]") {
    auto hbc = hbc::HBCFile::open(CONTROL_FLOW_HBC);
    const auto& funcs = hbc.functions();
    REQUIRE_FALSE(funcs.empty());

    bool foundLoop = false;
    for (uint32_t i = 0; i < funcs.size(); i++) {
        auto cfg = hbc.cfg(i);
        if (cfg.hasLoops) {
            foundLoop = true;
            CHECK(hasEdgeKind(cfg, hbc::EdgeKind::BackEdge));
            CHECK(countLoopHeaders(cfg) >= 1);
            break;
        }
    }
    CHECK(foundLoop);
}

// ===========================================================================
// Test 4 - predecessors consistent with successors
// ===========================================================================
// For every edge A->B in A's successors, B.predecessors must contain A.id.
TEST_CASE("CFG: predecessors consistent with successors", "[cfg]") {
    auto hbc = hbc::HBCFile::open(CONTROL_FLOW_HBC);
    const auto& funcs = hbc.functions();

    for (uint32_t fi = 0; fi < funcs.size(); fi++) {
        auto cfg = hbc.cfg(fi);
        for (const auto& bb : cfg.blocks) {
            for (const auto& edge : bb.successors) {
                const auto& target = cfg.blocks[edge.targetId];
                bool found = false;
                for (uint32_t pid : target.predecessors)
                    if (pid == bb.id) { found = true; break; }
                CHECK(found);
            }
        }
    }
}

// ===========================================================================
// Test 5 - entry block has startOffset == 0
// ===========================================================================
TEST_CASE("CFG: entry block starts at offset 0", "[cfg]") {
    auto hbc = hbc::HBCFile::open(HELLO_HBC);
    const auto& funcs = hbc.functions();
    for (uint32_t i = 0; i < funcs.size(); i++) {
        auto cfg = hbc.cfg(i);
        REQUIRE_FALSE(cfg.blocks.empty());
        CHECK(cfg.blocks[cfg.entryId].startOffset == 0);
    }
}

// ===========================================================================
// Test 6 - SwitchImm: jump table resolved, every case gets a real edge
// ===========================================================================
// Regression for a real bug found writing the thesis appendix: the jump
// table of a SwitchImm ALWAYS lives PAST the function's own
// bytecodeSizeInBytes (in the gap before the next function's body), and its
// start must be rounded up to a 4-byte boundary - verified against
// `hermes -dump-bytecode` and the runtime interpreter's own table lookup
// (facebook/hermes @ hermes-2023-09-26-RNv0.73.0, lib/VM/Interpreter.cpp,
// `CASE(SwitchImm)`). Both facts are
// stated only in a source comment (BytecodeList.def, SwitchImm: jump tables
// "are appended to the bytecode", Arg2 "is dynamically aligned at runtime").
// Before the fix, disasm()/cfg() read the
// table from the per-function slice with an unaligned offset - every
// SwitchImm ended up with zero resolved targets and zero SwitchCase edges
// (only SwitchDefault), even though the cases are very much reachable at
// runtime.
//
// switch_dense.ts (12 contiguous cases, 0..11 - the only shape Hermes lowers
// to SwitchImm, see LowerSwitchIntoJumpTables: needs >=10 cases and a dense
// range). Regenerate the corpus with
// `.venv/bin/python -m seeder.compile seeder/samples/switch_dense.ts` if missing
// (seeder/corpus/*.hbc is gitignored).
TEST_CASE("CFG: SwitchImm produces SwitchCase edges for every case", "[cfg]") {
    auto hbc = hbc::HBCFile::open(SWITCH_DENSE_HBC);
    const auto& funcs = hbc.functions();

    uint32_t classifyIdx = UINT32_MAX;
    for (const auto& fi : funcs)
        if (fi.name == "classify") { classifyIdx = fi.index; break; }
    REQUIRE(classifyIdx != UINT32_MAX);

    auto instrs = hbc.disasm(classifyIdx);
    const hbc::Instruction* switchInstr = nullptr;
    for (const auto& instr : instrs)
        if (instr.mnemonic == "SwitchImm") { switchInstr = &instr; break; }
    REQUIRE(switchInstr != nullptr);

    // 12 contiguous cases (0..11): the disassembler must resolve all 12 targets.
    REQUIRE(switchInstr->switchTargets.size() == 12);

    auto cfg = hbc.cfg(classifyIdx);
    uint32_t switchCaseEdges = 0, switchDefaultEdges = 0;
    for (const auto& bb : cfg.blocks)
        for (const auto& e : bb.successors) {
            if (e.kind == hbc::EdgeKind::SwitchCase)    switchCaseEdges++;
            if (e.kind == hbc::EdgeKind::SwitchDefault) switchDefaultEdges++;
        }
    CHECK(switchCaseEdges == 12);
    CHECK(switchDefaultEdges == 1);

    // Every resolved target must land exactly on a block boundary (no
    // misaligned/garbage offset) - cross-check disasm's switchTargets
    // against the CFG's own leader set.
    for (uint32_t target : switchInstr->switchTargets) {
        bool isLeader = false;
        for (const auto& bb : cfg.blocks)
            if (bb.startOffset == target) { isLeader = true; break; }
        CHECK(isLeader);
    }
}
