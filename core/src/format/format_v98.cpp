// FormatVersion module for HBC v98 as shipped by Hermes V1 (React Native 0.84+),
// tag hermes-v250829098.0.14. See format_v98.h for the format summary. The
// 12-byte function header uses Hermes's BitField storage, which does not map to
// a portable C bitfield struct, so the fields are parsed/written bit by bit.

#include "hbc/format_version.h"
#include "hbc/format_v98.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace hbc {

namespace {

// Small-header word layout (two uint32 words + three bytes + flags):
//   w1: Offset[0:25] ParamCount[25:30] LoopDepth[30:32]
//   w2: BytecodeSizeInBytes[0:14] FunctionName[14:22] NumberRegCount[22:27] NonPtrRegCount[27:32]
//   b8: FrameSize   b9: ReadCacheSize   b10: WriteCacheSize[0:6] ...   b11: flags
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void     wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// Flag byte: prohibitInvoke[0:2] strictMode[2] hasExceptionHandler[3]
//            hasDebugInfo[4] overflowed[5] kind[6:8]. `overflowed` is NOT read
//            here: an overflowed function's large header carries the real flags
//            (with overflowed = 0), so the caller sets r.overflowed from the
//            small header it already examined.
void fillFlags(FuncHeaderFields& r, uint8_t flags) {
    r.prohibitInvoke      = flags & 0x3;
    r.strictMode          = (flags >> 2) & 1;
    r.hasExceptionHandler = (flags >> 3) & 1;
    r.hasDebugInfo        = (flags >> 4) & 1;
    r.kind                = (flags >> 6) & 0x3;
}

class FormatV98 final : public FormatVersion {
public:
    uint32_t funcHeaderSize()  const override { return kV98SmallFuncHeaderSize; }  // 12
    uint32_t largeHeaderSize() const override { return kV98LargeFuncHeaderSize; }  // 37

    FuncHeaderFields readFuncHeader(const uint8_t* fileData, uint32_t fileLen,
                                    const uint8_t* fhBase, uint32_t index) const override {
        const uint8_t* p = fhBase + static_cast<size_t>(index) * kV98SmallFuncHeaderSize;
        FuncHeaderFields r;
        r.overflowed = (p[11] >> 5) & 1;  // from the small header, not the large one
        if (!r.overflowed) {
            const uint32_t w1 = rd32(p), w2 = rd32(p + 4);
            r.offset              = w1 & 0x01FFFFFFu;         // 25 bits
            r.paramCount          = (w1 >> 25) & 0x1Fu;       // 5 bits
            r.bytecodeSizeInBytes = w2 & 0x3FFFu;             // 14 bits
            r.functionName        = (w2 >> 14) & 0xFFu;       // 8 bits
            r.frameSize           = p[8];
            r.infoOffset          = 0;  // no info section unless overflowed
            fillFlags(r, p[11]);
            return r;
        }
        const uint32_t largeOff = largeHeaderOffset(p);
        if (static_cast<uint64_t>(largeOff) + kV98LargeFuncHeaderSize > fileLen) {
            r.valid = false;
            return r;
        }
        // Large header (37 bytes): Offset@0, ParamCount@4, LoopDepth@8,
        // BytecodeSizeInBytes@12, FunctionName@16, ... FrameSize@28, ... flags@36.
        const uint8_t* lh = fileData + largeOff;
        r.offset              = rd32(lh +  0);
        r.paramCount          = rd32(lh +  4);
        r.bytecodeSizeInBytes = rd32(lh + 12);
        r.functionName        = rd32(lh + 16);
        r.frameSize           = rd32(lh + 28);
        r.infoOffset          = (largeOff + kV98LargeFuncHeaderSize + 3) & ~3u;  // EH/debug follow, 4-aligned
        fillFlags(r, lh[36]);
        return r;
    }

    uint32_t largeHeaderOffset(const uint8_t* smallHeader) const override {
        // Overflow packs the offset as offset(low 24) | functionName(high 8).
        const uint32_t offset       = rd32(smallHeader) & 0x00FFFFFFu;
        const uint32_t functionName = (rd32(smallHeader + 4) >> 14) & 0xFFu;
        return (functionName << 24) | offset;
    }

    uint32_t debugOffsetsSize() const override { return 4; }  // { sourceLocations }
    DebugOffsetsFields readDebugOffsets(const uint8_t* p) const override {
        DebugOffsetsFields d;
        std::memcpy(&d.debugOffset, p, 4);  // sourceLocations; no lexical/scope/textified
        return d;
    }

    bool objectLiteral(const std::vector<uint32_t>& op, const uint8_t* shapeTable,
                       uint32_t shapeTableSize, uint32_t& keyBufferOffset, uint32_t& count,
                       uint32_t& valueBufferIdx) const override {
        // NewObjectWithBuffer: dst, shapeTableIndex, valueBufferIndex.
        if (op.size() < 3) return false;
        const uint32_t shapeIdx = op[1];
        valueBufferIdx = op[2];
        if (static_cast<uint64_t>(shapeIdx) * sizeof(ShapeTableEntry) + sizeof(ShapeTableEntry)
                > shapeTableSize)
            return false;
        const uint8_t* e = shapeTable + static_cast<size_t>(shapeIdx) * sizeof(ShapeTableEntry);
        std::memcpy(&keyBufferOffset, e,     4);
        std::memcpy(&count,           e + 4, 4);
        return true;
    }

