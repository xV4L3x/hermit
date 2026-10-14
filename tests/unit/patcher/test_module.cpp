// =============================================================================
// test_module.cpp - structural serializer + relocatable assembler
// =============================================================================
// hbc::Module rebuilds the file after inserting instructions into a
// function; hbc::assemble_function assigns offsets, runs branch relaxation
// (Addr8 -> Addr32) and fills in jump displacements (target given as an index).
// HBC_CORPUS_DIR is injected by CMake.
// =============================================================================

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "hbc/hbc.h"
#include "test_util.h"
#include "hbc/module.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using hbc::Operand;
using testutil::openOrSkip;
using testutil::opValue;

// ---------------------------------------------------------------------------

// Builds the list of AsmInstr for a function, resolving jump targets to
// instruction INDICES.
static std::vector<hbc::AsmInstr> buildIr(const std::vector<hbc::Instruction>& ins) {
    auto tbl = hbc::opcode_table(96);
    std::unordered_map<uint8_t, const hbc::OpcodeDesc*> byOp;
    for (const auto& d : tbl) byOp[d.opcode] = &d;

    std::unordered_map<uint32_t, int32_t> off2idx;
    for (size_t i = 0; i < ins.size(); ++i) off2idx[ins[i].offset] = static_cast<int32_t>(i);

    std::vector<hbc::AsmInstr> ir;
    ir.reserve(ins.size());
    for (const auto& x : ins) {
        hbc::AsmInstr a;
        a.opcode = x.opcode;
        a.origByteOffset = static_cast<int32_t>(x.offset);
        for (const auto& o : x.operands) a.operands.push_back(opValue(o));

        auto it = byOp.find(x.opcode);
        if (it != byOp.end() &&
            (it->second->jumpKind == "Unconditional" || it->second->jumpKind == "Conditional")) {
            a.addrOperandPos = 0;
            uint32_t tgt = static_cast<uint32_t>(
                static_cast<int32_t>(x.offset) + x.operands[0].i32);
            auto ti = off2idx.find(tgt);
            a.jumpTargetIdx = (ti != off2idx.end()) ? ti->second : -1;
        }
        ir.push_back(std::move(a));
    }
    return ir;
}

static uint8_t opcodeOf(const char* mnemonic) {
    for (const auto& d : hbc::opcode_table(96))
        if (d.mnemonic == mnemonic) return d.opcode;
    throw std::runtime_error(std::string("opcode not found: ") + mnemonic);
}

