#include "hbc/module.h"

#include "hbc/format.h"
#include "format/opcodes.h"     // OpcodeInfo, getOpcodeTable, operandSize, JumpKind
#include "disasm/disasm.h"      // disassemble()
#include "util/sha1.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace hbc {

AssembledFunction assemble_function(uint32_t version, std::vector<AsmInstr> ir) {
    const OpcodeInfo* table = getOpcodeTable(version);
    if (!table)
        throw std::invalid_argument("assemble_function: unsupported HBC version: " +
                                    std::to_string(version));

    auto instrSize = [&](const AsmInstr& a) -> uint32_t {
        const OpcodeInfo& info = table[a.opcode];
        uint32_t sz = 1;
        for (uint8_t k = 0; k < info.numOperands; ++k)
            sz += operandSize(info.operands[k]);
        return sz;
    };

    // Opcode of the Long variant (Addr32) of a short jump, or -1.
    auto longVariant = [&](uint8_t op, int8_t addrPos) -> int {
        if (op == 0xFF || addrPos < 0) return -1;
        const OpcodeInfo& s = table[op];
        const OpcodeInfo& l = table[op + 1];
        if (!s.mnemonic || !l.mnemonic) return -1;
        if (std::string(s.mnemonic) + "Long" != l.mnemonic) return -1;
        if (addrPos >= l.numOperands) return -1;
        if (l.operands[addrPos] != Operand::Type::Addr32) return -1;
        return static_cast<int>(op) + 1;
    };

    const size_t n = ir.size();
    std::vector<uint32_t> off(n + 1, 0);

    // Fixpoint: assign offsets, promote short jumps whose displacement doesn't fit Addr8.
    bool changed = true;
    int guard = 0;
    while (changed) {
        if (++guard > static_cast<int>(n) + 4)
            throw std::runtime_error("assemble_function: relaxation did not converge");
        changed = false;
        for (size_t i = 0; i < n; ++i)
            off[i + 1] = off[i] + instrSize(ir[i]);

        for (size_t i = 0; i < n; ++i) {
            AsmInstr& a = ir[i];
            if (a.jumpTargetIdx < 0) continue;
            const uint32_t tgt = static_cast<uint32_t>(a.jumpTargetIdx);
            if (tgt > n)
                throw std::out_of_range("assemble_function: jump target index out of range");
            const int64_t disp =
                static_cast<int64_t>(off[tgt]) - static_cast<int64_t>(off[i]);

            const OpcodeInfo& info = table[a.opcode];
            const bool isShort =
                a.addrOperandPos >= 0 && a.addrOperandPos < info.numOperands &&
                info.operands[a.addrOperandPos] == Operand::Type::Addr8;

            if (isShort && (disp < -128 || disp > 127)) {
                const int lv = longVariant(a.opcode, a.addrOperandPos);
                if (lv < 0)
                    throw std::runtime_error(
                        "assemble_function: short jump has no Long variant (opcode " +
                        std::to_string(a.opcode) + ")");
                a.opcode = static_cast<uint8_t>(lv);
                changed = true;
            }
        }
    }

    // Fill in the final displacements.
    for (size_t i = 0; i < n; ++i) {
        AsmInstr& a = ir[i];
        if (a.jumpTargetIdx < 0) continue;
        const uint32_t tgt = static_cast<uint32_t>(a.jumpTargetIdx);
        a.operands.at(static_cast<size_t>(a.addrOperandPos)) =
            static_cast<double>(static_cast<int64_t>(off[tgt]) - static_cast<int64_t>(off[i]));
    }

    // Emit.
    AssembledFunction result;
    result.offsets.resize(n);
    for (size_t i = 0; i < n; ++i) {
        result.offsets[i] = static_cast<uint32_t>(result.bytecode.size());
        std::vector<uint8_t> b = encode_instruction(version, ir[i].opcode, ir[i].operands);
        result.bytecode.insert(result.bytecode.end(), b.begin(), b.end());
    }
    return result;
}


namespace {

uint32_t align4(uint32_t x) { return (x + 3u) & ~3u; }

void appendU32(std::vector<uint8_t>& v, uint32_t x) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), b, b + 4);
}

// SmallStringTableEntry as a raw uint32 (bitfields LSB-first): isUTF16:1 | offset:23 | length:8.
uint32_t packSmallString(bool isUTF16, uint32_t offset, uint32_t length) {
    return (isUTF16 ? 1u : 0u) | ((offset & 0x7FFFFFu) << 1) | ((length & 0xFFu) << 24);
}

// SmallFuncHeader of the function appended by add_function().
std::vector<uint8_t> buildNewFunctionHeader(uint32_t offset, uint32_t infoOffset, uint32_t bodySize,
                                            uint32_t paramCount, uint32_t nameId,
                                            uint32_t frameSize, bool strictMode) {
    SmallFuncHeader fh{};
    fh.offset                 = offset;
    fh.paramCount             = paramCount;
    fh.bytecodeSizeInBytes    = bodySize;
    fh.functionName           = nameId;
    fh.infoOffset             = infoOffset;
    fh.frameSize              = frameSize;
    fh.environmentSize        = 0;
    fh.highestReadCacheIndex  = 0;
    fh.highestWriteCacheIndex = 0;
    // prohibitInvoke must be None (callable and constructible, like a plain
    // `function`), not 0: 0 is ProhibitCall (class constructor), and a Call on
    // the new function would fail at runtime with "Class constructor invoked
    // without new".
    fh.flags = FunctionFlags{};
    fh.flags.prohibitInvoke = kProhibitInvokeNone;
    fh.flags.strictMode     = strictMode;

    std::vector<uint8_t> bytes(sizeof(SmallFuncHeader));
    std::memcpy(bytes.data(), &fh, sizeof(SmallFuncHeader));
    return bytes;
}

}  // namespace

// -----------------------------------------------------------------------------
// Module::from
// -----------------------------------------------------------------------------

