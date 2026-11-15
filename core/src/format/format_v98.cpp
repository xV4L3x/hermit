// FormatVersion module for HBC v98 (Static Hermes, React Native 0.84+). v98 has
// 12-byte function headers with no infoOffset / environmentSize and a 2-bit kind
// flag, a 23-byte overflow header, two uint32 per-function debug offsets, and a
// merged literal value buffer plus an object shape table. Structs: format_v98.h.

#include "hbc/format_version.h"
#include "hbc/format_v98.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace hbc {

namespace {

class FormatV98 final : public FormatVersion {
public:
    uint32_t funcHeaderSize()  const override { return sizeof(SmallFuncHeader_v98); }  // 12
    uint32_t largeHeaderSize() const override { return sizeof(FunctionHeader_v98); }   // 23

    FuncHeaderFields readFuncHeader(const uint8_t* fileData, uint32_t fileLen,
                                    const uint8_t* fhBase, uint32_t index) const override {
        const auto& fh = reinterpret_cast<const SmallFuncHeader_v98*>(fhBase)[index];
        FuncHeaderFields r;
        r.overflowed = fh.flags.overflowed;
        if (!r.overflowed) {
            r.offset              = fh.offset;
            r.bytecodeSizeInBytes = fh.bytecodeSizeInBytes;
            r.paramCount          = fh.paramCount;
            r.frameSize           = fh.frameSize;
            r.infoOffset          = 0;  // no info section when not overflowed
            r.functionName        = fh.functionName;
            fillFlags(r, fh.flags);
            return r;
        }
        const uint32_t largeOff = largeHeaderOffset(reinterpret_cast<const uint8_t*>(&fh));
        if (static_cast<uint64_t>(largeOff) + sizeof(FunctionHeader_v98) > fileLen) {
            r.valid = false;
            return r;
        }
        const auto* lh = reinterpret_cast<const FunctionHeader_v98*>(fileData + largeOff);
        r.offset              = lh->offset;
        r.bytecodeSizeInBytes = lh->bytecodeSizeInBytes;
        r.paramCount          = lh->paramCount;
        r.frameSize           = lh->frameSize;
        // EH table / debug offsets follow the large header, aligned to 4.
        r.infoOffset          = (largeOff + static_cast<uint32_t>(sizeof(FunctionHeader_v98)) + 3) & ~3u;
        r.functionName        = lh->functionName;
        fillFlags(r, lh->flags);
        return r;
    }

    uint32_t largeHeaderOffset(const uint8_t* smallHeader) const override {
        const auto& fh = *reinterpret_cast<const SmallFuncHeader_v98*>(smallHeader);
        return (static_cast<uint32_t>(fh.functionName) << 16) | fh.offset;
    }

    uint32_t debugOffsetsSize() const override { return 8; }
    DebugOffsetsFields readDebugOffsets(const uint8_t* p) const override {
        DebugOffsetsFields d;
        std::memcpy(&d.debugOffset, p, 4);  // sourceLocations; no scope/textified sections
        return d;
    }

    bool objectLiteral(const std::vector<uint32_t>& op, const uint8_t* shapeTable,
                       uint32_t shapeTableSize, uint32_t& keyBufferOffset, uint32_t& count,
                       uint32_t& valueBufferIdx) const override {
        // v98 operands: dst, shapeTableIndex, valueBufferIndex. The key buffer
        // offset and property count come from the shape table entry.
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
        // v98 header words: arrayBufferSize holds literalValueBufferSize,
        // objValueBufferSize holds objShapeTableCount (8-byte entries).
        return {
            {"literal value buffer", h.arrayBufferSize,  4},
            {"object key buffer",    h.objKeyBufferSize, 4},
            {"object shape table",
             static_cast<uint32_t>(h.objValueBufferSize * sizeof(ShapeTableEntry)), 4},
        };
    }
    const char* arrayBufferSection()    const override { return "literal value buffer"; }
    const char* objValueBufferSection() const override { return "literal value buffer"; }
    const char* objValueWordSection()   const override { return "object shape table"; }

    void writeFuncHeader(uint8_t* fhBase, uint32_t index,
                         uint8_t* info, uint32_t infoStart, size_t infoSize,
                         const FuncHeaderWrite& w) const override {
        auto& fh = reinterpret_cast<SmallFuncHeader_v98*>(fhBase)[index];
        if (w.overflowed) {
            const size_t p = w.origLargeOffset - infoStart;
            if (p + sizeof(FunctionHeader_v98) > infoSize)
                throw std::runtime_error("Module: overflow FunctionHeader beyond the info block");
            // FunctionHeader_v98: offset@0, bytecodeSizeInBytes@8, frameSize@16 (uint32);
            // no infoOffset field.
            std::memcpy(info + p +  0, &w.offset,              4);
            std::memcpy(info + p +  8, &w.bytecodeSizeInBytes, 4);
            if (w.writeFrameSize)
                std::memcpy(info + p + 16, &w.frameSize, 4);
            // The small header packs the large offset into offset | (functionName << 16).
            fh.offset       = w.newLargeOffset & 0xFFFFu;
            fh.functionName = w.newLargeOffset >> 16;
        } else {
            if (w.offset >= (1u << 25))
                throw std::runtime_error("Module: function offset >= 2^25 - would need promotion to overflow (not supported)");
            if (w.bytecodeSizeInBytes >= (1u << 15))
                throw std::runtime_error("Module: function bytecodeSizeInBytes >= 32768 - would need promotion to overflow (not supported)");
            fh.offset              = w.offset;
            fh.bytecodeSizeInBytes = w.bytecodeSizeInBytes;
            if (w.writeFrameSize)
                fh.frameSize = static_cast<uint8_t>(w.frameSize);
        }
    }

    void writeHeaderBufferWords(HBCFileHeader& h, uint32_t arrayOrLiteralValueBytes,
                                uint32_t objKeyBytes, uint32_t objValueOrShapeBytes) const override {
        h.arrayBufferSize    = arrayOrLiteralValueBytes;                 // literalValueBufferSize
        h.objKeyBufferSize   = objKeyBytes;
        h.objValueBufferSize = objValueOrShapeBytes / sizeof(ShapeTableEntry);  // objShapeTableCount
    }

private:
    static void fillFlags(FuncHeaderFields& r, const FunctionFlags_v98& f) {
        r.prohibitInvoke      = f.prohibitInvoke;
        r.kind                = f.kind;
        r.strictMode          = f.strictMode;
        r.hasExceptionHandler = f.hasExceptionHandler;
        r.hasDebugInfo        = f.hasDebugInfo;
    }
};

const FormatV98 kV98;

} // namespace

const FormatVersion* formatV98() { return &kV98; }

} // namespace hbc