static hbc::HBCFile reparse(const std::vector<uint8_t>& bytes) {
    return hbc::HBCFile::from_bytes(std::string(bytes.begin(), bytes.end()));
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

static void checkModuleIdentity(const char* path) {
    auto f = openOrSkip(path);
    auto m = hbc::Module::from(f);
    REQUIRE(m.serialize() == f.serialize());
}

TEST_CASE("Module identity serialize", "[patcher]") {
    checkModuleIdentity(HBC_CORPUS_DIR "/loop_sum_g0.hbc");
    checkModuleIdentity(HBC_CORPUS_DIR "/try_catch_g0.hbc");   // with EH
    checkModuleIdentity(HBC_CORPUS_DIR "/control_flow_g0.hbc");
    checkModuleIdentity(HBC_CORPUS_DIR "/hello.hbc");          // -g1: debug blob
    checkModuleIdentity(HBC_CORPUS_DIR "/control_flow.hbc");   // -g1
}

TEST_CASE("assemble_function is an identity when nothing is inserted", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/control_flow_g0.hbc");
    for (uint32_t i = 0; i < f.header().functionCount; ++i) {
        auto ins = f.disasm(i);
        bool skip = false;
        for (const auto& x : ins) if (x.mnemonic == "SwitchImm" || x.mnemonic == "???") skip = true;
        if (skip) continue;

        auto r = hbc::assemble_function(96, buildIr(ins));
        const auto& fi = f.functions()[i];
        std::vector<uint8_t> orig(f.raw_data() + fi.offset,
                                   f.raw_data() + fi.offset + fi.bytecodeSizeInBytes);
        INFO("function " << i << " " << f.functions()[i].name);
        REQUIRE(r.bytecode == orig);
    }
}

// ---------------------------------------------------------------------------
// Insertion
// ---------------------------------------------------------------------------

// Inserts `Mov r0,r0` (no-op) at `atIdx` in function `name`, re-serializes and
// returns the re-parsed file. `deltaOut` = bytes added.
static hbc::HBCFile insertNop(const char* path, const char* name, size_t atIdx,
                              size_t count, uint32_t* newSizeOut) {
    auto f = openOrSkip(path);
    uint32_t fidx = 0;
    for (const auto& fn : f.functions()) if (fn.name == name) fidx = fn.index;
    const auto& fi = f.functions()[fidx];

    auto ir = buildIr(f.disasm(fidx));
    for (auto& a : ir)
        if (a.jumpTargetIdx >= static_cast<int32_t>(atIdx))
            a.jumpTargetIdx += static_cast<int32_t>(count);

    hbc::AsmInstr nop;
    nop.opcode = opcodeOf("Mov");
    nop.operands = {0.0, 0.0};
    for (size_t k = 0; k < count; ++k) ir.insert(ir.begin() + atIdx, nop);

    auto m = hbc::Module::from(f);
    m.set_function_body(fidx, ir, fi.frameSize);
    auto out = m.serialize();
    *newSizeOut = 0;
    auto g = reparse(out);
    *newSizeOut = g.functions()[fidx].bytecodeSizeInBytes;
    return g;
}

TEST_CASE("insert a no-op grows the function and shifts later ones", "[patcher]") {
    auto f0 = openOrSkip(HBC_CORPUS_DIR "/control_flow_g0.hbc");
    uint32_t maxIdx = 0, sumIdx = 0;
    for (const auto& fn : f0.functions()) {
        if (fn.name == "max")  maxIdx = fn.index;
        if (fn.name == "sumTo") sumIdx = fn.index;
    }
    uint32_t oldMax = f0.functions()[maxIdx].bytecodeSizeInBytes;
    uint32_t oldSumOff = f0.functions()[sumIdx].offset;

    uint32_t newSize = 0;
    auto g = insertNop(HBC_CORPUS_DIR "/control_flow_g0.hbc", "max", 1, 1, &newSize);

    REQUIRE(newSize == oldMax + 3);                       // Mov = 1 + Reg8 + Reg8
    REQUIRE(g.functions()[sumIdx].offset == oldSumOff + 3);
    // disasm of the patched function: +1 instruction, all known
    auto ins = g.disasm(maxIdx);
    for (const auto& x : ins) REQUIRE(x.mnemonic != "???");
}

TEST_CASE("backward jump target is preserved after an insertion before it", "[patcher]") {
    // sumTo: idx 9 = backward JLessEqual (-10 -> 0x11). We insert right before it.
    auto f0 = openOrSkip(HBC_CORPUS_DIR "/control_flow_g0.hbc");
    uint32_t sumIdx = 0;
    for (const auto& fn : f0.functions()) if (fn.name == "sumTo") sumIdx = fn.index;
    auto before = f0.disasm(sumIdx);
    // find the index of the backward jump
    int jIdx = -1;
    for (size_t i = 0; i < before.size(); ++i)
        if (!before[i].operands.empty() && before[i].mnemonic.rfind("J", 0) == 0 &&
            before[i].operands[0].i32 < 0)
            jIdx = static_cast<int>(i);
    REQUIRE(jIdx > 0);
    uint32_t origTargetByte =
        static_cast<uint32_t>(static_cast<int32_t>(before[jIdx].offset) + before[jIdx].operands[0].i32);

    uint32_t newSize = 0;
    auto g = insertNop(HBC_CORPUS_DIR "/control_flow_g0.hbc", "sumTo",
                       static_cast<size_t>(jIdx), 1, &newSize);
    auto after = g.disasm(sumIdx);
    // the jump is now at jIdx+1; its absolute target must be the instruction
    // that used to be at origTargetByte (unchanged, it precedes the insertion point).
    const auto& j2 = after[static_cast<size_t>(jIdx) + 1];
    REQUIRE(j2.mnemonic.rfind("J", 0) == 0);
    uint32_t newTargetByte =
        static_cast<uint32_t>(static_cast<int32_t>(j2.offset) + j2.operands[0].i32);
    REQUIRE(newTargetByte == origTargetByte);
}

TEST_CASE("branch relaxation promotes a short jump to Long", "[patcher]") {
    // sumTo idx 5 = JmpFalse (Addr8). We insert 60 no-ops between the jump and the target.
    auto f0 = openOrSkip(HBC_CORPUS_DIR "/control_flow_g0.hbc");
    uint32_t sumIdx = 0;
    for (const auto& fn : f0.functions()) if (fn.name == "sumTo") sumIdx = fn.index;

    uint32_t newSize = 0;
    auto g = insertNop(HBC_CORPUS_DIR "/control_flow_g0.hbc", "sumTo", 6, 60, &newSize);
    auto ins = g.disasm(sumIdx);
    bool foundLong = false;
    for (const auto& x : ins) if (x.mnemonic == "JmpFalseLong") foundLong = true;
    REQUIRE(foundLong);
    REQUIRE(newSize == 33 + 60 * 3 + 3);   // 60*Mov(3) + relaxation (Addr8->Addr32 = +3)
}

TEST_CASE("exception handler offsets are relocated", "[patcher]") {
    auto f0 = openOrSkip(HBC_CORPUS_DIR "/try_catch_g0.hbc");
    uint32_t rIdx = 0;
    for (const auto& fn : f0.functions()) if (fn.name == "risky") rIdx = fn.index;
    auto eh0 = f0.functions()[rIdx].exceptionHandlers;
    REQUIRE(eh0.size() >= 1);

    uint32_t newSize = 0;
    auto g = insertNop(HBC_CORPUS_DIR "/try_catch_g0.hbc", "risky", 1, 1, &newSize);  // +3 bytes at idx 1 (offset > start)
    auto eh1 = g.functions()[rIdx].exceptionHandlers;
    REQUIRE(eh1.size() == eh0.size());
    for (size_t i = 0; i < eh0.size(); ++i) {
        REQUIRE(eh1[i].start  == eh0[i].start  + 3);
        REQUIRE(eh1[i].end    == eh0[i].end    + 3);
        REQUIRE(eh1[i].target == eh0[i].target + 3);
    }
}

// Removes instruction `atIdx` (must not be a jump target), re-serializes.
static hbc::HBCFile deleteInstr(const char* path, const char* name, size_t atIdx,
                                uint32_t* newSizeOut) {
    auto f = openOrSkip(path);
    uint32_t fidx = 0;
    for (const auto& fn : f.functions()) if (fn.name == name) fidx = fn.index;

    auto ir = buildIr(f.disasm(fidx));
    for (auto& a : ir) {
        REQUIRE(a.jumpTargetIdx != static_cast<int32_t>(atIdx));   // isn't a target
        if (a.jumpTargetIdx > static_cast<int32_t>(atIdx)) a.jumpTargetIdx -= 1;
    }
    ir.erase(ir.begin() + atIdx);

    auto m = hbc::Module::from(f);
    m.set_function_body(fidx, ir, f.functions()[fidx].frameSize);
    auto g = reparse(m.serialize());
    *newSizeOut = g.functions()[fidx].bytecodeSizeInBytes;
    return g;
}

TEST_CASE("delete an instruction shrinks the function and shifts later ones back", "[patcher]") {
    auto f0 = openOrSkip(HBC_CORPUS_DIR "/control_flow_g0.hbc");
    uint32_t facIdx = 0, sumIdx = 0;
    for (const auto& fn : f0.functions()) {
        if (fn.name == "factorial") facIdx = fn.index;
        if (fn.name == "sumTo")     sumIdx = fn.index;
    }
    uint32_t oldFac    = f0.functions()[facIdx].bytecodeSizeInBytes;
    uint32_t oldSumOff = f0.functions()[sumIdx].offset;

    // factorial idx 3 = GetGlobalObject (not a jump, not a target): 2 bytes.
    auto ins3 = f0.disasm(facIdx)[3];
    REQUIRE(ins3.mnemonic == "GetGlobalObject");

    uint32_t newSize = 0;
    auto g = deleteInstr(HBC_CORPUS_DIR "/control_flow_g0.hbc", "factorial", 3, &newSize);

    REQUIRE(newSize == oldFac - 2);
    REQUIRE(g.functions()[sumIdx].offset == oldSumOff - 2);
    for (const auto& x : g.disasm(facIdx)) REQUIRE(x.mnemonic != "???");
}

TEST_CASE("delete before a forward jump keeps its target", "[patcher]") {
    auto f0 = openOrSkip(HBC_CORPUS_DIR "/control_flow_g0.hbc");
    uint32_t facIdx = 0;
    for (const auto& fn : f0.functions()) if (fn.name == "factorial") facIdx = fn.index;
    auto before = f0.disasm(facIdx);
    // idx 2 = forward JLessEqual; target = last instruction (Ret 0).
    REQUIRE(before[2].mnemonic.rfind("J", 0) == 0);
    uint32_t origTargetByte = static_cast<uint32_t>(
        static_cast<int32_t>(before[2].offset) + before[2].operands[0].i32);
    std::string tgtMnem;
    for (const auto& x : before) if (x.offset == origTargetByte) tgtMnem = x.mnemonic;
    REQUIRE(tgtMnem == "Ret");

    uint32_t newSize = 0;
    auto g = deleteInstr(HBC_CORPUS_DIR "/control_flow_g0.hbc", "factorial", 3, &newSize);
    auto after = g.disasm(facIdx);
    const auto& j = after[2];
    uint32_t newTargetByte = static_cast<uint32_t>(
        static_cast<int32_t>(j.offset) + j.operands[0].i32);
    // the target must still be a Ret (the instruction itself wasn't touched)
    std::string m;
    for (const auto& x : after) if (x.offset == newTargetByte) m = x.mnemonic;
    REQUIRE(m == "Ret");
    REQUIRE(newTargetByte == origTargetByte - 2);   // the target moved by -2
}

TEST_CASE("frameSize bump is refused on a function with exception handlers", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/try_catch_g0.hbc");
    uint32_t rIdx = 0;
    for (const auto& fn : f.functions()) if (fn.name == "risky") rIdx = fn.index;
    uint32_t frame = f.functions()[rIdx].frameSize;

    auto ir = buildIr(f.disasm(rIdx));
    hbc::AsmInstr nop; nop.opcode = opcodeOf("Mov"); nop.operands = {0.0, 0.0};
    ir.insert(ir.begin() + 1, nop);

    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_AS(m.set_function_body(rIdx, ir, frame + 1), std::runtime_error);
    // same frameSize: ok
    REQUIRE_NOTHROW(m.set_function_body(rIdx, ir, frame));
}

TEST_CASE("Module refuses switch functions", "[patcher]") {
    try {
        auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/switch.hbc");
        int swIdx = -1;
        for (uint32_t i = 0; i < f.header().functionCount; ++i)
            for (const auto& x : f.disasm(i))
                if (x.mnemonic == "SwitchImm") swIdx = static_cast<int>(i);
        if (swIdx >= 0) {
            auto m = hbc::Module::from(f);
            auto ir = buildIr(f.disasm(static_cast<uint32_t>(swIdx)));
            REQUIRE_THROWS_AS(
                m.set_function_body(static_cast<uint32_t>(swIdx), ir,
                                    f.functions()[static_cast<uint32_t>(swIdx)].frameSize),
                std::runtime_error);
        }
    } catch (const std::exception&) {
        // switch.hbc missing / has no SwitchImm - ok
    }
}

// =========================================================================
// Overflowed functions + real-bundle layout
// =========================================================================

TEST_CASE("Module identity on a real bundle (overflow + non-monotonic + shared)", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");   // 12.7 MB, gitignored
    auto m = hbc::Module::from(f);
    REQUIRE(m.serialize() == f.serialize());
}

TEST_CASE("Module identity on overflow_func.hbc", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/overflow_func.hbc");
    auto m = hbc::Module::from(f);
    REQUIRE(m.serialize() == f.serialize());
}

// hermitvault.hbc - the thesis demo app, a real React Native bundle compiled
// as RN ships it (HBC 96, ~1.4 MB, Hermes V1 disabled). Gitignored: rebuild
// with `npm run build:hbc` in hermit-vault/. Unlike the toy samples it carries the
// full structure of a shipped bundle (Metro module system, many functions,
// UTF-16 strings), so proving the structural serializer reproduces it
// byte-for-byte anchors the round-trip claim on the application it is meant to
// analyse.
TEST_CASE("Module identity on hermitvault.hbc (demo app bundle)", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/hermitvault.hbc");
    REQUIRE(f.version() == 96);

    // HBCFile baseline: the serialized copy equals the bytes on disk.
    auto raw = f.serialize();
    REQUIRE(raw.size() == f.header().fileLength);
    REQUIRE(raw.size() == f.raw_size());
    REQUIRE(std::memcmp(raw.data(), f.raw_data(), f.raw_size()) == 0);

    // Structural serializer: dismantle into sections and rebuild from scratch,
    // recomputing every offset. The result must be byte-identical.
    auto m = hbc::Module::from(f);
    REQUIRE(m.serialize() == f.serialize());
}

TEST_CASE("edit an overflowed function (same size, EH preserved)", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/overflow_func.hbc");
    uint32_t ovIdx = 0;
    bool found = false;
    for (const auto& fn : f.functions())
        if (fn.overflowed) { ovIdx = fn.index; found = true; }
    REQUIRE(found);
    const auto& fi = f.functions()[ovIdx];
    REQUIRE(fi.frameSize > 127);   // precisely because it's overflowed

    auto ins = f.disasm(ovIdx);
    int row = -1;
    for (size_t i = 0; i < ins.size(); ++i)
        if (ins[i].mnemonic == "LoadConstUndefined") { row = static_cast<int>(i); break; }
    REQUIRE(row >= 0);

    auto ir = buildIr(ins);
    ir[static_cast<size_t>(row)].opcode = opcodeOf("LoadConstNull");   // same signature {Reg8}

    auto m = hbc::Module::from(f);
    m.set_function_body(ovIdx, ir, fi.frameSize);
    auto g = reparse(m.serialize());
    const auto& gi = g.functions()[ovIdx];
    REQUIRE(gi.overflowed);
    REQUIRE(gi.bytecodeSizeInBytes == fi.bytecodeSizeInBytes);
    REQUIRE(gi.frameSize == fi.frameSize);
    REQUIRE(g.disasm(ovIdx)[static_cast<size_t>(row)].mnemonic == "LoadConstNull");
    for (const auto& x : g.disasm(ovIdx)) REQUIRE(x.mnemonic != "???");
}

TEST_CASE("insert into an overflowed function relocates EH and largeOff", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/overflow_func.hbc");
    uint32_t ovIdx = 0;
    for (const auto& fn : f.functions()) if (fn.overflowed) ovIdx = fn.index;
    const auto& fi = f.functions()[ovIdx];
    auto eh0 = fi.exceptionHandlers;
    REQUIRE(eh0.size() >= 1);

    auto ir = buildIr(f.disasm(ovIdx));
    hbc::AsmInstr nop; nop.opcode = opcodeOf("Mov"); nop.operands = {0.0, 0.0};
    for (auto& a : ir) if (a.jumpTargetIdx >= 1) a.jumpTargetIdx += 1;
    ir.insert(ir.begin() + 1, nop);                  // +3 bytes at instruction 1's offset

    auto m = hbc::Module::from(f);
    m.set_function_body(ovIdx, ir, fi.frameSize);
    auto g = reparse(m.serialize());
    const auto& gi = g.functions()[ovIdx];
    REQUIRE(gi.overflowed);
    REQUIRE(gi.bytecodeSizeInBytes == fi.bytecodeSizeInBytes + 3);
    auto eh1 = gi.exceptionHandlers;
    REQUIRE(eh1.size() == eh0.size());
    for (size_t i = 0; i < eh0.size(); ++i) {
        REQUIRE(eh1[i].start  == eh0[i].start  + 3);
        REQUIRE(eh1[i].end    == eh0[i].end    + 3);
        REQUIRE(eh1[i].target == eh0[i].target + 3);
    }
    for (const auto& x : g.disasm(ovIdx)) REQUIRE(x.mnemonic != "???");
}

TEST_CASE("shared bytecode: refuse without editAll, apply with editAll", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    const auto& funcs = f.functions();
    // a function with shared body, not overflow, without EH
    int sIdx = -1;
    for (const auto& fn : funcs)
        if (fn.shared && !fn.overflowed && !fn.hasExceptionHandler) { sIdx = static_cast<int>(fn.index); break; }
    REQUIRE(sIdx >= 0);
    const auto& sfi = funcs[static_cast<uint32_t>(sIdx)];
    REQUIRE(sfi.shareCount >= 2);

    auto ir = buildIr(f.disasm(static_cast<uint32_t>(sIdx)));   // identical re-encode

    {
        auto m = hbc::Module::from(f);
        REQUIRE_THROWS_AS(
            m.set_function_body(static_cast<uint32_t>(sIdx), ir, sfi.frameSize, /*editAll=*/false),
            std::runtime_error);
    }
    {
        auto m = hbc::Module::from(f);
        m.set_function_body(static_cast<uint32_t>(sIdx), ir, sfi.frameSize, /*editAll=*/true);
        // identical re-encode -> byte-identical file
        REQUIRE(m.serialize() == f.serialize());
    }
}

// Inserts `Mov r0,r0` before instruction 1 of `fidx` and applies it with
// editAll to the whole shared group. Every EH boundary other than 0 is at or
// after instruction 1, so it must move by exactly +3.
static std::vector<hbc::AsmInstr> nopAt1(const hbc::HBCFile& f, uint32_t fidx) {
    auto ir = buildIr(f.disasm(fidx));
    for (auto& a : ir) if (a.jumpTargetIdx >= 1) a.jumpTargetIdx += 1;
    hbc::AsmInstr nop; nop.opcode = opcodeOf("Mov"); nop.operands = {0.0, 0.0};
    ir.insert(ir.begin() + 1, nop);
    return ir;
}

static void requireEhShifted(const std::vector<hbc::ExceptionHandlerInfo>& eh0,
                             const std::vector<hbc::ExceptionHandlerInfo>& eh1) {
    auto moved = [](uint32_t v) { return v == 0 ? 0u : v + 3; };
    REQUIRE(eh1.size() == eh0.size());
    for (size_t i = 0; i < eh0.size(); ++i) {
        REQUIRE(eh1[i].start  == moved(eh0[i].start));
        REQUIRE(eh1[i].end    == moved(eh0[i].end));
        REQUIRE(eh1[i].target == moved(eh0[i].target));
    }
}

TEST_CASE("editAll relocates the EH table of every member of the group (real bundle)", "[patcher]") {
    // ridemovi.hbc has shared groups whose members ALL carry an exception
    // handler (identical bodies with try/catch -> identical EH tables, one per
    // entry). Editing through any member must relocate every member's table.
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    const auto& funcs = f.functions();

    std::unordered_map<uint32_t, std::vector<uint32_t>> byOff;
    for (const auto& fn : funcs) byOff[fn.offset].push_back(fn.index);

    std::vector<uint32_t> group;
    for (const auto& [off, members] : byOff) {
        if (members.size() < 2) continue;
        bool ok = true;
        for (uint32_t i : members)
            if (!funcs[i].hasExceptionHandler || funcs[i].overflowed) ok = false;
        if (!ok) continue;
        for (const auto& x : f.disasm(members[0]))
            if (x.mnemonic == "SwitchImm" || x.mnemonic.rfind("SaveGenerator", 0) == 0 ||
                x.mnemonic == "???") ok = false;
        if (ok) { group = members; break; }
    }
    if (group.empty()) SKIP("no shared group with EH on every member in ridemovi.hbc");

    // edit through the first and through the last member: the result must not
    // depend on which member of the group was opened
    for (uint32_t dirtyIdx : {group.front(), group.back()}) {
        INFO("editing through function " << dirtyIdx);
        auto m = hbc::Module::from(f);
        m.set_function_body(dirtyIdx, nopAt1(f, dirtyIdx), funcs[dirtyIdx].frameSize, /*editAll=*/true);
        auto g = reparse(m.serialize());
        for (uint32_t i : group) {
            INFO("member " << i);
            REQUIRE(g.functions()[i].bytecodeSizeInBytes == funcs[i].bytecodeSizeInBytes + 3);
            REQUIRE(g.functions()[i].offset == g.functions()[group.front()].offset);
            requireEhShifted(funcs[i].exceptionHandlers, g.functions()[i].exceptionHandlers);
        }
    }
}

// Builds a mixed shared group on try_catch_g0.hbc: function `other` (no EH) is
// repointed at the body of `risky` (EH). No real file has such a group, but
// it is the case where the EH member is NOT the function being edited.
static std::string mixedSharedGroup(const hbc::HBCFile& f, uint32_t riskyIdx, uint32_t otherIdx) {
    std::string bytes(reinterpret_cast<const char*>(f.raw_data()), f.raw_size());
    auto* fh = reinterpret_cast<hbc::SmallFuncHeader*>(bytes.data() + sizeof(hbc::HBCFileHeader));
    fh[otherIdx].offset              = fh[riskyIdx].offset;
    fh[otherIdx].bytecodeSizeInBytes = fh[riskyIdx].bytecodeSizeInBytes;
    return bytes;
}

TEST_CASE("editAll through a member without EH relocates the EH of another member", "[patcher]") {
    auto f0 = openOrSkip(HBC_CORPUS_DIR "/try_catch_g0.hbc");
    int rIdx = -1, oIdx = -1;
    for (const auto& fn : f0.functions()) {
        if (fn.name == "risky") rIdx = static_cast<int>(fn.index);
        else if (!fn.hasExceptionHandler && !fn.overflowed && oIdx < 0) oIdx = static_cast<int>(fn.index);
    }
    REQUIRE(rIdx >= 0);
    REQUIRE(oIdx >= 0);
    const auto r = static_cast<uint32_t>(rIdx), o = static_cast<uint32_t>(oIdx);

    auto f = hbc::HBCFile::from_bytes(mixedSharedGroup(f0, r, o));
    REQUIRE(f.functions()[o].shared);
    REQUIRE(f.functions()[r].shared);
    REQUIRE_FALSE(f.functions()[o].hasExceptionHandler);
    const uint32_t frame = f.functions()[o].frameSize;
    auto ir = nopAt1(f, o);

    auto m = hbc::Module::from(f);
    m.set_function_body(o, ir, frame, /*editAll=*/true);
    auto g = reparse(m.serialize());

    for (uint32_t i : {r, o})
        REQUIRE(g.functions()[i].bytecodeSizeInBytes == f.functions()[i].bytecodeSizeInBytes + 3);
    REQUIRE(g.functions()[r].offset == g.functions()[o].offset);
    requireEhShifted(f.functions()[r].exceptionHandlers, g.functions()[r].exceptionHandlers);
    for (const auto& x : g.disasm(r)) REQUIRE(x.mnemonic != "???");
}

TEST_CASE("editAll refuses a frameSize bump if any member of the group has EH", "[patcher]") {
    auto f0 = openOrSkip(HBC_CORPUS_DIR "/try_catch_g0.hbc");
    int rIdx = -1, oIdx = -1;
    for (const auto& fn : f0.functions()) {
        if (fn.name == "risky") rIdx = static_cast<int>(fn.index);
        else if (!fn.hasExceptionHandler && !fn.overflowed && oIdx < 0) oIdx = static_cast<int>(fn.index);
    }
    REQUIRE(rIdx >= 0);
    REQUIRE(oIdx >= 0);
    const auto r = static_cast<uint32_t>(rIdx), o = static_cast<uint32_t>(oIdx);

    auto f = hbc::HBCFile::from_bytes(mixedSharedGroup(f0, r, o));
    const uint32_t frame = f.functions()[o].frameSize;
    auto ir = nopAt1(f, o);

    auto m = hbc::Module::from(f);
    // the opened function has no EH, but `risky` in the same group does:
    // the new frameSize would be written on it too
    REQUIRE_THROWS_WITH(m.set_function_body(o, ir, frame + 1, /*editAll=*/true),
                        Catch::Matchers::ContainsSubstring("exception handler"));
    // same frameSize: ok
    REQUIRE_NOTHROW(m.set_function_body(o, ir, frame, /*editAll=*/true));
}

TEST_CASE("editing a function with a lower offset than a lower-index one is local", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    const auto& funcs = f.functions();
    // find i, j with i < j but offset[j] < offset[i]  (non-monotonic layout)
    int hi = -1, lo = -1;
    for (uint32_t k = 1; k < funcs.size(); ++k)
        if (funcs[k].offset < funcs[k - 1].offset &&
            !funcs[k].overflowed && !funcs[k - 1].overflowed) {
            hi = static_cast<int>(k - 1);   // lower index, higher offset
            lo = static_cast<int>(k);       // higher index, lower offset
            break;
        }
    REQUIRE(lo >= 0);
    REQUIRE(funcs[static_cast<uint32_t>(lo)].offset < funcs[static_cast<uint32_t>(hi)].offset);

    auto irLo = buildIr(f.disasm(static_cast<uint32_t>(lo)));
    auto m = hbc::Module::from(f);
    m.set_function_body(static_cast<uint32_t>(lo), irLo,
                        funcs[static_cast<uint32_t>(lo)].frameSize,
                        funcs[static_cast<uint32_t>(lo)].shared);
    auto g = reparse(m.serialize());

    // function hi (higher offset) wasn't touched by the disasm
    auto a = f.disasm(static_cast<uint32_t>(hi));
    auto b = g.disasm(static_cast<uint32_t>(hi));
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i].mnemonic == b[i].mnemonic);
}