Module Module::from(const HBCFile& file) {
    Module m;
    m.version_ = file.version();

    const uint8_t* raw = file.raw_data();
    const size_t rawSize = file.raw_size();
    const HBCFileHeader& h = file.header();
    const uint32_t count = h.functionCount;
    if (count == 0)
        throw std::runtime_error("Module: file has no functions");

    m.header_.assign(raw, raw + sizeof(HBCFileHeader));

    const uint8_t* fhBase = raw + sizeof(HBCFileHeader);
    m.funcHeaders_.assign(fhBase, fhBase + count * sizeof(SmallFuncHeader));
    m.origStringCount_ = h.stringCount;

    const auto* sfh = reinterpret_cast<const SmallFuncHeader*>(fhBase);
    const std::vector<FunctionInfo>& funcs = file.functions();

    m.funcs_.resize(count);
    uint32_t blobStart = UINT32_MAX;
    uint32_t blobEnd   = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const FunctionInfo& fi = funcs[i];        // overflow already resolved to real values
        Module::FuncState& fs = m.funcs_[i];
        fs.origOffset     = fi.offset;
        fs.origSize       = fi.bytecodeSizeInBytes;
        fs.origInfoOffset = fi.infoOffset;         // overflow: largeOff + 32
        fs.origFrameSize  = fi.frameSize;
        fs.hasEH          = fi.hasExceptionHandler;
        fs.hasDebugInfo   = fi.hasDebugInfo;
        fs.overflowed     = sfh[i].flags.overflowed;
        if (fs.overflowed)
            fs.origLargeOff = (sfh[i].offset & 0xFFFFu) | ((sfh[i].infoOffset & 0xFFFFu) << 16);

        if (fi.offset + fi.bytecodeSizeInBytes > rawSize)
            throw std::runtime_error("Module: function " + std::to_string(i) +
                                     " bytecode is out of file bounds");
        blobStart = std::min(blobStart, fi.offset);
        blobEnd   = std::max(blobEnd,   fi.offset + fi.bytecodeSizeInBytes);
    }

    const uint32_t feEnd = sizeof(HBCFileHeader) + count * sizeof(SmallFuncHeader);
    if (blobStart < feEnd)
        throw std::runtime_error("Module: first bytecode offset is inconsistent with the layout");
    m.blobStart_     = blobStart;
    m.funcTableEnd_  = feEnd;

    // The sections right after the function table are multiples of 4 bytes
    // and 4-aligned, so they follow each other without padding.
    uint32_t p = feEnd;
    auto slice = [&](uint32_t nbytes) {
        std::vector<uint8_t> v(raw + p, raw + p + nbytes);
        p += nbytes;
        return v;
    };
    m.kindTable_     = slice(h.stringKindCount     * 4);
    m.idHash_        = slice(h.identifierCount     * 4);
    m.stringTable_   = slice(h.stringCount         * 4);
    m.overflowTable_ = slice(h.overflowStringCount * 8);
    m.stringStorage_ = slice(h.stringStorageSize);

    // Middle sections: size and alignment from section_map(). The padding
    // before the bytecode is not captured, serialize() regenerates it.
    static const char* kMidNames[] = {
        "array buffer", "object key buffer", "object value buffer",
        "bigint table", "bigint storage", "regexp table", "regexp storage",
        "cjs module table", "function source table",
    };
    for (const Section& s : file.section_map()) {
        for (const char* name : kMidNames) {
            if (s.name == name) {
                Module::MidSection ms;
                ms.name  = s.name;
                ms.align = s.align;
                ms.bytes.assign(raw + s.offset, raw + s.offset + s.size);
                m.midSections_.push_back(std::move(ms));
                break;
            }
        }
    }

    // Bodies kept verbatim, with no reordering: that preserves bodies out of
    // index order and shared bodies.
    m.bytecodeBlob_.assign(raw + blobStart, raw + blobEnd);

    m.oldInfoStart_       = align4(blobEnd);
    m.oldDebugInfoOffset_ = h.debugInfoOffset;

    if (h.debugInfoOffset < m.oldInfoStart_ || h.debugInfoOffset + 20 > rawSize)
        throw std::runtime_error("Module: header.debugInfoOffset is inconsistent with the layout");

    // Info blob: per-function overflow FunctionHeader + EH table + debug
    // offsets, between the end of the bytecode and debugInfoOffset.
    m.infoBlob_.assign(raw + m.oldInfoStart_, raw + h.debugInfoOffset);
    m.debugBlob_.assign(raw + h.debugInfoOffset, raw + rawSize - 20);

    // Shared bytecode groups: same origOffset.
    std::unordered_map<uint32_t, std::vector<uint32_t>> byOff;
    for (uint32_t i = 0; i < count; ++i)
        byOff[m.funcs_[i].origOffset].push_back(i);
    for (uint32_t i = 0; i < count; ++i) {
        auto& grp = byOff[m.funcs_[i].origOffset];
        if (grp.size() > 1) {
            m.funcs_[i].sharedBody = true;
            m.funcs_[i].shareGroup = grp;
        }
    }

    return m;
}

// -----------------------------------------------------------------------------
// Edit setters
// -----------------------------------------------------------------------------