    std::vector<MidSectionDesc> literalBufferSections(const HBCFileHeader& h) const override {
        return {
            {"literal value buffer", h.arrayBufferSize,  4},   // literalValueBufferSize @0x50
            {"object key buffer",    h.objKeyBufferSize, 4},
            {"object shape table",
             static_cast<uint32_t>(h.objValueBufferSize * sizeof(ShapeTableEntry)), 4},  // count @0x58
        };
    }
    const char* arrayBufferSection()    const override { return "literal value buffer"; }
    const char* objValueBufferSection() const override { return "literal value buffer"; }
    const char* objValueWordSection()   const override { return "object shape table"; }
    uint32_t objectValueOperandIndex()   const override { return 2; }
    bool     objectLiteralUsesShapeTable() const override { return true; }

    // The tail header words (segmentID, cjsModuleCount, functionSourceCount,
    // debugInfoOffset) are shifted +4 by numStringSwitchImms; read/write them
    // through the v98 header struct, not the shared (v94-96) one.
    uint32_t debugInfoOffset(const HBCFileHeader& h) const override {
        return reinterpret_cast<const HBCFileHeader_v98&>(h).debugInfoOffset;
    }
    uint32_t cjsModuleCount(const HBCFileHeader& h) const override {
        return reinterpret_cast<const HBCFileHeader_v98&>(h).cjsModuleCount;
    }
    uint32_t functionSourceCount(const HBCFileHeader& h) const override {
        return reinterpret_cast<const HBCFileHeader_v98&>(h).functionSourceCount;
    }
    void setDebugInfoOffset(HBCFileHeader& h, uint32_t v) const override {
        reinterpret_cast<HBCFileHeader_v98&>(h).debugInfoOffset = v;
    }

    void writeFuncHeader(uint8_t* fhBase, uint32_t index,
                         uint8_t* info, uint32_t infoStart, size_t infoSize,
                         const FuncHeaderWrite& w) const override {
        uint8_t* p = fhBase + static_cast<size_t>(index) * kV98SmallFuncHeaderSize;
        if (w.overflowed) {
            const size_t pos = w.origLargeOffset - infoStart;
            if (pos + kV98LargeFuncHeaderSize > infoSize)
                throw std::runtime_error("Module: overflow FunctionHeader beyond the info block");
            wr32(info + pos +  0, w.offset);
            wr32(info + pos + 12, w.bytecodeSizeInBytes);
            if (w.writeFrameSize)
                wr32(info + pos + 28, w.frameSize);
            // Small header: offset(low 24) in w1, functionName(high 8) in w2[14:22].
            uint32_t w1 = rd32(p), w2 = rd32(p + 4);
            w1 = (w1 & ~0x00FFFFFFu) | (w.newLargeOffset & 0x00FFFFFFu);
            w2 = (w2 & ~(0xFFu << 14)) | (((w.newLargeOffset >> 24) & 0xFFu) << 14);
            wr32(p, w1); wr32(p + 4, w2);
        } else {
            if (w.offset >= (1u << 25))
                throw std::runtime_error("Module: function offset >= 2^25 - would need promotion to overflow (not supported)");
            if (w.bytecodeSizeInBytes >= (1u << 14))
                throw std::runtime_error("Module: function bytecodeSizeInBytes >= 16384 (14-bit field) - would need promotion to overflow (not supported)");
            uint32_t w1 = rd32(p), w2 = rd32(p + 4);
            w1 = (w1 & ~0x01FFFFFFu) | (w.offset & 0x01FFFFFFu);
            w2 = (w2 & ~0x3FFFu) | (w.bytecodeSizeInBytes & 0x3FFFu);
            wr32(p, w1); wr32(p + 4, w2);
            if (w.writeFrameSize)
                p[8] = static_cast<uint8_t>(w.frameSize);
        }
    }

    void writeHeaderBufferWords(HBCFileHeader& h, uint32_t arrayOrLiteralValueBytes,
                                uint32_t objKeyBytes, uint32_t objValueOrShapeBytes) const override {
        h.arrayBufferSize    = arrayOrLiteralValueBytes;                        // literalValueBufferSize
        h.objKeyBufferSize   = objKeyBytes;
        h.objValueBufferSize = objValueOrShapeBytes / sizeof(ShapeTableEntry);  // objShapeTableCount
    }

    std::vector<uint8_t> buildNewSmallHeader(uint32_t offset, uint32_t bodySize,
        uint32_t paramCount, uint32_t nameId, uint32_t frameSize, uint32_t /*infoOffset*/,
        bool strictMode) const override {
        if (bodySize >= (1u << 14))
            throw std::runtime_error("Module::add_function: body >= 16384 bytes (14-bit field)");
        if (nameId > 0xFF)
            throw std::runtime_error("Module::add_function: functionName >= 256 (8-bit field)");
        std::vector<uint8_t> b(kV98SmallFuncHeaderSize, 0);
        const uint32_t w1 = (offset & 0x01FFFFFFu) | ((paramCount & 0x1Fu) << 25);
        const uint32_t w2 = (bodySize & 0x3FFFu)   | ((nameId & 0xFFu) << 14);
        wr32(b.data(), w1); wr32(b.data() + 4, w2);
        b[8] = static_cast<uint8_t>(frameSize);
        uint8_t flags = kProhibitInvokeNone & 0x3u;  // callable + constructible
        if (strictMode) flags |= (1u << 2);
        // hasEH=0, hasDebug=0, overflowed=0, kind=Normal=0
        b[11] = flags;
        return b;
    }
};

const FormatV98 kV98;

} // namespace

const FormatVersion* formatV98() { return &kV98; }

} // namespace hbc