// =========================================================================
// add_string + multi-section rebuild
// =========================================================================

// Compares the disasm (mnemonic + operands) of every function between two files:
// add_string moves bodies as blocks, it doesn't resize them.
static void requireAllFunctionsUnchanged(hbc::HBCFile& a, hbc::HBCFile& b) {
    REQUIRE(a.functions().size() == b.functions().size());
    for (uint32_t i = 0; i < a.functions().size(); ++i) {
        auto x = a.disasm(i);
        auto y = b.disasm(i);
        REQUIRE(x.size() == y.size());
        for (size_t k = 0; k < x.size(); ++k) {
            REQUIRE(x[k].mnemonic == y[k].mnemonic);
            REQUIRE(x[k].operands.size() == y[k].operands.size());
            for (size_t o = 0; o < x[k].operands.size(); ++o)
                REQUIRE(opValue(x[k].operands[o]) == opValue(y[k].operands[o]));
        }
    }
}

TEST_CASE("Module identity holds on a file with literal buffers (align fix)", "[patcher]") {
    // medium.hbc has non-empty array/object buffers: before section_map's fix
    // (align 1 -> 4 on the buffers) the per-section re-layout lost the padding
    // before "array buffer" and shifted the bytecode by 4.
    auto f = openOrSkip(HBC_CORPUS_DIR "/medium.hbc");
    REQUIRE(f.header().arrayBufferSize + f.header().objKeyBufferSize +
            f.header().objValueBufferSize > 0);
    auto m = hbc::Module::from(f);
    REQUIRE(m.serialize() == f.serialize());
}