void Module::set_function_body(uint32_t funcIdx, std::vector<AsmInstr> ir,
                               uint32_t frameSize, bool editAll) {
    if (funcIdx >= funcs_.size())
        throw std::out_of_range("Module::set_function_body: function index");
    if (ir.empty())
        throw std::runtime_error("Module::set_function_body: empty body");

    FuncState& fsRef = funcs_[funcIdx];

    // SmallFuncHeader.frameSize is 7 bits; overflowed functions have a 32-bit field.
    if (!fsRef.overflowed && frameSize > 127)
        throw std::runtime_error("Module::set_function_body: frameSize " +
                                 std::to_string(frameSize) + " > 127 (7-bit field); "
                                 "would need promotion to overflow (not supported)");

    // Shared bytecode without editAll would need a private clone.
    if (fsRef.sharedBody && !editAll)
        throw std::runtime_error(
            "Module: function " + std::to_string(funcIdx) + " shares its "
            "bytecode with " + std::to_string(fsRef.shareGroup.size()) +
            " functions - pass editAll=true to modify them all "
            "(a private clone of a single function is not supported)");

    // Growing frameSize on a function with an exception handler crashes the
    // Hermes runtime (SIGSEGV on the throw path). With editAll the new
    // frameSize goes to every member of the group, so the check covers all of them.
    if (frameSize > fsRef.origFrameSize) {
        int64_t ehIdx = fsRef.hasEH ? static_cast<int64_t>(funcIdx) : -1;
        if (ehIdx < 0 && editAll && fsRef.sharedBody)
            for (uint32_t g : fsRef.shareGroup)
                if (funcs_[g].hasEH) { ehIdx = g; break; }
        if (ehIdx >= 0)
            throw std::runtime_error(
                "Module: cannot increase frameSize on a function with an exception "
                "handler (function " + std::to_string(ehIdx) + ") - the inserted "
                "instruction must use only registers < " +
                std::to_string(fsRef.origFrameSize));
    }

    const OpcodeInfo* table = getOpcodeTable(version_);
    if (!table)
        throw std::invalid_argument("Module: unsupported version");

    // The original function must not contain constructs we can't relocate.
    const size_t bodyPos = fsRef.origOffset - blobStart_;
    std::vector<Instruction> orig = disassemble(
        bytecodeBlob_.data() + bodyPos, fsRef.origSize, version_, {}, {});
    for (const Instruction& x : orig) {
        if (x.mnemonic == "SwitchImm")
            throw std::runtime_error("Module: the function contains SwitchImm - not supported");
        if (x.mnemonic.rfind("SaveGenerator", 0) == 0)
            throw std::runtime_error("Module: the function is a generator (SaveGenerator) - not supported");
        if (x.mnemonic == "???")
            throw std::runtime_error("Module: unknown opcode in the function - not supported");
    }
    for (const AsmInstr& a : ir) {
        const OpcodeInfo& info = table[a.opcode];
        if (!info.mnemonic)
            throw std::runtime_error("Module: opcode " + std::to_string(a.opcode) + " unknown");
        if (info.jumpKind == JumpKind::Switch)
            throw std::runtime_error("Module: inserting SwitchImm is not supported");
    }

    fsRef.dirty        = true;
    fsRef.editAll      = editAll && fsRef.sharedBody;
    fsRef.newIr        = std::move(ir);
    fsRef.newFrameSize = frameSize;
}

uint32_t Module::add_string(const std::string& value) {
    if (value.empty())
        throw std::invalid_argument("Module::add_string: empty string");
    for (unsigned char c : value)
        if (c >= 0x80)
            throw std::invalid_argument(
                "Module::add_string: non-ASCII character - CESU-8 / UTF-16 encoder "
                "not supported");
    pendingStrings_.push_back(value);
    return origStringCount_ + static_cast<uint32_t>(pendingStrings_.size()) - 1;
}

void Module::set_string_value(uint32_t idx, const std::string& newBytes) {
    if (idx >= origStringCount_)
        throw std::out_of_range("Module::set_string_value: string index");
    if (pendingStringEdit_)
        throw std::runtime_error(
            "Module::set_string_value: a length edit is already queued - "
            "only one edit per serialize (two separate commits)");

    SmallStringTableEntry e;
    std::memcpy(&e, stringTable_.data() + idx * 4, 4);
    const bool isUTF16     = e.isUTF16;
    const bool wasOverflow = (e.length == 255);

    if (isUTF16 && (newBytes.size() % 2 != 0))
        throw std::invalid_argument(
            "Module::set_string_value: odd newBytes size on a UTF-16 entry");
    const uint32_t newCharLen = isUTF16
        ? static_cast<uint32_t>(newBytes.size() / 2)
        : static_cast<uint32_t>(newBytes.size());

    if (!wasOverflow && newCharLen >= 255)
        throw std::runtime_error(
            "Module::set_string_value: promotion to overflow not supported");

    // Overflow slot: the small entry's `offset` is the index into the overflow
    // table (Hermes' BytecodeDataProvider::getStringTableEntry).
    uint32_t ovfSlot = 0;
    if (wasOverflow) {
        ovfSlot = e.offset;
        if (ovfSlot >= overflowTable_.size() / 8)
            throw std::runtime_error(
                "Module::set_string_value: overflow index out of range");
        // The edit rewrites the slot in place, so a slot used by several entries
        // is refused.
        const uint32_t count = static_cast<uint32_t>(stringTable_.size() / 4);
        for (uint32_t j = 0; j < count; ++j) {
            if (j == idx) continue;
            SmallStringTableEntry ej;
            std::memcpy(&ej, stringTable_.data() + j * 4, 4);
            if (ej.length == 255 && ej.offset == ovfSlot)
                throw std::runtime_error(
                    "Module::set_string_value: overflow slot shared by several entries "
                    "- not supported");
        }
    }

    pendingStringEdit_ = PendingStringEdit{idx, newBytes, isUTF16, wasOverflow, ovfSlot};
}

