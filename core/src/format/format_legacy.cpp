// FormatVersion module for HBC v94/v95/v96 (the facebook/hermes mainline). These
// three share the layout in format.h: a 16-byte SmallFuncHeader with an
// infoOffset field, a 32-byte overflow FunctionHeader, three uint32 per-function
// debug offsets, and separate array / object-value literal buffers.

#include "hbc/format_version.h"
#include "hbc/format.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace hbc {

namespace {

class FormatLegacy final : public FormatVersion {
public:
    uint32_t funcHeaderSize()  const override { return sizeof(SmallFuncHeader); }  // 16
    uint32_t largeHeaderSize() const override { return sizeof(FunctionHeader); }   // 32

    FuncHeaderFields readFuncHeader(const uint8_t* fileData, uint32_t fileLen,
                                    const uint8_t* fhBase, uint32_t index) const override {
        const auto& fh = reinterpret_cast<const SmallFuncHeader*>(fhBase)[index];
        FuncHeaderFields r;
        r.overflowed = fh.flags.overflowed;
        if (!r.overflowed) {
            r.offset              = fh.offset;
            r.bytecodeSizeInBytes = fh.bytecodeSizeInBytes;
            r.paramCount          = fh.paramCount;
            r.frameSize           = fh.frameSize;
            r.infoOffset          = fh.infoOffset;
            r.functionName        = fh.functionName;
            fillFlags(r, fh.flags);
            return r;
        }
        const uint32_t largeOff = largeHeaderOffset(reinterpret_cast<const uint8_t*>(&fh));
        if (static_cast<uint64_t>(largeOff) + sizeof(FunctionHeader) > fileLen) {
            r.valid = false;
            return r;
        }
        const auto* lh = reinterpret_cast<const FunctionHeader*>(fileData + largeOff);
        r.offset              = lh->offset;
        r.bytecodeSizeInBytes = lh->bytecodeSizeInBytes;
        r.paramCount          = lh->paramCount;
        r.frameSize           = lh->frameSize;
        // The info section (EH + debug) starts right after the large header.
        r.infoOffset          = largeOff + static_cast<uint32_t>(sizeof(FunctionHeader));
        r.functionName        = lh->functionName;
        fillFlags(r, lh->flags);
        return r;
    }

    uint32_t largeHeaderOffset(const uint8_t* smallHeader) const override {
        const auto& fh = *reinterpret_cast<const SmallFuncHeader*>(smallHeader);
        return (fh.offset & 0xFFFFu) | ((fh.infoOffset & 0xFFFFu) << 16);
    }

    uint32_t debugOffsetsSize() const override { return 12; }
    DebugOffsetsFields readDebugOffsets(const uint8_t* p) const override {
        DebugOffsetsFields d;
        std::memcpy(&d.debugOffset,            p,     4);
        std::memcpy(&d.scopeDescOffset,        p + 4, 4);
        std::memcpy(&d.textifiedCalleesOffset, p + 8, 4);
        return d;
    }

    bool objectLiteral(const std::vector<uint32_t>& op, const uint8_t*, uint32_t,
                       uint32_t& keyBufferOffset, uint32_t& count,
                       uint32_t& valueBufferIdx) const override {
        // v94-96 operands: dst, sizeHint, count, keyBufferIdx, valueBufferIdx.
        if (op.size() < 5) return false;
        count          = op[2];
        keyBufferOffset = op[3];
        valueBufferIdx = op[4];
        return true;
    }

    std::vector<MidSectionDesc> literalBufferSections(const HBCFileHeader& h) const override {
        return {
            {"array buffer",        h.arrayBufferSize,    4},
            {"object key buffer",   h.objKeyBufferSize,   4},
            {"object value buffer", h.objValueBufferSize, 4},
        };
    }
    const char* arrayBufferSection()    const override { return "array buffer"; }
    const char* objValueBufferSection() const override { return "object value buffer"; }
    const char* objValueWordSection()   const override { return "object value buffer"; }
    uint32_t objectValueOperandIndex()   const override { return 4; }
    bool     objectLiteralUsesShapeTable() const override { return false; }

    void writeFuncHeader(uint8_t* fhBase, uint32_t index,
                         uint8_t* info, uint32_t infoStart, size_t infoSize,
                         const FuncHeaderWrite& w) const override {
        auto& fh = reinterpret_cast<SmallFuncHeader*>(fhBase)[index];
        if (w.overflowed) {
            const size_t p = w.origLargeOffset - infoStart;
            if (p + sizeof(FunctionHeader) > infoSize)
                throw std::runtime_error("Module: overflow FunctionHeader beyond the info block");
            std::memcpy(info + p +  0, &w.offset,              4);  // FunctionHeader.offset
            std::memcpy(info + p +  8, &w.bytecodeSizeInBytes, 4);  // bytecodeSizeInBytes
            std::memcpy(info + p + 16, &w.newLargeOffset,      4);  // infoOffset (sentinel = largeOff)
            if (w.writeFrameSize)
                std::memcpy(info + p + 20, &w.frameSize, 4);
            fh.offset     = (fh.offset     & ~0xFFFFu) | (w.newLargeOffset & 0xFFFFu);
            fh.infoOffset = (fh.infoOffset & ~0xFFFFu) | ((w.newLargeOffset >> 16) & 0xFFFFu);
        } else {
            if (w.offset >= (1u << 25))
                throw std::runtime_error("Module: function offset >= 2^25 - would need promotion to overflow (not supported)");
            if (w.bytecodeSizeInBytes >= (1u << 15))
                throw std::runtime_error("Module: function bytecodeSizeInBytes >= 32768 - would need promotion to overflow (not supported)");
            if (w.infoOffset >= (1u << 25))
                throw std::runtime_error("Module: function infoOffset >= 2^25 (25-bit field)");
            fh.offset              = w.offset;
            fh.bytecodeSizeInBytes = w.bytecodeSizeInBytes;
            fh.infoOffset          = w.infoOffset;
            if (w.writeFrameSize)
                fh.frameSize = w.frameSize;
        }
    }

    void writeHeaderBufferWords(HBCFileHeader& h, uint32_t arrayOrLiteralValueBytes,
                                uint32_t objKeyBytes, uint32_t objValueOrShapeBytes) const override {
        h.arrayBufferSize    = arrayOrLiteralValueBytes;
        h.objKeyBufferSize   = objKeyBytes;
        h.objValueBufferSize = objValueOrShapeBytes;
    }

    std::vector<uint8_t> buildNewSmallHeader(uint32_t offset, uint32_t bodySize,
        uint32_t paramCount, uint32_t nameId, uint32_t frameSize, uint32_t infoOffset,
        bool strictMode) const override {
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
        fh.flags = FunctionFlags{};
        fh.flags.prohibitInvoke = kProhibitInvokeNone;  // callable + constructible
        fh.flags.strictMode     = strictMode;
        std::vector<uint8_t> bytes(sizeof(SmallFuncHeader));
        std::memcpy(bytes.data(), &fh, sizeof(SmallFuncHeader));
        return bytes;
    }

private:
    static void fillFlags(FuncHeaderFields& r, const FunctionFlags& f) {
        r.prohibitInvoke      = f.prohibitInvoke;
        r.kind                = 0;  // v94-96 have no kind field
        r.strictMode          = f.strictMode;
        r.hasExceptionHandler = f.hasExceptionHandler;
        r.hasDebugInfo        = f.hasDebugInfo;
    }
};

const FormatLegacy kLegacy;

} // namespace

const FormatVersion* formatLegacy() { return &kLegacy; }

} // namespace hbc