TEST_CASE("add_string appends a String and shifts everything after", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    const auto  h0     = f.header();
    const auto  n0     = h0.stringCount;
    const auto  store0 = h0.stringStorageSize;
    const auto  kinds0 = h0.stringKindCount;

    auto m   = hbc::Module::from(f);
    uint32_t idx = m.add_string("hi");
    REQUIRE(idx == n0);
    auto g = reparse(m.serialize());

    REQUIRE(g.header().stringCount        == n0 + 1);
    REQUIRE(g.header().stringStorageSize  == store0 + 2);
    REQUIRE(g.header().overflowStringCount == h0.overflowStringCount);
    // hello.hbc ends with an Identifier run -> a new String run is needed
    REQUIRE(g.header().stringKindCount    == kinds0 + 1);
    REQUIRE(g.header().fileLength         == m.serialize().size());
    REQUIRE(g.strings().at(idx) == "hi");
    REQUIRE(g.stringKinds().at(idx) == hbc::StringKind::String);
    for (uint32_t i = 0; i < n0; ++i) {
        REQUIRE(g.strings()[i]     == f.strings()[i]);
        REQUIRE(g.stringKinds()[i] == f.stringKinds()[i]);
    }
    requireAllFunctionsUnchanged(f, g);
}

TEST_CASE("add_string with >= 255 chars uses the overflow table", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    const std::string big(300, 'x');
    auto m = hbc::Module::from(f);
    uint32_t idx = m.add_string(big);
    auto g = reparse(m.serialize());

    REQUIRE(g.header().overflowStringCount == f.header().overflowStringCount + 1);
    REQUIRE(g.header().stringStorageSize   == f.header().stringStorageSize + 300);
    REQUIRE(g.strings().at(idx) == big);
}

TEST_CASE("two add_string calls get consecutive indices and one String run", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    const auto n0 = f.header().stringCount;
    auto m = hbc::Module::from(f);
    REQUIRE(m.add_string("aaa") == n0);
    REQUIRE(m.add_string("bbbb") == n0 + 1);
    auto g = reparse(m.serialize());

    REQUIRE(g.header().stringCount == n0 + 2);
    REQUIRE(g.strings().at(n0)     == "aaa");
    REQUIRE(g.strings().at(n0 + 1) == "bbbb");
    // two contiguous Strings -> only one run added
    REQUIRE(g.header().stringKindCount == f.header().stringKindCount + 1);
}

