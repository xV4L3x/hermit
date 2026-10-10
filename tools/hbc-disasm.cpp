// hbc_disasm <file.hbc> [func:<N>] [--output|-o <out.txt>]
//
// Disassembles every function (or just function N) to stdout or a file:
//
//  *func global (js_params: 0, hbc_params: 1, regs: 4)
//     0x0000  GetGlobalObject     r0
//   func greet (js_params: 1, hbc_params: 2, regs: 11)
//     0x0000  LoadParam           r0, 1
//     0x0002  LoadConstString     r1, "Hello, " [string]

#include "hbc/hbc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Operand as text; a string shows its kind outside the quotes: "value" [kind].
static std::string formatOperand(const hbc::Operand& op) {
    using T = hbc::Operand::Type;
    char buf[64];
    switch (op.type) {
        case T::Reg8:
        case T::Reg32:
            std::snprintf(buf, sizeof(buf), "r%u", op.u32);
            return buf;

        case T::UInt8:
        case T::UInt16:
        case T::UInt32:
            std::snprintf(buf, sizeof(buf), "%u", op.u32);
            return buf;

        case T::Imm32:
            std::snprintf(buf, sizeof(buf), "%d", op.i32);
            return buf;

        case T::Double:
            std::snprintf(buf, sizeof(buf), "%g", op.f64);
            return buf;

        case T::Addr8:
        case T::Addr32:
            // Relative jump offsets, signed.
            if (op.i32 >= 0)
                std::snprintf(buf, sizeof(buf), "+%d", op.i32);
            else
                std::snprintf(buf, sizeof(buf), "%d", op.i32);
            return buf;

        case T::StringID: {
            // resolvedString = "value [kind]" - kind outside the quotes: "value" [kind]
            const auto& s = op.resolvedString;
            auto pos = s.rfind(" [");
            if (pos != std::string::npos)
                return "\"" + s.substr(0, pos) + "\" " + s.substr(pos + 1);
            return "\"" + s + "\"";
        }
    }
    return "?";
}

static void printFunction(const hbc::HBCFile& f, uint32_t idx, FILE* out) {
    const auto& fi = f.functions()[idx];
    const auto& h  = f.header();

    // js_params = paramCount - 1 (the implicit `this` is excluded)
    bool isGlobal = (idx == h.globalCodeIndex);
    std::fprintf(out, "\nfunc %s (js_params: %u, hbc_params: %u, regs: %u",
        fi.name.c_str(),
        fi.paramCount > 0 ? fi.paramCount - 1 : 0,
        fi.paramCount,
        fi.frameSize);

    if (fi.isStrictMode || fi.isAsync || fi.isGenerator) {
        std::fprintf(out, ",");
        if (fi.isStrictMode) std::fprintf(out, " strict");
        if (fi.isGenerator)  std::fprintf(out, " generator");
        if (fi.isAsync)      std::fprintf(out, " async");
    }
    // * marks the global function (entry point)
    std::fprintf(out, ") <fun_id: %u>%s\n", idx, isGlobal ? " *" : "");

    auto instrs = f.disasm(idx);

    if (instrs.empty()) {
        std::fprintf(out, "  <no instructions>\n");
        return;
    }

    for (const auto& instr : instrs) {
        std::fprintf(out, "  0x%04X  ", instr.offset);
        std::fprintf(out, "%-20s", instr.mnemonic.c_str());
        // A mnemonic of 20+ chars would run into the first operand.
        if (!instr.operands.empty() && instr.mnemonic.size() >= 20)
            std::fprintf(out, " ");

        bool first = true;
        for (const auto& op : instr.operands) {
            if (!first) std::fprintf(out, ", ");
            std::fprintf(out, "%s", formatOperand(op).c_str());
            first = false;
        }
        std::fprintf(out, "\n");
    }
}

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s <file.hbc> [func:<N>] [--output|-o <out.txt>]\n"
        "\n"
        "  Without func:   disassembles every function.\n"
        "  func:<N>:       disassembles only the function at index N.\n"
        "  --output/-o:    writes the output to a file instead of stdout.\n"
        "\n"
        "Examples:\n"
        "  %s hello.hbc\n"
        "  %s hello.hbc func:1\n"
        "  %s hello.hbc --output hello_disasm.txt\n"
        "  %s hello.hbc func:1 -o greet.txt\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    const char* hbcPath    = argv[1];
    const char* outputPath = nullptr;   // nullptr = stdout
    int32_t     funcFilter = -1;        // -1 = all functions

    for (int i = 2; i < argc; i++) {
        if (std::strncmp(argv[i], "func:", 5) == 0) {
            funcFilter = static_cast<int32_t>(std::strtol(argv[i] + 5, nullptr, 10));

        } else if ((std::strcmp(argv[i], "--output") == 0 ||
                    std::strcmp(argv[i], "-o") == 0) && i + 1 < argc) {
            outputPath = argv[++i];

        } else {
            std::fprintf(stderr, "Unknown argument: %s\n\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    hbc::HBCFile f = [&]() -> hbc::HBCFile {
        try {
            return hbc::HBCFile::open(hbcPath);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Error opening '%s': %s\n", hbcPath, e.what());
            std::exit(1);
        }
    }();

    FILE* out = stdout;
    if (outputPath) {
        out = std::fopen(outputPath, "w");
        if (!out) {
            std::fprintf(stderr, "Error opening output '%s'\n", outputPath);
            return 1;
        }
    }

    const auto& h = f.header();

    std::fprintf(out, "HBC v%u  |  %u functions  |  global: func[%u]",
        f.version(), h.functionCount, h.globalCodeIndex);
    if (outputPath)
        std::fprintf(out, "  |  -> %s", outputPath);
    std::fprintf(out, "\n");

    if (funcFilter >= 0) {
        uint32_t idx = static_cast<uint32_t>(funcFilter);
        if (idx >= h.functionCount) {
            std::fprintf(stderr, "Error: index %u out of range (functionCount=%u)\n",
                         idx, h.functionCount);
            if (outputPath) std::fclose(out);
            return 1;
        }
        printFunction(f, idx, out);
    } else {
        for (uint32_t idx = 0; idx < h.functionCount; idx++)
            printFunction(f, idx, out);
    }

    std::fprintf(out, "\n");

    if (outputPath) {
        std::fclose(out);
        std::fprintf(stdout, "Output written to: %s\n", outputPath);
    }

    return 0;
}