Module::LiteralTarget Module::locateLiteralSite(
    const char* who, const char* action, uint32_t funcIdx, uint32_t instrOffset,
    bool isObject, uint32_t maxOperandIdx) const
{
    const FuncState& fs = funcs_[funcIdx];

    // The operands live INSIDE the instruction, so an in-place edit would touch
    // every function of a shared group.
    if (fs.sharedBody)
        throw std::runtime_error(
            "Module: function " + std::to_string(funcIdx) + " shares its "
            "bytecode with " + std::to_string(fs.shareGroup.size()) + " functions - " +
            action + " would touch every function in the group "
            "(private clone not supported)");

    // Re-disassemble from bytecodeBlob_ (the same source set_function_body uses)
    // instead of trusting offsets computed by the caller.
    const size_t bodyPos = fs.origOffset - blobStart_;
    const std::vector<Instruction> instrs = disassemble(
        bytecodeBlob_.data() + bodyPos, fs.origSize, version_, {}, {});

    const Instruction* target = nullptr;
    for (const Instruction& x : instrs)
        if (x.offset == instrOffset) { target = &x; break; }
    if (!target)
        throw std::out_of_range(
            std::string(who) + ": no instruction at instrOffset " +
            std::to_string(instrOffset) + " in function " + std::to_string(funcIdx));

    const bool isArrMnem = target->mnemonic == "NewArrayWithBuffer" ||
                           target->mnemonic == "NewArrayWithBufferLong";
    const bool isObjMnem = target->mnemonic == "NewObjectWithBuffer" ||
                           target->mnemonic == "NewObjectWithBufferLong";
    if (isObject ? !isObjMnem : !isArrMnem)
        throw std::out_of_range(
            std::string(who) + ": instruction at instrOffset " +
            std::to_string(instrOffset) + " is '" + target->mnemonic +
            "', expected " + (isObject ? "NewObjectWithBuffer[Long]" : "NewArrayWithBuffer[Long]"));

    const OpcodeInfo* table = getOpcodeTable(version_);
    if (!table)
        throw std::invalid_argument(std::string(who) + ": unsupported version");
    const OpcodeInfo& info = table[target->opcode];
    if (info.numOperands <= maxOperandIdx)
        throw std::out_of_range(
            std::string(who) + ": unexpected opcode signature for '" + target->mnemonic + "'");

    LiteralTarget lt;
    lt.instr = *target;
    uint32_t pos = static_cast<uint32_t>(bodyPos) + target->offset + 1;  // +1 = opcode byte
    for (uint8_t i = 0; i < info.numOperands; ++i) {
        lt.operandBytePos.push_back(pos);
        lt.operandWidth.push_back(operandSize(info.operands[i]));
        pos += lt.operandWidth.back();
    }
    return lt;
}

uint32_t Module::newBufferOffset(const char* who, const char* midName, uint32_t operandWidth) const {
    const MidSection* section = nullptr;
    for (const MidSection& ms : midSections_)
        if (ms.name == midName) { section = &ms; break; }
    if (!section)
        throw std::runtime_error(std::string(who) + ": section '" + midName + "' missing");

    const uint32_t offRel = static_cast<uint32_t>(section->bytes.size());
    const uint64_t maxForWidth = (operandWidth >= 4)
        ? 0xFFFFFFFFull : ((1ull << (operandWidth * 8)) - 1);
    if (offRel > maxForWidth)
        throw std::runtime_error(
            std::string(who) + ": the new offset " + std::to_string(offRel) +
            " doesn't fit the operand at " + std::to_string(operandWidth * 8) +
            " bits - would need the *Long variant (promotion not supported)");
    return offRel;
}

// Operands of New{Array,Object}WithBuffer: dst(0), sizeHint(1), count(2), then
// bufferIdx(3) for arrays or keyBufferIdx(3) + valBufferIdx(4) for objects.
// "bufferIdx" below is the value buffer (same mapping as computeLiteralSites).

void Module::set_literal_value(uint32_t funcIdx, uint32_t instrOffset,
                               bool isObject, const std::string& newBufferBytes) {
    constexpr const char* who = "Module::set_literal_value";
    if (funcIdx >= funcs_.size())
        throw std::out_of_range(std::string(who) + ": function index");
    if (pendingLiteralEdit_)
        throw std::runtime_error(
            std::string(who) + ": a literal edit is already queued - "
            "only one edit per serialize (two separate commits)");

    const uint32_t bufIdx = isObject ? 4 : 3;
    const LiteralTarget t = locateLiteralSite(who, "editing a literal", funcIdx, instrOffset,
                                              isObject, bufIdx);

    const char* midName = isObject ? "object value buffer" : "array buffer";
    PendingLiteralEdit pl;
    pl.operandBytePos = t.operandBytePos[bufIdx];
    pl.operandWidth   = t.operandWidth[bufIdx];
    pl.midSectionName = midName;
    pl.newBufferBytes = newBufferBytes;
    pl.offRel         = newBufferOffset(who, midName, pl.operandWidth);
    pendingLiteralEdit_ = std::move(pl);
}

void Module::resize_literal(uint32_t funcIdx, uint32_t instrOffset, bool isObject,
                            bool isAdd, const std::string& newBufferBytes,
                            const std::string& newKeyBufferBytes) {
    constexpr const char* who = "Module::resize_literal";
    if (funcIdx >= funcs_.size())
        throw std::out_of_range(std::string(who) + ": function index");
    if (pendingLiteralEdit_ || pendingLiteralResize_)
        throw std::runtime_error(
            std::string(who) + ": a literal edit is already queued - "
            "only one edit per serialize (two separate commits)");

    const uint32_t valueIdx = isObject ? 4 : 3;
    const LiteralTarget t = locateLiteralSite(who, "adding/removing an element", funcIdx,
                                              instrOffset, isObject, valueIdx);

    const uint32_t origSizeHint = t.instr.operands[1].u32;
    const uint32_t origCount    = t.instr.operands[2].u32;
    if (origSizeHint != origCount)
        throw std::runtime_error(
            std::string(who) + ": sizeHint (" + std::to_string(origSizeHint) +
            ") != count (" + std::to_string(origCount) + ") - mixed literal+dynamic "
            "literal, out of scope (resize operates only on pure literals)");
    if (!isAdd && origCount == 0)
        throw std::out_of_range(std::string(who) + ": count is already 0, nothing to remove");

    const int64_t newCount64 = static_cast<int64_t>(origCount) + (isAdd ? 1 : -1);
    if (newCount64 > 0xFFFF)
        throw std::runtime_error(
            std::string(who) + ": count " + std::to_string(newCount64) +
            " exceeds 65535 - no *Long variant exists for the count operand "
            "(impossible, not just unimplemented)");

    PendingLiteralResize pr;
    pr.countOperandBytePos    = t.operandBytePos[2];
    pr.sizeHintOperandBytePos = t.operandBytePos[1];
    pr.newCount               = static_cast<uint32_t>(newCount64);
    pr.isObject               = isObject;
    pr.newBufferBytes         = newBufferBytes;

    pr.valueMidSectionName  = isObject ? "object value buffer" : "array buffer";
    pr.valueOperandBytePos  = t.operandBytePos[valueIdx];
    pr.valueOperandWidth    = t.operandWidth[valueIdx];
    pr.valueOffRel          = newBufferOffset(who, pr.valueMidSectionName.c_str(), pr.valueOperandWidth);

    if (isObject) {
        pr.newKeyBufferBytes = newKeyBufferBytes;
        pr.keyOperandBytePos = t.operandBytePos[3];
        pr.keyOperandWidth   = t.operandWidth[3];
        pr.keyOffRel         = newBufferOffset(who, "object key buffer", pr.keyOperandWidth);
    }

    pendingLiteralResize_ = std::move(pr);
}