TEST_CASE("add_string on a real bundle with overflow functions", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    const auto n0 = f.header().stringCount;
    uint32_t ovFuncs0 = 0;
    for (const auto& fn : f.functions()) if (fn.overflowed) ++ovFuncs0;
    REQUIRE(ovFuncs0 > 0);

    auto m = hbc::Module::from(f);
    uint32_t idx = m.add_string("hermit-was-here");
    auto g = reparse(m.serialize());

    REQUIRE(g.header().stringCount == n0 + 1);
    REQUIRE(g.strings().at(idx) == "hermit-was-here");
    uint32_t ovFuncs1 = 0;
    for (const auto& fn : g.functions()) if (fn.overflowed) ++ovFuncs1;
    REQUIRE(ovFuncs1 == ovFuncs0);
    // a sample of functions (start / end / middle) disassembles with no ???
    for (uint32_t i : {0u, g.header().functionCount / 2, g.header().functionCount - 1}) {
        auto ins = g.disasm(i);
        for (const auto& x : ins) REQUIRE(x.mnemonic != "???");
    }
}

TEST_CASE("add_string rejects non-ASCII and empty", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_AS(m.add_string(""), std::invalid_argument);
    REQUIRE_THROWS_AS(m.add_string(std::string("caf\xc3\xa9")), std::invalid_argument);
}

TEST_CASE("add_string is not combinable with set_function_body", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    auto ir = buildIr(f.disasm(0));
    m.set_function_body(0, ir, f.functions()[0].frameSize);
    m.add_string("x");
    REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
}

// =========================================================================
// set_string_value (string length change, implicit detach)
// =========================================================================

TEST_CASE("set_string_value Case A grows a non-overflow entry", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    const auto h0 = f.header();
    const std::string newVal = "strict mode enabled";  // 20 B, was "use strict" (10 B)

    auto m = hbc::Module::from(f);
    m.set_string_value(4, newVal);
    auto g = reparse(m.serialize());
    const auto h1 = g.header();

    REQUIRE(h1.stringStorageSize  == h0.stringStorageSize + newVal.size());
    REQUIRE(h1.stringCount        == h0.stringCount);
    REQUIRE(h1.stringKindCount    == h0.stringKindCount);
    REQUIRE(h1.overflowStringCount == h0.overflowStringCount);
    REQUIRE(g.strings().at(4) == newVal);
    for (uint32_t i = 0; i < h0.stringCount; ++i)
        if (i != 4) REQUIRE(g.strings()[i] == f.strings()[i]);
    requireAllFunctionsUnchanged(f, g);
}

TEST_CASE("set_string_value Case A shrinks a non-overflow entry", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    const auto h0 = f.header();

    uint32_t storageStart = 0;
    for (const auto& s : f.section_map())
        if (s.name == "string storage") storageStart = s.offset;
    REQUIRE(storageStart != 0);

    auto m = hbc::Module::from(f);
    m.set_string_value(4, "ok");  // 2 B, was "use strict" (10 B)
    auto g = reparse(m.serialize());

    REQUIRE(g.header().stringStorageSize == h0.stringStorageSize + 2);
    REQUIRE(g.strings().at(4) == "ok");
    // Append-only: the new offset is past the end of the ORIGINAL storage
    // (never an in-place reuse of the old slot).
    REQUIRE(g.string_entries().at(4).storageOffset >= storageStart + h0.stringStorageSize);
}

TEST_CASE("set_string_value Case B rejects promotion to overflow", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    REQUIRE_FALSE(f.string_entries().at(4).isOverflow);   // "use strict", non-overflow
    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_WITH(m.set_string_value(4, std::string(300, 'x')),
                        Catch::Matchers::ContainsSubstring("overflow"));
}

TEST_CASE("set_string_value rejects a second call on the same Module", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    m.set_string_value(3, "abc");
    REQUIRE_THROWS_AS(m.set_string_value(4, "def"), std::runtime_error);
}

TEST_CASE("set_string_value is not combinable with add_string", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    m.set_string_value(3, "abc");
    m.add_string("newstr");
    REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
}

TEST_CASE("set_string_value is not combinable with set_function_body", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    m.set_string_value(3, "abc");
    auto ir = buildIr(f.disasm(0));
    m.set_function_body(0, ir, f.functions()[0].frameSize);
    REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
}

TEST_CASE("set_string_value Case C keeps an already-overflow entry overflow (real bundle)", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    const auto& entries0 = f.string_entries();
    REQUIRE(entries0.at(328).isOverflow);   // finding: CSS blob, 458 characters
    const uint32_t ovfCount0 = f.header().overflowStringCount;

    auto m = hbc::Module::from(f);
    m.set_string_value(328, "short css");   // 9 B, much shorter
    auto g = reparse(m.serialize());

    REQUIRE(g.header().overflowStringCount == ovfCount0);   // no slot change
    REQUIRE(g.string_entries().at(328).isOverflow);          // stays overflow (never demoted)
    REQUIRE(g.strings().at(328) == "short css");
    // the other overflow entries (including the remaining 646) are untouched.
    const auto& entries1 = g.string_entries();
    for (uint32_t i = 0; i < entries0.size(); ++i) {
        if (i == 328 || !entries0[i].isOverflow) continue;
        REQUIRE(entries1[i].storageOffset == entries0[i].storageOffset);
        REQUIRE(g.strings()[i] == f.strings()[i]);
    }
}

TEST_CASE("set_string_value Case A supports a real UTF-16 entry (real bundle)", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    const auto& e = f.string_entries().at(22928);
    REQUIRE(e.isUTF16);
    REQUIRE_FALSE(e.isOverflow);

    // "nouveau texte utf16" in UTF-16LE (BMP only, ASCII characters for simplicity).
    std::string newBytes;
    for (char c : std::string("nouveau texte utf16")) {
        newBytes.push_back(c);
        newBytes.push_back('\0');
    }
    auto m = hbc::Module::from(f);
    m.set_string_value(22928, newBytes);
    auto g = reparse(m.serialize());

    const auto& e1 = g.string_entries().at(22928);
    REQUIRE(e1.isUTF16);
    REQUIRE(e1.byteLength == newBytes.size());
    REQUIRE(g.strings().at(22928) == "nouveau texte utf16");
}

TEST_CASE("set_string_value rejects odd byte count on a UTF-16 entry", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    REQUIRE(f.string_entries().at(22928).isUTF16);
    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_AS(m.set_string_value(22928, "abc"), std::invalid_argument);
}

TEST_CASE("Module identity holds on ridemovi.hbc without set_string_value", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    auto m = hbc::Module::from(f);
    REQUIRE(m.serialize() == f.serialize());   // no regression on the add_string path
}

// ---------------------------------------------------------------------------
// set_literal_value: generalized value-edit + silent detach
// ---------------------------------------------------------------------------

TEST_CASE("set_literal_value grows the array buffer and repoints only the target site", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/literals_g0.hbc");
    const auto& sites = f.literal_sites();
    if (sites.size() != 1 || sites[0].isObject)
        SKIP("literals_g0.hbc: unexpected compiler layout");
    const auto site = sites[0];
    const auto h0 = f.header();
    const auto oldBuf = f.array_buffer();

    // [10,20,30] (Integer,Integer,Integer) -> [10,Null,30]: element 1
    // changes TAG (Integer -> Null, 0 value bytes) - RLE recomputed by
    // hand (no run can merge, the tag changes in the middle):
    //   Integer run=1 (10) | Null run=1 | Integer run=1 (30)
    static const uint8_t kNewPayload[] = {
        0x71, 0x0a, 0x00, 0x00, 0x00,
        0x01,
        0x71, 0x1e, 0x00, 0x00, 0x00,
    };
    const std::string newBytes(reinterpret_cast<const char*>(kNewPayload), sizeof(kNewPayload));

    auto m = hbc::Module::from(f);
    m.set_literal_value(site.functionIndex, site.instructionOffset, site.isObject, newBytes);
    auto g = reparse(m.serialize());

    REQUIRE(g.header().arrayBufferSize == h0.arrayBufferSize + newBytes.size());
    const auto newBuf = g.array_buffer();
    REQUIRE(newBuf.substr(0, oldBuf.size()) == oldBuf);    // append-only: prefix intact
    REQUIRE(newBuf.substr(oldBuf.size())    == newBytes);

    const auto& sites1 = g.literal_sites();
    REQUIRE(sites1.size() == 1);
    REQUIRE(sites1[0].bufferIdx == static_cast<uint32_t>(oldBuf.size()));  // repointed at the end
    REQUIRE(sites1[0].count     == site.count);                            // count unchanged
    // Only the edited instruction's bufferIdx operand changes: mnemonic,
    // offset and count stay the same (requireAllFunctionsUnchanged doesn't
    // apply here - it would also compare bufferIdx, which CHANGES by
    // design).
    REQUIRE(sites1[0].mnemonic          == site.mnemonic);
    REQUIRE(sites1[0].instructionOffset == site.instructionOffset);
}

TEST_CASE("set_literal_value rejects a function with shared bytecode (real bundle)", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    const auto& sites = f.literal_sites();
    const hbc::LiteralSite* shared = nullptr;
    for (const auto& s : sites)
        if (f.functions()[s.functionIndex].shared) { shared = &s; break; }
    if (!shared) SKIP("no site on a function with shared bytecode in ridemovi.hbc");

    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_WITH(
        m.set_literal_value(shared->functionIndex, shared->instructionOffset,
                            shared->isObject, std::string(1, '\0')),
        Catch::Matchers::ContainsSubstring("private clone"));
}

TEST_CASE("set_literal_value rejects a Short bufferIdx that would overflow (real bundle)", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    REQUIRE(f.header().arrayBufferSize > 0xFFFFu);   // precondition: already past 64 KB

    const auto& sites = f.literal_sites();
    const hbc::LiteralSite* target = nullptr;
    for (const auto& s : sites) {
        if (s.mnemonic != "NewArrayWithBuffer") continue;   // Short variant only
        if (f.functions()[s.functionIndex].shared) continue;
        target = &s;
        break;
    }
    REQUIRE(target != nullptr);

    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_WITH(
        m.set_literal_value(target->functionIndex, target->instructionOffset, false, std::string(4, '\0')),
        Catch::Matchers::ContainsSubstring("promotion"));
}

TEST_CASE("set_literal_value rejects an instruction that is not New{Array,Object}WithBuffer", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_AS(m.set_literal_value(0, 0, false, std::string(2, '\0')), std::out_of_range);
}

TEST_CASE("set_literal_value rejects a second call on the same Module", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/literals_g0.hbc");
    const auto& sites = f.literal_sites();
    if (sites.empty()) SKIP("literals_g0.hbc: no site");
    const auto site = sites[0];
    auto m = hbc::Module::from(f);
    m.set_literal_value(site.functionIndex, site.instructionOffset, site.isObject, std::string(1, '\0'));
    REQUIRE_THROWS_AS(
        m.set_literal_value(site.functionIndex, site.instructionOffset, site.isObject, std::string(1, '\0')),
        std::runtime_error);
}