uint32_t Module::add_function(std::vector<AsmInstr> ir, uint32_t frameSize) {
    if (ir.empty())
        throw std::runtime_error("Module::add_function: empty body");
    if (frameSize > 127)
        throw std::runtime_error("Module::add_function: frameSize " +
                                 std::to_string(frameSize) + " > 127 (7-bit field) - "
                                 "would need promotion to overflow (not supported)");
    if (pendingNewFunction_)
        throw std::runtime_error(
            "Module::add_function: an add_function is already queued - only one "
            "add_function per serialize (two separate commits)");

    const OpcodeInfo* table = getOpcodeTable(version_);
    if (!table)
        throw std::invalid_argument("Module::add_function: unsupported version");
    for (const AsmInstr& a : ir) {
        const OpcodeInfo& info = table[a.opcode];
        if (!info.mnemonic)
            throw std::runtime_error("Module::add_function: opcode " +
                                     std::to_string(a.opcode) + " unknown");
        if (info.jumpKind == JumpKind::Switch)
            throw std::runtime_error("Module::add_function: SwitchImm not supported");
    }

    PendingFunction pf;
    pf.ir        = std::move(ir);
    pf.frameSize = frameSize;
    pendingNewFunction_ = std::move(pf);
    return static_cast<uint32_t>(funcs_.size());
}

// -----------------------------------------------------------------------------
// serialize
// -----------------------------------------------------------------------------

// String sections after add_string / set_string_value. Both only APPEND to the
// storage, so no existing offset moves.
Module::StringSections Module::buildStringSections() const {
    StringSections ss;
    ss.kindTable     = kindTable_;
    ss.stringTable   = stringTable_;
    ss.overflowTable = overflowTable_;
    ss.storage       = stringStorage_;
    ss.stringCount   = origStringCount_;
    ss.kindCount     = static_cast<uint32_t>(kindTable_.size() / 4);
    ss.overflowCount = static_cast<uint32_t>(overflowTable_.size() / 8);

    const uint32_t k = static_cast<uint32_t>(pendingStrings_.size());
    if (k > 0) {
        uint32_t addedOverflow = 0;
        for (const std::string& s : pendingStrings_) {
            const uint32_t offRel = static_cast<uint32_t>(ss.storage.size());
            const uint32_t len    = static_cast<uint32_t>(s.size());
            ss.storage.insert(ss.storage.end(), s.begin(), s.end());

            if (len < 255) {
                if (offRel >= (1u << 23))
                    throw std::runtime_error("Module: string storage beyond 8 MB - not handled");
                appendU32(ss.stringTable, packSmallString(false, offRel, len));
            } else {
                // Sentinel length == 255; `offset` is the index of the new slot in
                // the overflow table (existing slots + those added so far), which
                // is where the VM reads it: overflowTable[small.offset].
                appendU32(ss.stringTable,
                          packSmallString(false, ss.overflowCount + addedOverflow, 255));
                appendU32(ss.overflowTable, offRel);     // OverflowStringTableEntry
                appendU32(ss.overflowTable, len);
                ++addedOverflow;
            }
        }

        // Kind table is RLE: the new strings are all kind String and
        // contiguous at the end, so extend the last run if it is String,
        // otherwise start a new run.
        bool extended = false;
        if (!ss.kindTable.empty()) {
            uint32_t last;
            std::memcpy(&last, ss.kindTable.data() + ss.kindTable.size() - 4, 4);
            if ((last >> 31) == 0) {
                const uint32_t merged = ((last & 0x7FFFFFFFu) + k) & 0x7FFFFFFFu;
                std::memcpy(ss.kindTable.data() + ss.kindTable.size() - 4, &merged, 4);
                extended = true;
            }
        }
        if (!extended) {
            appendU32(ss.kindTable, k & 0x7FFFFFFFu);
            ++ss.kindCount;
        }

        ss.stringCount   += k;
        ss.overflowCount += addedOverflow;
    }

    // Always appended, even at unchanged length: never overwriting is what
    // detaches a shared entry for free. Only entry idx is repointed, so the
    // slot counts don't change.
    if (pendingStringEdit_) {
        const PendingStringEdit& pe = *pendingStringEdit_;
        const uint32_t offRel = static_cast<uint32_t>(ss.storage.size());
        ss.storage.insert(ss.storage.end(), pe.newBytes.begin(), pe.newBytes.end());
        const uint32_t newCharLen = pe.isUTF16
            ? static_cast<uint32_t>(pe.newBytes.size() / 2)
            : static_cast<uint32_t>(pe.newBytes.size());

        if (!pe.wasOverflow) {
            if (offRel >= (1u << 23))
                throw std::runtime_error("Module: string storage beyond 8 MB - not handled");
            const uint32_t packed = packSmallString(pe.isUTF16, offRel, newCharLen);
            std::memcpy(ss.stringTable.data() + pe.idx * 4, &packed, 4);
        } else {
            // Already overflow: stays overflow whatever the new length. The
            // sentinel in the string table doesn't change; only the 8 bytes of
            // its overflow slot (a full uint32 offset, no 2^23 limit).
            std::memcpy(ss.overflowTable.data() + pe.ovfSlot * 8 + 0, &offRel,     4);
            std::memcpy(ss.overflowTable.data() + pe.ovfSlot * 8 + 4, &newCharLen, 4);
        }
    }

    return ss;
}