TEST_CASE("set_literal_value is not combinable with add_string / set_string_value / set_function_body", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/literals_g0.hbc");
    const auto& sites = f.literal_sites();
    if (sites.empty()) SKIP("literals_g0.hbc: no site");
    const auto site = sites[0];

    {
        auto m = hbc::Module::from(f);
        m.set_literal_value(site.functionIndex, site.instructionOffset, site.isObject, std::string(1, '\0'));
        m.add_string("newstr");
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
    {
        auto m = hbc::Module::from(f);
        m.set_literal_value(site.functionIndex, site.instructionOffset, site.isObject, std::string(1, '\0'));
        m.set_string_value(0, "x");
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
    {
        auto m = hbc::Module::from(f);
        m.set_literal_value(site.functionIndex, site.instructionOffset, site.isObject, std::string(1, '\0'));
        auto ir = buildIr(f.disasm(0));
        m.set_function_body(0, ir, f.functions()[0].frameSize);
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
}

// ---------------------------------------------------------------------------
// resize_literal: add/remove ONLY at the end
// ---------------------------------------------------------------------------

TEST_CASE("resize_literal adds an element to a pure array literal", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/literals_g0.hbc");
    const auto& sites = f.literal_sites();
    if (sites.size() != 1 || sites[0].isObject)
        SKIP("literals_g0.hbc: unexpected compiler layout");
    const auto site = sites[0];
    const auto h0 = f.header();
    const auto oldBuf = f.array_buffer();

    // [10,20,30] + 999 -> Integer run(4): 0x74, then 4 int32 LE.
    static const uint8_t kNewPayload[] = {
        0x74,
        0x0a, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00,
        0x1e, 0x00, 0x00, 0x00,
        0xe7, 0x03, 0x00, 0x00,
    };
    const std::string newBytes(reinterpret_cast<const char*>(kNewPayload), sizeof(kNewPayload));

    auto m = hbc::Module::from(f);
    m.resize_literal(site.functionIndex, site.instructionOffset, site.isObject, /*isAdd=*/true, newBytes);
    auto g = reparse(m.serialize());

    REQUIRE(g.header().arrayBufferSize == h0.arrayBufferSize + newBytes.size());
    const auto newBuf = g.array_buffer();
    REQUIRE(newBuf.substr(0, oldBuf.size()) == oldBuf);   // append-only: prefix intact

    const auto& sites1 = g.literal_sites();
    REQUIRE(sites1.size() == 1);
    REQUIRE(sites1[0].count     == site.count + 1);
    REQUIRE(sites1[0].bufferIdx == static_cast<uint32_t>(oldBuf.size()));

    // sizeHint == count still holds afterwards (it already was == count before the edit).
    auto instrs = g.disasm(site.functionIndex);
    auto it = std::find_if(instrs.begin(), instrs.end(),
                           [&](const hbc::Instruction& x) { return x.offset == site.instructionOffset; });
    REQUIRE(it != instrs.end());
    REQUIRE(opValue(it->operands[1]) == site.count + 1);   // sizeHint
    REQUIRE(opValue(it->operands[2]) == site.count + 1);   // count
}

TEST_CASE("resize_literal removes the last element of a pure array literal", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/literals_g0.hbc");
    const auto& sites = f.literal_sites();
    if (sites.size() != 1 || sites[0].isObject)
        SKIP("literals_g0.hbc: unexpected compiler layout");
    const auto site = sites[0];

    // [10,20,30] without the last one -> [10,20]: Integer run(2): 0x72, then 2 int32.
    static const uint8_t kNewPayload[] = {
        0x72, 0x0a, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
    };
    const std::string newBytes(reinterpret_cast<const char*>(kNewPayload), sizeof(kNewPayload));

    auto m = hbc::Module::from(f);
    m.resize_literal(site.functionIndex, site.instructionOffset, site.isObject, /*isAdd=*/false, newBytes);
    auto g = reparse(m.serialize());

    const auto& sites1 = g.literal_sites();
    REQUIRE(sites1[0].count == site.count - 1);

    auto instrs = g.disasm(site.functionIndex);
    auto it = std::find_if(instrs.begin(), instrs.end(),
                           [&](const hbc::Instruction& x) { return x.offset == site.instructionOffset; });
    REQUIRE(it != instrs.end());
    REQUIRE(opValue(it->operands[1]) == site.count - 1);   // sizeHint
    REQUIRE(opValue(it->operands[2]) == site.count - 1);   // count
}

TEST_CASE("resize_literal adds a key-value pair to an object literal (both buffers)", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/medium.hbc");
    const auto& sites = f.literal_sites();
    const hbc::LiteralSite* site = nullptr;
    for (const auto& s : sites) if (s.isObject) { site = &s; break; }
    REQUIRE(site != nullptr);
    const auto h0 = f.header();
    const auto oldKeyBuf = f.obj_key_buffer();
    const auto oldValBuf = f.obj_value_buffer();

    // keys 12,14,15 (ByteString) + new key string:0 (ByteString) -> run(4): 0x64.
    static const uint8_t kNewKeys[] = { 0x64, 0x0c, 0x0e, 0x0f, 0x00 };
    // values 1,2,3 (Integer) + new 42 -> run(4): 0x74.
    static const uint8_t kNewVals[] = {
        0x74, 0x01,0,0,0, 0x02,0,0,0, 0x03,0,0,0, 0x2a,0,0,0,
    };
    const std::string newKeyBytes(reinterpret_cast<const char*>(kNewKeys), sizeof(kNewKeys));
    const std::string newValBytes(reinterpret_cast<const char*>(kNewVals), sizeof(kNewVals));

    auto m = hbc::Module::from(f);
    m.resize_literal(site->functionIndex, site->instructionOffset, /*isObject=*/true, /*isAdd=*/true,
                     newValBytes, newKeyBytes);
    auto g = reparse(m.serialize());

    REQUIRE(g.header().objKeyBufferSize   == h0.objKeyBufferSize   + newKeyBytes.size());
    REQUIRE(g.header().objValueBufferSize == h0.objValueBufferSize + newValBytes.size());
    REQUIRE(g.obj_key_buffer().substr(0, oldKeyBuf.size())   == oldKeyBuf);
    REQUIRE(g.obj_value_buffer().substr(0, oldValBuf.size()) == oldValBuf);

    const auto& sites1 = g.literal_sites();
    const hbc::LiteralSite* site1 = nullptr;
    for (const auto& s : sites1) if (s.isObject) { site1 = &s; break; }
    REQUIRE(site1 != nullptr);
    REQUIRE(site1->count        == site->count + 1);
    REQUIRE(site1->bufferIdx    == static_cast<uint32_t>(oldValBuf.size()));
    REQUIRE(site1->keyBufferIdx == static_cast<uint32_t>(oldKeyBuf.size()));
}

TEST_CASE("resize_literal rejects a function with shared bytecode (real bundle)", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    const auto& sites = f.literal_sites();
    const hbc::LiteralSite* shared = nullptr;
    for (const auto& s : sites)
        if (f.functions()[s.functionIndex].shared) { shared = &s; break; }
    if (!shared) SKIP("no site on a function with shared bytecode in ridemovi.hbc");

    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_WITH(
        m.resize_literal(shared->functionIndex, shared->instructionOffset, shared->isObject,
                         /*isAdd=*/true, std::string(4, '\0')),
        Catch::Matchers::ContainsSubstring("private clone"));
}

TEST_CASE("resize_literal rejects sizeHint != count (mixed literal, real bundle)", "[patcher]") {
    // func[311]@42 in ridemovi.hbc: NewObjectWithBuffer with sizeHint=11,
    // count=2 (mixed literal+dynamic object literal) - found by scanning
    // the corpus (see the implementation notes).
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    const auto& sites = f.literal_sites();
    const hbc::LiteralSite* mixed = nullptr;
    for (const auto& s : sites) {
        if (s.functionIndex != 311 || s.instructionOffset != 42) continue;
        mixed = &s;
        break;
    }
    if (!mixed) SKIP("func[311]@42 not found - corpus layout changed");
    REQUIRE_FALSE(f.functions()[mixed->functionIndex].shared);

    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_WITH(
        m.resize_literal(mixed->functionIndex, mixed->instructionOffset, mixed->isObject,
                         /*isAdd=*/true, std::string(4, '\0'), std::string(4, '\0')),
        Catch::Matchers::ContainsSubstring("sizeHint"));
}

TEST_CASE("resize_literal rejects removing from an empty site", "[patcher]") {
    // Removes the 3 elements of [10,20,30] one at a time (three separate
    // Module/serialize rounds, re-parsing each time) down to count==0, then
    // checks that a fourth remove raises out_of_range.
    auto f = openOrSkip(HBC_CORPUS_DIR "/literals_g0.hbc");
    const auto& sites0 = f.literal_sites();
    if (sites0.size() != 1 || sites0[0].isObject)
        SKIP("literals_g0.hbc: unexpected compiler layout");

    static const std::string kPayloads[] = {
        std::string("\x72\x0a\x00\x00\x00\x14\x00\x00\x00", 9),   // [10,20]
        std::string("\x71\x0a\x00\x00\x00", 5),                    // [10]
        std::string("", 0),                                        // []
    };

    std::optional<hbc::HBCFile> cur;
    const hbc::HBCFile* curPtr = &f;   // HBCFile isn't copyable: points at f or at *cur
    for (const std::string& payload : kPayloads) {
        const auto& sites = curPtr->literal_sites();
        REQUIRE(sites.size() == 1);
        const auto site = sites[0];
        auto m = hbc::Module::from(*curPtr);
        m.resize_literal(site.functionIndex, site.instructionOffset, site.isObject, false, payload);
        cur.emplace(reparse(m.serialize()));
        curPtr = &*cur;
    }
    REQUIRE(curPtr->literal_sites()[0].count == 0);

    const auto site0 = curPtr->literal_sites()[0];
    auto m = hbc::Module::from(*curPtr);
    REQUIRE_THROWS_AS(
        m.resize_literal(site0.functionIndex, site0.instructionOffset, site0.isObject,
                         false, std::string()),
        std::out_of_range);
}

TEST_CASE("resize_literal rejects a second call on the same Module", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/literals_g0.hbc");
    const auto& sites = f.literal_sites();
    if (sites.empty()) SKIP("literals_g0.hbc: no site");
    const auto site = sites[0];
    auto m = hbc::Module::from(f);
    m.resize_literal(site.functionIndex, site.instructionOffset, site.isObject, true, std::string(4, '\0'));
    REQUIRE_THROWS_AS(
        m.resize_literal(site.functionIndex, site.instructionOffset, site.isObject, true, std::string(4, '\0')),
        std::runtime_error);
}

TEST_CASE("resize_literal is not combinable with set_literal_value / add_string / set_string_value / set_function_body", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/literals_g0.hbc");
    const auto& sites = f.literal_sites();
    if (sites.empty()) SKIP("literals_g0.hbc: no site");
    const auto site = sites[0];

    {
        auto m = hbc::Module::from(f);
        m.resize_literal(site.functionIndex, site.instructionOffset, site.isObject, true, std::string(4, '\0'));
        m.set_literal_value(site.functionIndex, site.instructionOffset, site.isObject, std::string(1, '\0'));
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
    {
        auto m = hbc::Module::from(f);
        m.resize_literal(site.functionIndex, site.instructionOffset, site.isObject, true, std::string(4, '\0'));
        m.add_string("newstr");
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
    {
        auto m = hbc::Module::from(f);
        m.resize_literal(site.functionIndex, site.instructionOffset, site.isObject, true, std::string(4, '\0'));
        m.set_string_value(0, "x");
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
    {
        auto m = hbc::Module::from(f);
        m.resize_literal(site.functionIndex, site.instructionOffset, site.isObject, true, std::string(4, '\0'));
        auto ir = buildIr(f.disasm(0));
        m.set_function_body(0, ir, f.functions()[0].frameSize);
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
}

TEST_CASE("resize_literal rejects an instruction that is not New{Array,Object}WithBuffer", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_AS(
        m.resize_literal(0, 0, false, true, std::string(4, '\0')),
        std::out_of_range);
}

// ---------------------------------------------------------------------------
// add_function: adds a whole new function at the end of the function
// table. Fixed metadata (paramCount=1, functionName=0, strictMode=true,
// prohibitConstruct=true - see the comment in module.cpp), never EH/debug info.
// ---------------------------------------------------------------------------

static std::vector<hbc::AsmInstr> loadUndefinedRetIr() {
    hbc::AsmInstr load;
    load.opcode   = opcodeOf("LoadConstUndefined");
    load.operands = {0.0};
    hbc::AsmInstr ret;
    ret.opcode   = opcodeOf("Ret");
    ret.operands = {0.0};
    return {load, ret};
}

static uint8_t findUnusedOpcode() {
    auto tbl = hbc::opcode_table(96);
    std::vector<bool> used(256, false);
    for (const auto& d : tbl) used[d.opcode] = true;
    for (int i = 0; i < 256; ++i)
        if (!used[i]) return static_cast<uint8_t>(i);
    throw std::runtime_error("add_function test: no free opcode (unexpected)");
}

TEST_CASE("add_function appends a new function with fixed metadata", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    const uint32_t origCount = f.header().functionCount;
    const uint32_t origGlobal = f.header().globalCodeIndex;

    auto m = hbc::Module::from(f);
    uint32_t idx = m.add_function(loadUndefinedRetIr(), 1);
    REQUIRE(idx == origCount);   // always the last index

    auto out = m.serialize();
    auto g = reparse(out);
    REQUIRE(g.header().functionCount   == origCount + 1);
    REQUIRE(g.header().globalCodeIndex == origGlobal);
    REQUIRE(g.header().fileLength      == out.size());

    const auto& fi = g.functions()[idx];
    REQUIRE(fi.paramCount          == 1);
    REQUIRE(fi.frameSize           == 1);
    REQUIRE(fi.hasExceptionHandler == false);
    REQUIRE(fi.hasDebugInfo        == false);
    REQUIRE(fi.overflowed          == false);
    REQUIRE(fi.isStrictMode        == true);
    // prohibitInvoke = None (2): callable and constructible, like a plain
    // `function`. 0 would be ProhibitCall ("Class constructor invoked without
    // new" on every Call) - see the comment in module.cpp.
    REQUIRE(fi.prohibitCall        == false);
    REQUIRE(fi.prohibitConstruct   == false);
    const uint8_t newFlags = out[sizeof(hbc::HBCFileHeader) + idx * sizeof(hbc::SmallFuncHeader) + 15];
    REQUIRE((newFlags & 0x3) == hbc::kProhibitInvokeNone);
}

// flags bits 0-1 are the 2-bit ProhibitInvoke enum (0 = Call prohibited,
// 1 = Construct prohibited, 2 = None), not two independent bits. hermes v96
// emits 2 for a plain function and 1 for arrow / async / generator functions.
TEST_CASE("prohibitInvoke is decoded as a 2-bit enum", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/invoke_kinds.hbc");
    auto byName = [&](const char* n) -> const hbc::FunctionInfo& {
        for (const auto& fn : f.functions()) if (fn.name == n) return fn;
        FAIL("function not found: " << n);
        throw std::logic_error("unreachable");
    };
    for (const char* n : {"plainDecl", "shorthandMethod"}) {
        INFO(n);
        REQUIRE_FALSE(byName(n).prohibitCall);
        REQUIRE_FALSE(byName(n).prohibitConstruct);
    }
    for (const char* n : {"arrowFn", "asyncFn", "genFn"}) {
        INFO(n);
        REQUIRE_FALSE(byName(n).prohibitCall);
        REQUIRE(byName(n).prohibitConstruct);
    }
}

// Rewrites bits 0-1 of `name`'s flags on a raw image and re-parses it.
static hbc::HBCFile withProhibitInvoke(const hbc::HBCFile& f, const char* name, uint8_t v) {
    std::string bytes(reinterpret_cast<const char*>(f.raw_data()), f.raw_size());
    for (const auto& fn : f.functions())
        if (fn.name == name) {
            auto* fh = reinterpret_cast<hbc::SmallFuncHeader*>(bytes.data() + sizeof(hbc::HBCFileHeader));
            fh[fn.index].flags.prohibitInvoke = v;
        }
    return hbc::HBCFile::from_bytes(bytes);
}

TEST_CASE("prohibitInvoke values 0..3 map to prohibitCall / prohibitConstruct", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/invoke_kinds.hbc");
    struct { uint8_t v; bool call, ctor; } cases[] = {
        {0, true,  false},   // ProhibitCall
        {1, false, true},    // ProhibitConstruct
        {2, false, false},   // ProhibitNone
        {3, false, false},   // VM: `prohibitInvoke == isConstructCall` -> never prohibited
    };
    for (const auto& c : cases) {
        INFO("prohibitInvoke = " << int(c.v));
        auto g = withProhibitInvoke(f, "plainDecl", c.v);
        for (const auto& fn : g.functions())
            if (fn.name == "plainDecl") {
                REQUIRE(fn.prohibitCall      == c.call);
                REQUIRE(fn.prohibitConstruct == c.ctor);
            }
    }
}

TEST_CASE("add_function's new body disassembles back to the input IR", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    uint32_t idx = m.add_function(loadUndefinedRetIr(), 1);
    auto g = reparse(m.serialize());

    auto d = g.disasm(idx);
    REQUIRE(d.size() == 2);
    REQUIRE(d[0].mnemonic == "LoadConstUndefined");
    REQUIRE(d[1].mnemonic == "Ret");
    REQUIRE(static_cast<uint32_t>(d[0].operands[0].u32) == 0u);
    REQUIRE(static_cast<uint32_t>(d[1].operands[0].u32) == 0u);
}

TEST_CASE("add_function preserves every existing function's body (hello.hbc)", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    const uint32_t origCount = f.header().functionCount;

    auto m = hbc::Module::from(f);
    m.add_function(loadUndefinedRetIr(), 1);
    auto g = reparse(m.serialize());

    for (uint32_t i = 0; i < origCount; ++i) {
        auto x = f.disasm(i);
        auto y = g.disasm(i);
        REQUIRE(x.size() == y.size());
        for (size_t k = 0; k < x.size(); ++k) {
            REQUIRE(x[k].mnemonic == y[k].mnemonic);
            REQUIRE(x[k].operands.size() == y[k].operands.size());
            for (size_t o = 0; o < x[k].operands.size(); ++o)
                REQUIRE(opValue(x[k].operands[o]) == opValue(y[k].operands[o]));
        }
    }
}

TEST_CASE("add_function rejects an empty body", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_AS(m.add_function({}, 1), std::runtime_error);
}

TEST_CASE("add_function rejects frameSize > 127", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    REQUIRE_THROWS_WITH(m.add_function(loadUndefinedRetIr(), 128),
                        Catch::Matchers::ContainsSubstring("127"));
}

TEST_CASE("add_function rejects SwitchImm", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    hbc::AsmInstr sw;
    sw.opcode = opcodeOf("SwitchImm");
    REQUIRE_THROWS_AS(m.add_function({sw}, 1), std::runtime_error);
}

TEST_CASE("add_function rejects an unknown opcode", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    hbc::AsmInstr bad;
    bad.opcode = findUnusedOpcode();
    REQUIRE_THROWS_AS(m.add_function({bad}, 1), std::runtime_error);
}

TEST_CASE("add_function rejects bytecode size >= 32768 B", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);

    std::vector<hbc::AsmInstr> ir;
    ir.reserve(16385);
    hbc::AsmInstr load;
    load.opcode   = opcodeOf("LoadConstUndefined");
    load.operands = {0.0};
    for (int i = 0; i < 16384; ++i) ir.push_back(load);   // 16384 * 2 B = 32768 B
    hbc::AsmInstr ret;
    ret.opcode   = opcodeOf("Ret");
    ret.operands = {0.0};
    ir.push_back(ret);

    m.add_function(ir, 1);
    REQUIRE_THROWS_WITH(m.serialize(), Catch::Matchers::ContainsSubstring("32768"));
}

TEST_CASE("add_function rejects a second call on the same Module", "[patcher]") {
    auto f = hbc::HBCFile::open(HBC_CORPUS_DIR "/hello.hbc");
    auto m = hbc::Module::from(f);
    m.add_function(loadUndefinedRetIr(), 1);
    REQUIRE_THROWS_AS(m.add_function(loadUndefinedRetIr(), 1), std::runtime_error);
}

TEST_CASE("add_function is not combinable with set_function_body / add_string / "
         "set_string_value / set_literal_value / resize_literal", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/literals_g0.hbc");
    const auto& sites = f.literal_sites();
    if (sites.empty()) SKIP("literals_g0.hbc: no site");
    const auto site = sites[0];

    {
        auto m = hbc::Module::from(f);
        m.add_function(loadUndefinedRetIr(), 1);
        auto ir = buildIr(f.disasm(0));
        m.set_function_body(0, ir, f.functions()[0].frameSize);
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
    {
        auto m = hbc::Module::from(f);
        m.add_function(loadUndefinedRetIr(), 1);
        m.add_string("newstr");
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
    {
        auto m = hbc::Module::from(f);
        m.add_function(loadUndefinedRetIr(), 1);
        m.set_string_value(0, "x");
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
    {
        auto m = hbc::Module::from(f);
        m.add_function(loadUndefinedRetIr(), 1);
        m.set_literal_value(site.functionIndex, site.instructionOffset, site.isObject, std::string(1, '\0'));
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
    {
        auto m = hbc::Module::from(f);
        m.add_function(loadUndefinedRetIr(), 1);
        m.resize_literal(site.functionIndex, site.instructionOffset, site.isObject, true, std::string(4, '\0'));
        REQUIRE_THROWS_AS(m.serialize(), std::runtime_error);
    }
}

TEST_CASE("add_function on a real bundle (ridemovi.hbc)", "[patcher]") {
    auto f = openOrSkip(HBC_CORPUS_DIR "/ridemovi.hbc");
    const uint32_t origCount = f.header().functionCount;

    auto m = hbc::Module::from(f);
    uint32_t idx = m.add_function(loadUndefinedRetIr(), 1);
    REQUIRE(idx == origCount);
    auto g = reparse(m.serialize());

    REQUIRE(g.header().functionCount   == origCount + 1);
    REQUIRE(g.header().globalCodeIndex == f.header().globalCodeIndex);

    auto d = g.disasm(idx);
    REQUIRE(d.size() == 2);
    REQUIRE(d[0].mnemonic == "LoadConstUndefined");
    REQUIRE(d[1].mnemonic == "Ret");

    // A sample of original functions (a full scan over 37k functions isn't
    // practical in a test - same sampled approach as the other rebuild tests).
    for (uint32_t i : {0u, origCount - 1, f.header().globalCodeIndex, origCount / 2}) {
        auto x = f.disasm(i);
        auto y = g.disasm(i);
        REQUIRE(x.size() == y.size());
        for (size_t k = 0; k < x.size(); ++k)
            REQUIRE(x[k].mnemonic == y[k].mnemonic);
    }
}