std::vector<uint8_t> Module::serialize() {
    const uint32_t count = function_count();

    // One kind of edit per serialize.
    int64_t dirtyIdx = -1;
    for (uint32_t i = 0; i < count; ++i)
        if (funcs_[i].dirty) {
            if (dirtyIdx >= 0)
                throw std::runtime_error("Module: only one dirty function per serialize");
            dirtyIdx = i;
        }
    const int queued = (dirtyIdx >= 0) + !pendingStrings_.empty() + pendingStringEdit_.has_value() +
                       pendingLiteralEdit_.has_value() + pendingLiteralResize_.has_value() +
                       pendingNewFunction_.has_value();
    if (queued > 1)
        throw std::runtime_error(
            "Module: structural operations (set_function_body, add_string, "
            "set_string_value, set_literal_value, resize_literal, add_function) "
            "cannot be combined in the same serialize - do two separate commits");

    // add_function grows the function table by one 16-byte header: everything
    // after it shifts through the same front-growth as the string tables.
    const uint32_t newFuncTableEnd = funcTableEnd_ + (pendingNewFunction_ ? 16u : 0u);

    // 1. Bytecode blob: verbatim, with the dirty body spliced in.
    std::vector<uint8_t> newBlob = bytecodeBlob_;
    AssembledFunction    asmd;
    uint32_t             splicePos    = 0;
    int64_t              N            = 0;   // bytes added/removed in the dirty function
    uint32_t             newDirtySize = 0;

    if (dirtyIdx >= 0) {
        const FuncState& d = funcs_[dirtyIdx];
        asmd = assemble_function(version_, d.newIr);
        newDirtySize = static_cast<uint32_t>(asmd.bytecode.size());
        const size_t pos = d.origOffset - blobStart_;
        newBlob.erase(newBlob.begin() + pos, newBlob.begin() + pos + d.origSize);
        newBlob.insert(newBlob.begin() + pos, asmd.bytecode.begin(), asmd.bytecode.end());
        splicePos = d.origOffset;
        N = static_cast<int64_t>(newDirtySize) - static_cast<int64_t>(d.origSize);
    }

    // add_function appends the new body at the end: nothing existing moves.
    AssembledFunction newFuncAsm;
    uint32_t          newFuncBodyRelOffset = 0;   // relative to newBlob
    if (pendingNewFunction_) {
        newFuncAsm = assemble_function(version_, pendingNewFunction_->ir);
        if (newFuncAsm.bytecode.size() >= (1u << 15))
            throw std::runtime_error(
                "Module::add_function: the new function's bytecode is >= 32768 B "
                "(15-bit field) - would need promotion to overflow (not "
                "supported)");
        newFuncBodyRelOffset = static_cast<uint32_t>(newBlob.size());
        newBlob.insert(newBlob.end(), newFuncAsm.bytecode.begin(), newFuncAsm.bytecode.end());
    }

    // 2. String sections (add_string / set_string_value).
    const StringSections ss = buildStringSections();

    // 3. Literal edits: append the new buffers to their sections and overwrite
    //    the operands in newBlob (same width, no function moves).
    std::vector<MidSection> newMidSections = midSections_;
    auto appendToMid = [&](const std::string& midName, const std::string& bytes) {
        for (MidSection& ms : newMidSections) {
            if (ms.name == midName) {
                ms.bytes.insert(ms.bytes.end(), bytes.begin(), bytes.end());
                return;
            }
        }
        throw std::runtime_error("Module: section '" + midName + "' missing during serialize");
    };

    if (pendingLiteralEdit_) {
        const PendingLiteralEdit& pl = *pendingLiteralEdit_;
        appendToMid(pl.midSectionName, pl.newBufferBytes);
        std::memcpy(newBlob.data() + pl.operandBytePos, &pl.offRel, pl.operandWidth);
    }

    if (pendingLiteralResize_) {
        const PendingLiteralResize& pr = *pendingLiteralResize_;
        appendToMid(pr.valueMidSectionName, pr.newBufferBytes);
        if (pr.isObject)
            appendToMid("object key buffer", pr.newKeyBufferBytes);

        std::memcpy(newBlob.data() + pr.countOperandBytePos,    &pr.newCount, 2);
        std::memcpy(newBlob.data() + pr.sizeHintOperandBytePos, &pr.newCount, 2);
        std::memcpy(newBlob.data() + pr.valueOperandBytePos, &pr.valueOffRel, pr.valueOperandWidth);
        if (pr.isObject)
            std::memcpy(newBlob.data() + pr.keyOperandBytePos, &pr.keyOffRel, pr.keyOperandWidth);
    }

    // 4. Re-layout: kind table, id hash, string table, overflow table, string
    //    storage, mid sections (each with its alignment), bytecode (align 4).
    uint32_t lp = newFuncTableEnd;
    lp += static_cast<uint32_t>(ss.kindTable.size());
    lp += static_cast<uint32_t>(idHash_.size());
    lp += static_cast<uint32_t>(ss.stringTable.size());
    lp += static_cast<uint32_t>(ss.overflowTable.size());
    lp += static_cast<uint32_t>(ss.storage.size());
    for (const MidSection& ms : newMidSections) {
        const uint32_t a = ms.align ? ms.align : 1;
        lp = (lp + a - 1) & ~(a - 1);
        lp += static_cast<uint32_t>(ms.bytes.size());
    }
    const uint32_t newBlobStart = align4(lp);
    const int64_t  frontDelta   =
        static_cast<int64_t>(newBlobStart) - static_cast<int64_t>(blobStart_);

    const uint32_t newBlobEnd   = newBlobStart + static_cast<uint32_t>(newBlob.size());
    const uint32_t newInfoStart = align4(newBlobEnd);
    const int64_t  delta        = static_cast<int64_t>(newInfoStart) - static_cast<int64_t>(oldInfoStart_);

    // New debugInfoOffset. Also the infoOffset of an added function: it points
    // at the end of the existing info block, always in bounds, and the parser
    // never dereferences it for a function with no EH and no debug info.
    const int64_t newDbgOff = static_cast<int64_t>(oldDebugInfoOffset_) + delta;
    if (newDbgOff < 0)
        throw std::runtime_error("Module: negative debugInfoOffset");

    // Mutable copy of the info block (overflow FunctionHeaders and EH tables).
    std::vector<uint8_t> info = infoBlob_;

    auto newBodyOffsetOf = [&](const FuncState& fs) -> int64_t {
        int64_t base = fs.origOffset;
        // members of the dirty group have origOffset == splicePos: they stay there
        if (dirtyIdx >= 0 && fs.origOffset > splicePos)
            base = static_cast<int64_t>(fs.origOffset) + N;
        return base + frontDelta;
    };

    // 5. Header of the added function (same 25-bit bounds as step 6).
    std::vector<uint8_t> newFuncHeaderBytes;
    if (pendingNewFunction_) {
        const int64_t newFuncOff = static_cast<int64_t>(newBlobStart) +
                                   static_cast<int64_t>(newFuncBodyRelOffset);
        if (newFuncOff >= (1 << 25))
            throw std::runtime_error(
                "Module::add_function: new function offset >= 2^25 - "
                "would need promotion to overflow (not supported)");
        if (newDbgOff >= (1 << 25))
            throw std::runtime_error(
                "Module::add_function: new function infoOffset >= 2^25 (25-bit field)");

        const PendingFunction& pf = *pendingNewFunction_;
        newFuncHeaderBytes = buildNewFunctionHeader(
            static_cast<uint32_t>(newFuncOff), static_cast<uint32_t>(newDbgOff),
            static_cast<uint32_t>(newFuncAsm.bytecode.size()),
            pf.paramCount, pf.functionNameId, pf.frameSize, pf.strictMode);
    }

    // 6. Rewrite the function table (SmallFuncHeader, and the FunctionHeader
    //    in the info block for overflowed functions).
    auto* fh = reinterpret_cast<SmallFuncHeader*>(funcHeaders_.data());
    const FuncState* dirty = dirtyIdx >= 0 ? &funcs_[dirtyIdx] : nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        FuncState& fs = funcs_[i];

        const bool inGroup = dirty && dirty->editAll &&
            std::find(dirty->shareGroup.begin(), dirty->shareGroup.end(), i) != dirty->shareGroup.end();
        const bool changedBody = (static_cast<int64_t>(i) == dirtyIdx) || inGroup;

        const int64_t newOff  = newBodyOffsetOf(fs);
        const uint32_t newSz  = changedBody ? newDirtySize : fs.origSize;
        const int64_t newInfo = static_cast<int64_t>(fs.origInfoOffset) + delta;
        if (newOff < 0 || newInfo < 0)
            throw std::runtime_error("Module: negative function offset after relocation");

        if (fs.overflowed) {
            const int64_t newLargeOff = static_cast<int64_t>(fs.origLargeOff) + delta;
            if (newLargeOff < 0)
                throw std::runtime_error("Module: negative largeOff");
            const size_t p = fs.origLargeOff - oldInfoStart_;   // position in `info`
            if (p + sizeof(FunctionHeader) > info.size())
                throw std::runtime_error("Module: overflow FunctionHeader beyond the info block");
            const uint32_t off32 = static_cast<uint32_t>(newOff);
            const uint32_t lo32  = static_cast<uint32_t>(newLargeOff);
            std::memcpy(info.data() + p +  0, &off32, 4);   // FunctionHeader.offset
            std::memcpy(info.data() + p +  8, &newSz, 4);   // FunctionHeader.bytecodeSizeInBytes
            std::memcpy(info.data() + p + 16, &lo32,  4);   // FunctionHeader.infoOffset (sentinel = largeOff)
            if (changedBody) {
                const uint32_t fsz = fs.newFrameSize ? fs.newFrameSize : fs.origFrameSize;
                std::memcpy(info.data() + p + 20, &fsz, 4); // FunctionHeader.frameSize
            }
            // SmallFuncHeader keeps the new largeOff in the low 16 bits of
            // offset / infoOffset; the overflowed flag is unchanged.
            fh[i].offset     = (fh[i].offset     & ~0xFFFFu) | (lo32 & 0xFFFFu);
            fh[i].infoOffset = (fh[i].infoOffset & ~0xFFFFu) | ((lo32 >> 16) & 0xFFFFu);
        } else {
            if (newOff >= (1 << 25))
                throw std::runtime_error("Module: function offset >= 2^25 - would need promotion to overflow (not supported)");
            if (newSz >= (1u << 15))
                throw std::runtime_error("Module: function " +
                    std::to_string(i) + " bytecodeSizeInBytes >= 32768 - would need promotion to overflow (not supported)");
            if (newInfo >= (1 << 25))
                throw std::runtime_error("Module: function infoOffset >= 2^25 (25-bit field)");
            fh[i].offset              = static_cast<uint32_t>(newOff);
            fh[i].bytecodeSizeInBytes = newSz;
            fh[i].infoOffset          = static_cast<uint32_t>(newInfo);
            if (changedBody)
                fh[i].frameSize = fs.newFrameSize ? fs.newFrameSize : fs.origFrameSize;
        }
    }

    // 7. Relocate the EH tables (in `info`) of every function whose body
    //    changed: the dirty one and, with editAll, its whole shared group.
    //    Each member has its own table but all refer to the same shared body,
    //    so the same old-to-new offset map applies.
    std::unordered_map<uint32_t, uint32_t> origToNew;  // original byte offset -> new
    if (dirty) {
        for (size_t j = 0; j < dirty->newIr.size(); ++j)
            if (dirty->newIr[j].origByteOffset >= 0)
                origToNew[static_cast<uint32_t>(dirty->newIr[j].origByteOffset)] = asmd.offsets[j];
    }
    auto remap = [&](uint32_t v) -> uint32_t {
        if (v == dirty->origSize) return newDirtySize;             // one past the end
        auto it = origToNew.find(v);
        if (it == origToNew.end())
            throw std::runtime_error("Module: EH offset not aligned to an instruction (" +
                                     std::to_string(v) + ")");
        return it->second;
    };

    std::unordered_set<size_t> relocatedEH;
    for (uint32_t i = 0; i < count && dirty; ++i) {
        const FuncState& fs = funcs_[i];
        const bool inGroup = dirty->editAll &&
            std::find(dirty->shareGroup.begin(), dirty->shareGroup.end(), i) != dirty->shareGroup.end();
        if ((static_cast<int64_t>(i) != dirtyIdx && !inGroup) || !fs.hasEH) continue;

        if (fs.origInfoOffset < oldInfoStart_)
            throw std::runtime_error("Module: function " + std::to_string(i) +
                                     " infoOffset precedes the info block");
        const size_t base = fs.origInfoOffset - oldInfoStart_;
        if (base + 4 > info.size())
            throw std::runtime_error("Module: EH table beyond the info block");
        // two functions can point at the same info section: relocate it once
        if (!relocatedEH.insert(base).second) continue;

        uint32_t ehCount = 0;
        std::memcpy(&ehCount, info.data() + base, 4);
        if (base + 4 + static_cast<size_t>(ehCount) * 12 > info.size())
            throw std::runtime_error("Module: truncated EH table");

        for (uint32_t j = 0; j < ehCount; ++j) {
            const size_t p = base + 4 + static_cast<size_t>(j) * 12;
            uint32_t s, e, t;
            std::memcpy(&s, info.data() + p,     4);
            std::memcpy(&e, info.data() + p + 4, 4);
            std::memcpy(&t, info.data() + p + 8, 4);
            s = remap(s); e = remap(e); t = remap(t);
            std::memcpy(info.data() + p,     &s, 4);
            std::memcpy(info.data() + p + 4, &e, 4);
            std::memcpy(info.data() + p + 8, &t, 4);
        }
    }

    // 8. Header fields (fileLength is set once the output is built).
    auto* hdr = reinterpret_cast<HBCFileHeader*>(header_.data());
    hdr->debugInfoOffset     = static_cast<uint32_t>(newDbgOff);
    hdr->stringCount         = ss.stringCount;
    hdr->stringKindCount     = ss.kindCount;
    hdr->stringStorageSize   = static_cast<uint32_t>(ss.storage.size());
    hdr->overflowStringCount = ss.overflowCount;
    hdr->functionCount       = count + (pendingNewFunction_ ? 1u : 0u);

    // Literal buffer sizes follow newMidSections (0 if absent). Always written:
    // idempotent when no literal edit is queued.
    auto midSizeOf = [&](const char* name) -> uint32_t {
        for (const MidSection& ms : newMidSections)
            if (ms.name == name) return static_cast<uint32_t>(ms.bytes.size());
        return 0;
    };
    hdr->arrayBufferSize    = midSizeOf("array buffer");
    hdr->objKeyBufferSize   = midSizeOf("object key buffer");
    hdr->objValueBufferSize = midSizeOf("object value buffer");

    // 9. Output: header, function table (+ new header), string sections, mid
    //    sections (aligned), pad, bytecode, pad, info, debug info, footer.
    std::vector<uint8_t> out;
    out.reserve(newInfoStart + info.size() + debugBlob_.size() + 20);

    auto put = [&](const std::vector<uint8_t>& v) { out.insert(out.end(), v.begin(), v.end()); };
    put(header_);
    put(funcHeaders_);
    put(newFuncHeaderBytes);
    put(ss.kindTable);
    put(idHash_);
    put(ss.stringTable);
    put(ss.overflowTable);
    put(ss.storage);

    uint32_t op = static_cast<uint32_t>(out.size());
    for (const MidSection& ms : newMidSections) {
        const uint32_t a = ms.align ? ms.align : 1;
        const uint32_t aligned = (op + a - 1) & ~(a - 1);
        out.insert(out.end(), aligned - op, 0u);
        put(ms.bytes);
        op = aligned + static_cast<uint32_t>(ms.bytes.size());
    }
    out.insert(out.end(), newBlobStart - op, 0u);
    put(newBlob);
    out.insert(out.end(), newInfoStart - newBlobEnd, 0u);
    put(info);
    put(debugBlob_);

    // 10. fileLength (footer included) and the SHA-1 footer of everything above it.
    reinterpret_cast<HBCFileHeader*>(out.data())->fileLength =
        static_cast<uint32_t>(out.size() + 20);
    std::array<uint8_t, 20> digest = sha1(out.data(), out.size());
    out.insert(out.end(), digest.begin(), digest.end());

    warnings_.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (!funcs_[i].dirty) continue;
        if (funcs_[i].hasDebugInfo)
            warnings_.push_back(
                "function " + std::to_string(i) + ": the source-location map was not "
                "re-encoded - stack traces will show wrong lines after the edit "
                "point (recompile the input with `hermes -g0` to avoid this)");
        if (funcs_[i].editAll)
            warnings_.push_back(
                "modified " + std::to_string(funcs_[i].shareGroup.size()) +
                " functions that shared the same bytecode");
    }

    return out;
}

}  // namespace hbc
