// Decompiler: per-instruction expression emitter, CFG structurer and the
// HBCFile::decompile() formatter. Uses only HBCFile's public API.

#include "decompiler/decompiler.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hbc {

static std::string regName(const DecompileContext& ctx, uint32_t idx) {
    if (idx < ctx.regNames.size()) return ctx.regNames[idx];
    return "r" + std::to_string(idx);
}

static std::string strAt(const DecompileContext& ctx, uint32_t idx) {
    if (idx < ctx.strings.size()) return ctx.strings[idx];
    return "<str" + std::to_string(idx) + ">";
}

static bool isValidIdentifier(const std::string& s) {
    if (s.empty()) return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_' || s[0] == '$')) return false;
    for (size_t i = 1; i < s.size(); i++) {
        char c = s[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$')) return false;
    }
    return true;
}

// Name of a referenced function (CreateClosure* / CallDirect*). Anonymous
// functions and names that aren't valid JS identifiers (Metro/Babel labels
// like "?anon_0_foo" or "get Networking", seen on ridemovi.hbc) become func_N.
static std::string funcRefName(const DecompileContext& ctx, uint32_t idx) {
    if (idx >= ctx.functions.size()) return "func_" + std::to_string(idx);
    const std::string& name = ctx.functions[idx].name;
    if (name.rfind("anonymous_func_", 0) == 0 || !isValidIdentifier(name))
        return "func_" + std::to_string(idx);
    return name;
}

static std::string jsStringLiteral(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    out += "\"";
    return out;
}

// obj.prop or obj["non-identifier prop"]
static std::string propAccess(const std::string& objExpr, const std::string& propName) {
    if (isValidIdentifier(propName)) return objExpr + "." + propName;
    return objExpr + "[" + jsStringLiteral(propName) + "]";
}

static std::string formatDouble(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == static_cast<double>(static_cast<int64_t>(d)) && std::abs(d) < 1e15) {
        return std::to_string(static_cast<int64_t>(d));
    }
    std::ostringstream oss;
    oss.precision(17);
    oss << d;
    return oss.str();
}

// SerializedLiteral (SLP) decoding of the array/object literal buffers
// (Python twin: cli/literals.py). Typed RLE runs: a tag byte
// [ext:1][tag:3][len:4] introduces `len` consecutive elements of the same tag
// (up to 4095 with the extension byte, else 15), each followed by its value
// bytes (little-endian, width fixed by the tag).
namespace {

enum class SLPTag : uint8_t {
    Null = 0, True = 1, False = 2, Number = 3,
    LongString = 4, ShortString = 5, ByteString = 6, Integer = 7,
};

// nullopt on a truncated or malformed buffer: the caller falls back to an
// untyped placeholder instead of failing.
std::optional<std::vector<std::string>> decodeSLPElements(
    const std::string& buf, uint32_t start, uint32_t count, const DecompileContext& ctx)
{
    std::vector<std::string> out;
    out.reserve(count);
    const auto* data = reinterpret_cast<const uint8_t*>(buf.data());
    const size_t n = buf.size();
    size_t pos = start;

    while (out.size() < count) {
        if (pos >= n) return std::nullopt;
        uint8_t tagByte = data[pos];
        auto tag = static_cast<SLPTag>((tagByte >> 4) & 0x7);
        uint32_t runLen;
        if (tagByte >> 7) {
            if (pos + 1 >= n) return std::nullopt;
            runLen = (static_cast<uint32_t>(tagByte & 0x0F) << 8) | data[pos + 1];
            pos += 2;
        } else {
            runLen = tagByte & 0x0F;
            pos += 1;
        }

        uint32_t width;
        switch (tag) {
            case SLPTag::Null: case SLPTag::True: case SLPTag::False: width = 0; break;
            case SLPTag::Number:                                      width = 8; break;
            case SLPTag::LongString: case SLPTag::Integer:            width = 4; break;
            case SLPTag::ShortString:                                 width = 2; break;
            case SLPTag::ByteString:                                  width = 1; break;
            default: return std::nullopt;
        }

        for (uint32_t i = 0; i < runLen && out.size() < count; i++) {
            if (pos + width > n) return std::nullopt;
            switch (tag) {
                case SLPTag::Null:  out.push_back("null");  break;
                case SLPTag::True:  out.push_back("true");  break;
                case SLPTag::False: out.push_back("false"); break;
                case SLPTag::Number: {
                    double d;
                    std::memcpy(&d, data + pos, 8);
                    out.push_back(formatDouble(d));
                    break;
                }
                case SLPTag::Integer: {
                    int32_t v;
                    std::memcpy(&v, data + pos, 4);
                    out.push_back(std::to_string(v));
                    break;
                }
                case SLPTag::LongString: case SLPTag::ShortString: case SLPTag::ByteString: {
                    uint32_t idx = 0;
                    std::memcpy(&idx, data + pos, width);
                    out.push_back(jsStringLiteral(strAt(ctx, idx)));
                    break;
                }
            }
            pos += width;
        }
    }
    return out;
}

// "" (never a valid literal, which is at least "[]"/"{}") means decode failure.
std::string renderArrayLiteral(uint32_t bufOffset, uint32_t count, const DecompileContext& ctx) {
    auto elems = decodeSLPElements(ctx.arrayBuffer, bufOffset, count, ctx);
    if (!elems) return "";
    std::string s = "[";
    for (size_t i = 0; i < elems->size(); i++) {
        if (i) s += ", ";
        s += (*elems)[i];
    }
    return s + "]";
}

std::string renderObjectLiteral(uint32_t keyBufOffset, uint32_t valBufOffset, uint32_t count,
                                const DecompileContext& ctx)
{
    auto keys = decodeSLPElements(ctx.objKeyBuffer, keyBufOffset, count, ctx);
    auto vals = decodeSLPElements(ctx.objValueBuffer, valBufOffset, count, ctx);
    if (!keys || !vals || keys->size() != vals->size()) return "";
    std::string s = "{";
    for (size_t i = 0; i < keys->size(); i++) {
        if (i) s += ", ";
        // Decoded keys are a quoted string or a bare integer: both valid as `key: value`.
        s += (*keys)[i] + ": " + (*vals)[i];
    }
    return s + "}";
}

} // namespace

std::string emitExpression(const Instruction& instr, const DecompileContext& ctx) {
    const auto& op = instr.operands;
    const std::string& mn = instr.mnemonic;

    auto reg = [&](size_t i) { return regName(ctx, op[i].u32); };

    // ---- Suppressed: VM bookkeeping with no JS equivalent ----
    // LoadParam/LoadThisNS destinations are already named this/param_N, so an
    // assignment would be redundant ("param_0 = param_0;").
    static const std::unordered_set<std::string> SUPPRESSED = {
        "LoadParam", "LoadParamLong", "LoadThisNS",
        "CreateEnvironment", "GetEnvironment",
        "ProfilePoint", "AsyncBreakCheck", "Debugger", "ReifyArguments",
        "Catch",  // the register is extracted directly by structureCFG
    };
    if (SUPPRESSED.count(mn)) return "";

    // ---- Binary arithmetic: dest = left <op> right ----
    static const std::unordered_map<std::string, std::string> BIN_ARITH = {
        {"Add", "+"}, {"AddN", "+"}, {"Sub", "-"}, {"SubN", "-"},
        {"Mul", "*"}, {"MulN", "*"}, {"Div", "/"}, {"DivN", "/"}, {"Mod", "%"},
        {"BitAnd", "&"}, {"BitOr", "|"}, {"BitXor", "^"},
        {"LShift", "<<"}, {"RShift", ">>"}, {"URshift", ">>>"},
    };
    {
        auto it = BIN_ARITH.find(mn);
        if (it != BIN_ARITH.end() && op.size() >= 3)
            return reg(0) + " = " + reg(1) + " " + it->second + " " + reg(2) + ";";
    }

    // ---- Comparisons: dest = left <op> right ----
    static const std::unordered_map<std::string, std::string> COMPARE = {
        {"Eq", "=="}, {"StrictEq", "==="}, {"Neq", "!="}, {"StrictNeq", "!=="},
        {"Less", "<"}, {"LessEq", "<="}, {"Greater", ">"}, {"GreaterEq", ">="},
    };
    {
        auto it = COMPARE.find(mn);
        if (it != COMPARE.end() && op.size() >= 3)
            return reg(0) + " = " + reg(1) + " " + it->second + " " + reg(2) + ";";
    }
    if (mn == "InstanceOf" && op.size() >= 3)
        return reg(0) + " = " + reg(1) + " instanceof " + reg(2) + ";";
    if (mn == "IsIn" && op.size() >= 3)
        return reg(0) + " = " + reg(1) + " in " + reg(2) + ";";

    // ---- Unary: dest = <op> src ----
    if (mn == "Negate" && op.size() >= 2) return reg(0) + " = -" + reg(1) + ";";
    if (mn == "Not" && op.size() >= 2)    return reg(0) + " = !" + reg(1) + ";";
    if (mn == "BitNot" && op.size() >= 2) return reg(0) + " = ~" + reg(1) + ";";
    if (mn == "TypeOf" && op.size() >= 2) return reg(0) + " = typeof " + reg(1) + ";";
    if (mn == "Inc" && op.size() >= 2)    return reg(0) + " = " + reg(1) + " + 1;";
    if (mn == "Dec" && op.size() >= 2)    return reg(0) + " = " + reg(1) + " - 1;";
    if ((mn == "ToNumber" || mn == "ToNumeric") && op.size() >= 2)
        return reg(0) + " = +" + reg(1) + ";";
    if (mn == "ToInt32" && op.size() >= 2) return reg(0) + " = " + reg(1) + " | 0;";
    if (mn == "CoerceThisNS" && op.size() >= 2) return reg(0) + " = " + reg(1) + ";";
    if (mn == "AddEmptyString" && op.size() >= 2) return reg(0) + " = \"\" + " + reg(1) + ";";
    if (mn == "ThrowIfEmpty" && op.size() >= 2) return reg(0) + " = " + reg(1) + ";";

    // ---- Mov ----
    if ((mn == "Mov" || mn == "MovLong") && op.size() >= 2)
        return reg(0) + " = " + reg(1) + ";";

    // ---- Constants ----
    if (mn == "LoadConstUInt8" && op.size() >= 2) return reg(0) + " = " + std::to_string(op[1].u32) + ";";
    if (mn == "LoadConstInt" && op.size() >= 2)    return reg(0) + " = " + std::to_string(op[1].i32) + ";";
    if (mn == "LoadConstDouble" && op.size() >= 2) return reg(0) + " = " + formatDouble(op[1].f64) + ";";
    if (mn == "LoadConstString" || mn == "LoadConstStringLongIndex") {
        if (op.size() >= 2) return reg(0) + " = " + jsStringLiteral(strAt(ctx, op[1].u32)) + ";";
    }
    if (mn == "LoadConstBigInt" || mn == "LoadConstBigIntLongIndex")
        return reg(0) + " = 0n; /* BigInt table #" + std::to_string(op.size() >= 2 ? op[1].u32 : 0) + ", not resolved */";
    if (mn == "LoadConstEmpty")     return reg(0) + " = undefined; /* <empty> */";
    if (mn == "LoadConstUndefined") return reg(0) + " = undefined;";
    if (mn == "LoadConstNull")      return reg(0) + " = null;";
    if (mn == "LoadConstTrue")      return reg(0) + " = true;";
    if (mn == "LoadConstFalse")     return reg(0) + " = false;";
    if (mn == "LoadConstZero")      return reg(0) + " = 0;";

    // ---- Objects / arrays ----
    if (mn == "NewObject") return reg(0) + " = {};";
    if (mn == "NewObjectWithParent" && op.size() >= 2) return reg(0) + " = Object.create(" + reg(1) + ");";
    if (mn == "NewObjectWithBuffer" || mn == "NewObjectWithBufferLong") {
        if (op.size() >= 5) {
            std::string lit = renderObjectLiteral(op[3].u32, op[4].u32, op[2].u32, ctx);
            if (!lit.empty()) return reg(0) + " = " + lit + ";";
        }
        return reg(0) + " = {}; /* buffer literal, content not resolved */";
    }
    if (mn == "NewArray") return reg(0) + " = [];";
    if (mn == "NewArrayWithBuffer" || mn == "NewArrayWithBufferLong") {
        if (op.size() >= 4) {
            std::string lit = renderArrayLiteral(op[3].u32, op[2].u32, ctx);
            if (!lit.empty()) return reg(0) + " = " + lit + ";";
        }
        return reg(0) + " = []; /* buffer literal, content not resolved */";
    }
    if (mn == "PutOwnByIndex" || mn == "PutOwnByIndexL") {
        if (op.size() >= 3) return reg(0) + "[" + std::to_string(op[2].u32) + "] = " + reg(1) + ";";
    }

    // ---- Property access by string (GetById*/PutById*) ----
    static const std::unordered_set<std::string> GET_BY_ID = {
        "GetByIdShort", "GetById", "GetByIdLong", "TryGetById", "TryGetByIdLong",
    };
    if (GET_BY_ID.count(mn) && op.size() >= 4)
        return reg(0) + " = " + propAccess(reg(1), strAt(ctx, op[3].u32)) + ";";

    static const std::unordered_set<std::string> PUT_BY_ID = {
        "PutById", "PutByIdLong", "TryPutById", "TryPutByIdLong",
    };
    if (PUT_BY_ID.count(mn) && op.size() >= 4)
        return propAccess(reg(0), strAt(ctx, op[3].u32)) + " = " + reg(1) + ";";

    static const std::unordered_set<std::string> PUT_NEW_OWN_BY_ID = {
        "PutNewOwnByIdShort", "PutNewOwnById", "PutNewOwnByIdLong",
        "PutNewOwnNEById", "PutNewOwnNEByIdLong",
    };
    if (PUT_NEW_OWN_BY_ID.count(mn) && op.size() >= 3)
        return propAccess(reg(0), strAt(ctx, op[2].u32)) + " = " + reg(1) + ";";

    if ((mn == "DelById" || mn == "DelByIdLong") && op.size() >= 3)
        return reg(0) + " = delete " + propAccess(reg(1), strAt(ctx, op[2].u32)) + ";";

    // ---- Property access by value (GetByVal/PutByVal) ----
    if (mn == "GetByVal" && op.size() >= 3)
        return reg(0) + " = " + reg(1) + "[" + reg(2) + "];";
    if ((mn == "PutByVal" || mn == "PutOwnByVal") && op.size() >= 3)
        return reg(0) + "[" + reg(1) + "] = " + reg(2) + ";";
    if (mn == "DelByVal" && op.size() >= 3)
        return reg(0) + " = delete " + reg(1) + "[" + reg(2) + "];";

    // ---- Calls ----
    if ((mn == "Call1" || mn == "Call2" || mn == "Call3" || mn == "Call4") && op.size() >= 3) {
        std::string args;
        for (size_t i = 2; i < op.size(); i++) {
            if (i > 2) args += ", ";
            args += reg(i);
        }
        return reg(0) + " = " + reg(1) + "(" + args + ");";
    }
    if ((mn == "Call" || mn == "CallLong") && op.size() >= 3)
        return reg(0) + " = " + reg(1) + "(/* " + std::to_string(op[2].u32) + " arg(s), not resolved */);";
    if ((mn == "Construct" || mn == "ConstructLong") && op.size() >= 3)
        return reg(0) + " = new " + reg(1) + "(/* " + std::to_string(op[2].u32) + " arg(s), not resolved */);";
    if ((mn == "CallDirect" || mn == "CallDirectLongIndex") && op.size() >= 3)
        return reg(0) + " = " + funcRefName(ctx, op[2].u32) + "(/* " + std::to_string(op[1].u32) + " arg(s), not resolved */);";
    // The callee must stay a valid identifier once comments are stripped:
    // "= /* comment */(...)" becomes "= (...)", a SyntaxError (seen on ridemovi.hbc).
    if ((mn == "CallBuiltin" || mn == "CallBuiltinLong") && op.size() >= 3)
        return reg(0) + " = builtin_" + std::to_string(op[1].u32) +
               "(/* " + std::to_string(op[2].u32) + " arg(s), not resolved */);";
    if (mn == "GetBuiltinClosure" && op.size() >= 2)
        return reg(0) + " = builtin_" + std::to_string(op[1].u32) + ";";

    // ---- Closures ----
    static const std::unordered_set<std::string> CREATE_CLOSURE = {
        "CreateClosure", "CreateClosureLongIndex",
        "CreateGeneratorClosure", "CreateGeneratorClosureLongIndex",
        "CreateAsyncClosure", "CreateAsyncClosureLongIndex",
        "CreateGenerator", "CreateGeneratorLongIndex",
    };
    if (CREATE_CLOSURE.count(mn) && op.size() >= 3)
        return reg(0) + " = " + funcRefName(ctx, op[2].u32) + ";";

    if (mn == "CreateThis" && op.size() >= 2)
        return reg(0) + " = Object.create(" + reg(1) + ");";
    if (mn == "SelectObject" && op.size() >= 3)
        return reg(0) + " = (" + reg(2) + " instanceof Object) ? " + reg(2) + " : " + reg(1) + ";";
    if (mn == "GetGlobalObject") return reg(0) + " = globalThis;";
    if (mn == "GetNewTarget")    return reg(0) + " = new.target;";

    // ---- Environment (closure-captured variables) ----
    // Slots of the function's own environment (ctx.ownEnvReg) are named via
    // scope_variables(); any other environment (an outer function's, from
    // GetEnvironment) falls back to envSlot_N, see decompiler.h.
    auto envSlotName = [&](uint32_t envReg, uint32_t slot) {
        if (static_cast<int64_t>(envReg) == ctx.ownEnvReg &&
            slot < ctx.envSlotNames.size() && isValidIdentifier(ctx.envSlotNames[slot]))
            return ctx.envSlotNames[slot];
        return "envSlot_" + std::to_string(slot);
    };
    if ((mn == "StoreToEnvironment" || mn == "StoreToEnvironmentL" ||
         mn == "StoreNPToEnvironment" || mn == "StoreNPToEnvironmentL") && op.size() >= 3)
        return envSlotName(op[0].u32, op[1].u32) + " = " + reg(2) + ";";
    if ((mn == "LoadFromEnvironment" || mn == "LoadFromEnvironmentL") && op.size() >= 3)
        return reg(0) + " = " + envSlotName(op[1].u32, op[2].u32) + ";";

    // ---- Miscellaneous ----
    if (mn == "DeclareGlobalVar" && op.size() >= 1)
        return "var " + strAt(ctx, op[0].u32) + ";";
    if (mn == "GetArgumentsLength" && op.size() >= 2)
        return reg(0) + " = arguments.length;";
    if (mn == "GetArgumentsPropByVal" && op.size() >= 3)
        return reg(0) + " = arguments[" + reg(1) + "];";
    if (mn == "DirectEval" && op.size() >= 2)
        return reg(0) + " = eval(" + reg(1) + ");";
    if (mn == "CreateRegExp" && op.size() >= 3)
        return reg(0) + " = /" + strAt(ctx, op[1].u32) + "/" + strAt(ctx, op[2].u32) + ";";

    // ---- "Leaf" control flow (Ret/Throw - Jmp* is handled by structureCFG) ----
    if (mn == "Ret" && op.size() >= 1)   return "return " + reg(0) + ";";
    if (mn == "Throw" && op.size() >= 1) return "throw " + reg(0) + ";";
    if (mn == "Unreachable")             return "throw new Error(\"unreachable\");";

    // ---- Fallback: unmapped mnemonic - raw comment, never a crash ----
    std::string raw = "/* " + mn;
    for (const auto& o : op) {
        raw += " ";
        switch (o.type) {
            case Operand::Type::StringID: raw += jsStringLiteral(strAt(ctx, o.u32)); break;
            case Operand::Type::Double:   raw += formatDouble(o.f64); break;
            case Operand::Type::Imm32:    raw += std::to_string(o.i32); break;
            case Operand::Type::Reg8:
            case Operand::Type::Reg32:    raw += regName(ctx, o.u32); break;
            default:                      raw += std::to_string(o.u32);
        }
    }
    raw += " */";
    return raw;
}

// =============================================================================
// structureCFG()
// =============================================================================

namespace {

constexpr uint32_t NO_BLOCK = std::numeric_limits<uint32_t>::max();

// Nearest preceding source location of an address (same strategy as
// cli/display.py's build_src_lookup/lookup_src_line).
struct SrcLookup {
    std::vector<uint32_t> addrs;
    std::vector<SourceLocation> locs;  // parallel to addrs, sorted by address

    explicit SrcLookup(const std::vector<SourceLocation>& sl) {
        addrs.reserve(sl.size());
        locs.reserve(sl.size());
        for (const auto& s : sl) {
            addrs.push_back(s.address);
            locs.push_back(s);
        }
    }

    const SourceLocation* lookup(uint32_t addr) const {
        if (addrs.empty()) return nullptr;
        auto it = std::upper_bound(addrs.begin(), addrs.end(), addr);
        if (it == addrs.begin()) return nullptr;
        return &locs[static_cast<size_t>((it - addrs.begin()) - 1)];
    }
};

// Jumps (Jmp*, J<compare>*) and SwitchImm have no line of their own: the
// if/while/try structure already expresses them. Every HBC jump mnemonic
// starts with 'J'.
bool isBranchMnemonic(const std::string& mn) {
    return !mn.empty() && mn[0] == 'J';
}

std::string indentStr(int level) {
    return std::string(static_cast<size_t>(level) * 2, ' ');
}

// JS boolean expression for the last instruction of a conditional block.
// Invariant: true exactly when the CondTaken edge is followed, so callers can
// always write `if (cond) { <CondTaken> } else { <CondFallThru> }` whatever
// the jump mnemonic.
std::string conditionText(const Instruction& instr, const DecompileContext& ctx) {
    const auto& op = instr.operands;
    const std::string& mn = instr.mnemonic;
    auto reg = [&](size_t i) { return regName(ctx, op[i].u32); };

    if (op.size() < 2) return "/* " + mn + " */true";

    if (mn.rfind("JmpTrue", 0) == 0)      return reg(1);
    if (mn.rfind("JmpFalse", 0) == 0)     return "!(" + reg(1) + ")";
    if (mn.rfind("JmpUndefined", 0) == 0) return reg(1) + " === undefined";

    if (op.size() < 3) return "/* " + mn + " */true";

    // The "Long" variants (Addr32 target) have the same semantics.
    std::string base = mn;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, "Long") == 0)
        base = base.substr(0, base.size() - 4);

    // {symbol, negated}: "Not<cmp>" mnemonics negate the base comparison.
    static const std::unordered_map<std::string, std::pair<std::string, bool>> J_CMP = {
        {"JLess", {"<", false}},          {"JLessN", {"<", false}},
        {"JNotLess", {"<", true}},        {"JNotLessN", {"<", true}},
        {"JLessEqual", {"<=", false}},    {"JLessEqualN", {"<=", false}},
        {"JNotLessEqual", {"<=", true}},  {"JNotLessEqualN", {"<=", true}},
        {"JGreater", {">", false}},       {"JGreaterN", {">", false}},
        {"JNotGreater", {">", true}},     {"JNotGreaterN", {">", true}},
        {"JGreaterEqual", {">=", false}}, {"JGreaterEqualN", {">=", false}},
        {"JNotGreaterEqual", {">=", true}}, {"JNotGreaterEqualN", {">=", true}},
        {"JEqual", {"==", false}},        {"JNotEqual", {"==", true}},
        {"JStrictEqual", {"===", false}}, {"JStrictNotEqual", {"===", true}},
    };
    auto it = J_CMP.find(base);
    if (it != J_CMP.end()) {
        std::string cmp = reg(1) + " " + it->second.first + " " + reg(2);
        return it->second.second ? ("!(" + cmp + ")") : cmp;
    }
    return "/* " + mn + " */true";
}

uint32_t findBlockAtOffset(const CFGData& cfg, uint32_t offset) {
    for (const auto& bb : cfg.blocks)
        if (bb.startOffset == offset) return bb.id;
    return NO_BLOCK;
}

// Merges the handlers sharing a target into one [min(start), max(end)) region.
// Hermes often emits several entries for one `catch` (e.g. one for the
// condition check, which excludes the comparison itself, and one for the
// branch that throws): same target, disjoint ranges that together cover the
// try body. Merged, the try start can be found by CONTAINMENT on the first
// reachable block, even if that block also holds preceding "safe" instructions
// (e.g. LoadParam); wrapping them in try{} is harmless.
std::vector<ExceptionHandlerInfo> mergeHandlersByTarget(
    const std::vector<ExceptionHandlerInfo>& handlers)
{
    std::vector<ExceptionHandlerInfo> merged;
    for (const auto& eh : handlers) {
        auto it = std::find_if(merged.begin(), merged.end(),
            [&](const ExceptionHandlerInfo& m) { return m.target == eh.target; });
        if (it == merged.end()) {
            merged.push_back(eh);
        } else {
            it->start = std::min(it->start, eh.start);
            it->end   = std::max(it->end, eh.end);
        }
    }
    return merged;
}

// Approximates the immediate post-dominator of two branches with the first
// block reachable from both (BFS over forward edges only: BackEdge and
// ExceptionHandler are not part of the flow being structured).
uint32_t findJoin(const CFGData& cfg, uint32_t a, uint32_t b) {
    auto reachable = [&](uint32_t start) {
        std::vector<bool> seen(cfg.blocks.size(), false);
        std::vector<uint32_t> order;
        std::vector<uint32_t> queue{start};
        seen[start] = true;
        for (size_t qi = 0; qi < queue.size(); qi++) {
            uint32_t id = queue[qi];
            order.push_back(id);
            for (const auto& e : cfg.blocks[id].successors) {
                if (e.kind == EdgeKind::BackEdge || e.kind == EdgeKind::ExceptionHandler) continue;
                if (!seen[e.targetId]) { seen[e.targetId] = true; queue.push_back(e.targetId); }
            }
        }
        return std::make_pair(seen, order);
    };
    auto [seenA, orderA] = reachable(a);
    auto [seenB, orderB] = reachable(b);
    for (uint32_t id : orderA)
        if (seenB[id]) return id;
    return NO_BLOCK;
}

struct CondEdges {
    const CFGEdge* taken    = nullptr;
    const CFGEdge* fallthru = nullptr;
};

CondEdges condEdges(const BasicBlock& bb) {
    CondEdges ce;
    for (const auto& e : bb.successors) {
        if (e.kind == EdgeKind::CondTaken) ce.taken = &e;
        if (e.kind == EdgeKind::CondFallThru) ce.fallthru = &e;
    }
    return ce;
}

// Rebuilds the structure of the CFG region by region. Every block is visited
// at most once; whatever is left unvisited is dumped by structureCFG().
class Structurer {
public:
    Structurer(const CFGData& cfg, std::vector<ExceptionHandlerInfo> handlers,
               const DecompileContext& ctx, const SrcLookup& src)
        : cfg_(cfg), handlers_(std::move(handlers)), ctx_(ctx), src_(src),
          visited_(cfg.blocks.size(), false) {}

    bool visited(uint32_t id) const { return visited_[id]; }

    // Emits the blocks from startId up to (excluding) stopId.
    // skipTryCheckOnFirst: don't let the first block open a try again. Set by
    // the recursion over a try's own body, which restarts from the block that
    // opened it and would otherwise recognize it forever.
    std::string emitRange(uint32_t startId, uint32_t stopId, int indent,
                          bool skipTryCheckOnFirst = false)
    {
        std::string out;
        std::string ind = indentStr(indent);
        uint32_t cur = startId;
        bool firstIteration = true;

        for (;;) {
            if (cur == NO_BLOCK || cur == stopId || cur >= cfg_.blocks.size()) break;

            // The first block of a try-body recursion is already marked
            // visited by the caller: bypass the guard for that iteration only.
            const bool skipFirstChecks = firstIteration && skipTryCheckOnFirst;
            if (!skipFirstChecks && visited_[cur]) break;
            firstIteration = false;

            const BasicBlock& bb = cfg_.blocks[cur];

            if (!skipFirstChecks) {
                const int ehIdx = findOpeningHandler(bb);
                if (ehIdx >= 0) {
                    out += emitTry(cur, static_cast<size_t>(ehIdx), indent, cur);
                    continue;
                }
            }

            visited_[cur] = true;

            // do-while: the block has a back-edge to itself.
            const CFGEdge* selfBack = nullptr;
            const CFGEdge* exitEdge = nullptr;
            for (const auto& e : bb.successors) {
                if (e.kind == EdgeKind::BackEdge && e.targetId == cur) selfBack = &e;
                else if (e.kind == EdgeKind::CondFallThru || e.kind == EdgeKind::FallThrough) exitEdge = &e;
            }
            if (selfBack && !bb.instructions.empty()) {
                out += ind + "do {\n";
                out += emitBlock(bb, indent + 1, /*skipLastIfBranch=*/true);
                out += ind + "} while (" + conditionText(bb.instructions.back(), ctx_) + ");\n";
                cur = exitEdge ? exitEdge->targetId : NO_BLOCK;
                continue;
            }

            const CondEdges ce = condEdges(bb);

            // while: loop header with the test on top, body between CondTaken and the back-edge.
            if (bb.isLoopHeader) {
                if (ce.taken && ce.fallthru && !bb.instructions.empty()) {
                    out += emitBlock(bb, indent, /*skipLastIfBranch=*/true);
                    out += ind + "while (" + conditionText(bb.instructions.back(), ctx_) + ") {\n";
                    out += emitRange(ce.taken->targetId, cur, indent + 1);
                    out += ind + "}\n";
                    cur = ce.fallthru->targetId;
                    continue;
                }
                // Unrecognized shape (e.g. unconditional back-edge): the rest of
                // the body is picked up by structureCFG()'s final dump.
                out += emitBlock(bb, indent, false);
                out += ind + "while (true) { /* body not fully structured, see below */ }\n";
                cur = NO_BLOCK;
                continue;
            }

            // Terminal block (Ret/Throw). A block inside a try always has an
            // ExceptionHandler edge, which isn't a continuation of normal flow.
            const bool hasRealSuccessor = std::any_of(bb.successors.begin(), bb.successors.end(),
                [](const CFGEdge& e) { return e.kind != EdgeKind::ExceptionHandler; });
            if (!hasRealSuccessor) {
                out += emitBlock(bb, indent, false);
                cur = NO_BLOCK;
                continue;
            }

            // if / if-else
            if (ce.taken && ce.fallthru && !bb.instructions.empty()) {
                out += emitBlock(bb, indent, /*skipLastIfBranch=*/true);
                const uint32_t joinId = findJoin(cfg_, ce.taken->targetId, ce.fallthru->targetId);
                out += ind + "if (" + conditionText(bb.instructions.back(), ctx_) + ") {\n";
                out += emitRange(ce.taken->targetId, joinId, indent + 1);
                if (ce.fallthru->targetId != joinId) {
                    out += ind + "} else {\n";
                    out += emitRange(ce.fallthru->targetId, joinId, indent + 1);
                }
                out += ind + "}\n";
                cur = joinId;
                continue;
            }

            // Linear continuation: a single Jump/FallThrough/BackEdge successor.
            if (bb.successors.size() == 1 &&
                (bb.successors[0].kind == EdgeKind::Jump ||
                 bb.successors[0].kind == EdgeKind::FallThrough ||
                 bb.successors[0].kind == EdgeKind::BackEdge)) {
                out += emitBlock(bb, indent, true);
                cur = bb.successors[0].targetId;
                continue;
            }

            // Switch or irregular CFG: raw dump, no recursion. Blocks reached
            // via SwitchCase/SwitchDefault stay unvisited and are collected by
            // structureCFG(): no instruction is lost, just not nested in a switch{}.
            out += ind + "// unstructured region: B" + std::to_string(cur) + "\n";
            out += emitBlock(bb, indent, false);
            cur = NO_BLOCK;
        }
        return out;
    }

    // Emits the unvisited blocks one by one (see structureCFG).
    std::string emitUnvisited(int indent) {
        std::string out;
        for (uint32_t i = 0; i < cfg_.blocks.size(); i++) {
            if (visited_[i]) continue;
            out += indentStr(indent) + "// unstructured region: B" + std::to_string(i) + "\n";
            out += emitBlock(cfg_.blocks[i], indent, false);
        }
        return out;
    }

private:
    // Index of the handler whose try range starts inside `bb` (by containment,
    // see mergeHandlersByTarget), or -1.
    int findOpeningHandler(const BasicBlock& bb) const {
        for (size_t i = 0; i < handlers_.size(); i++)
            if (bb.startOffset <= handlers_[i].start && handlers_[i].start < bb.endOffset)
                return static_cast<int>(i);
        return -1;
    }

    // try { body } catch (e) { handler }. `next` receives the block where
    // normal flow resumes after the construct.
    std::string emitTry(uint32_t cur, size_t ehIdx, int indent, uint32_t& next) {
        // Mark visited BEFORE recursing: the body recursion restarts from this
        // same block with skipTryCheckOnFirst, so it runs the normal dispatch.
        visited_[cur] = true;

        const ExceptionHandlerInfo& eh = handlers_[ehIdx];
        const uint32_t catchId = findBlockAtOffset(cfg_, eh.target);
        // Re-entry point: first block reachable from both the try body and the
        // catch body (same technique as the if/else join).
        const uint32_t afterId = (catchId != NO_BLOCK) ? findJoin(cfg_, cur, catchId) : NO_BLOCK;
        const std::string ind = indentStr(indent);

        std::string out = ind + "try {\n";
        out += emitRange(cur, afterId, indent + 1, /*skipTryCheckOnFirst=*/true);

        std::string catchReg = "e";
        if (catchId != NO_BLOCK && !cfg_.blocks[catchId].instructions.empty()) {
            const Instruction& first = cfg_.blocks[catchId].instructions[0];
            if (first.mnemonic == "Catch" && !first.operands.empty())
                catchReg = regName(ctx_, first.operands[0].u32);
        }
        out += ind + "} catch (" + catchReg + ") {\n";
        if (catchId != NO_BLOCK)
            out += emitRange(catchId, afterId, indent + 1);
        out += ind + "}\n";

        next = afterId;
        return out;
    }

    // Instructions of a block, with a "// line N" marker when the source line changes.
    std::string emitBlock(const BasicBlock& bb, int indent, bool skipLastIfBranch) const {
        std::string out;
        const std::string ind = indentStr(indent);
        bool havePrevLine = false;
        uint32_t prevLine = 0;
        const size_t n = bb.instructions.size();

        for (size_t i = 0; i < n; i++) {
            const Instruction& instr = bb.instructions[i];
            if (instr.mnemonic == "SwitchImm") continue;  // not structured, see emitRange
            if (skipLastIfBranch && i + 1 == n && isBranchMnemonic(instr.mnemonic)) continue;

            const SourceLocation* loc = src_.lookup(instr.offset);
            if (loc && (!havePrevLine || loc->line != prevLine)) {
                out += ind + "// line " + std::to_string(loc->line) + "\n";
                prevLine = loc->line;
                havePrevLine = true;
            }

            const std::string line = emitExpression(instr, ctx_);
            if (!line.empty()) out += ind + line + "\n";
        }
        return out;
    }

    const CFGData& cfg_;
    std::vector<ExceptionHandlerInfo> handlers_;
    const DecompileContext& ctx_;
    const SrcLookup& src_;
    std::vector<bool> visited_;
};

} // namespace

std::string structureCFG(
    const CFGData& cfg,
    const std::vector<ExceptionHandlerInfo>& exceptionHandlers,
    const std::vector<SourceLocation>& srcLocs,
    const DecompileContext& ctx)
{
    if (cfg.blocks.empty()) return "";

    SrcLookup srcLookup(srcLocs);
    Structurer structurer(cfg, mergeHandlersByTarget(exceptionHandlers), ctx, srcLookup);

    std::string out = structurer.emitRange(cfg.entryId, NO_BLOCK, 1);
    // Blocks the structuring never reached (switch, irregular CFG) are still
    // emitted, so no instruction is silently lost.
    out += structurer.emitUnvisited(1);
    return out;
}

// =============================================================================
// HBCFile::decompile()
// =============================================================================

namespace {

// Register names: rN by default, `this` / param_N from LoadParam/LoadThisNS.
std::vector<std::string> buildRegNames(const FunctionInfo& fi, const std::vector<Instruction>& instrs) {
    uint32_t maxReg = fi.frameSize;
    for (const auto& instr : instrs)
        for (const auto& op : instr.operands)
            if ((op.type == Operand::Type::Reg8 || op.type == Operand::Type::Reg32) &&
                op.u32 + 1 > maxReg)
                maxReg = op.u32 + 1;

    std::vector<std::string> regNames(maxReg);
    for (uint32_t i = 0; i < maxReg; i++) regNames[i] = "r" + std::to_string(i);

    for (const auto& instr : instrs) {
        if ((instr.mnemonic == "LoadParam" || instr.mnemonic == "LoadParamLong") &&
            instr.operands.size() >= 2) {
            uint32_t dest = instr.operands[0].u32;
            uint32_t idx  = instr.operands[1].u32;
            if (dest < regNames.size())
                regNames[dest] = (idx == 0) ? "this" : ("param_" + std::to_string(idx - 1));
        } else if (instr.mnemonic == "LoadThisNS" && !instr.operands.empty()) {
            uint32_t dest = instr.operands[0].u32;
            if (dest < regNames.size()) regNames[dest] = "this";
        }
    }

    // Hermes reuses registers aggressively: one named "this" can later be
    // overwritten with an unrelated value (seen on ridemovi.hbc, not in the
    // synthetic corpus). Emitting `this = <value>;` is a SyntaxError in any
    // mode, so such registers go back to rN. This is NOT done for param_N:
    // assigning to a parameter is valid JS, and the name stays more readable.
    static const std::unordered_set<std::string> NO_DEST_REG0 = {
        "Ret", "Throw",
        "PutById", "PutByIdLong", "TryPutById", "TryPutByIdLong",
        "PutNewOwnByIdShort", "PutNewOwnById", "PutNewOwnByIdLong",
        "PutNewOwnNEById", "PutNewOwnNEByIdLong",
        "PutByVal", "PutOwnByVal", "PutOwnByIndex", "PutOwnByIndexL",
        "StoreToEnvironment", "StoreToEnvironmentL",
        "StoreNPToEnvironment", "StoreNPToEnvironmentL",
        "SwitchImm",
    };
    for (const auto& instr : instrs) {
        if (instr.mnemonic == "LoadParam" || instr.mnemonic == "LoadParamLong" ||
            instr.mnemonic == "LoadThisNS" || NO_DEST_REG0.count(instr.mnemonic) ||
            instr.operands.empty())
            continue;
        const auto& op0 = instr.operands[0];
        if (op0.type != Operand::Type::Reg8 && op0.type != Operand::Type::Reg32) continue;
        uint32_t r = op0.u32;
        if (r < regNames.size() && regNames[r] == "this")
            regNames[r] = "r" + std::to_string(r);
    }
    return regNames;
}

// Name for a function in its own header: anonymous or non-identifier names become func_N.
std::string headerName(const FunctionInfo& fi, uint32_t functionIndex) {
    if (fi.name.rfind("anonymous_func_", 0) == 0 || !isValidIdentifier(fi.name))
        return "func_" + std::to_string(functionIndex);
    return fi.name;
}

} // namespace

std::string HBCFile::decompile(uint32_t functionIndex) const {
    const auto& funcs = functions();
    if (functionIndex >= funcs.size()) {
        throw std::out_of_range(
            "decompile: function index " + std::to_string(functionIndex) +
            " out of range (functionCount=" + std::to_string(funcs.size()) + ")");
    }
    const FunctionInfo& fi = funcs[functionIndex];

    // "show source" directive (see Function Source Table): Hermes kept the
    // function's original source, which beats any decompilation.
    if (fi.hasPreservedSource && !fi.sourceIsHidden) {
        std::ostringstream out;
        out << "// func[" << functionIndex << "] " << fi.name
            << " \xe2\x80\x94 ORIGINAL SOURCE, NOT decompiled\n";
        out << "// Recovered verbatim from Hermes' Function Source Table "
               "(\"show source\" directive) \xe2\x80\x94 comments and formatting are the author's.\n";
        out << fi.preservedSource << "\n";
        return out.str();
    }

    const auto instrs  = disasm(functionIndex);
    const auto cfgData = cfg(functionIndex);

    const auto regNames = buildRegNames(fi, instrs);

    // Names of the function's OWN scope variables, and the register of its
    // environment (from its first CreateEnvironment). Outer scopes aren't resolved.
    std::vector<std::string> envSlotNames;
    if (fi.hasDebugInfo)
        for (const auto& v : scope_variables(functionIndex)) envSlotNames.push_back(v.name);

    int64_t ownEnvReg = -1;
    for (const auto& instr : instrs) {
        if (instr.mnemonic == "CreateEnvironment" && !instr.operands.empty()) {
            ownEnvReg = instr.operands[0].u32;
            break;
        }
    }

    DecompileContext ctx{regNames, funcs, strings(), envSlotNames, ownEnvReg,
                         array_buffer(), obj_key_buffer(), obj_value_buffer()};

    std::vector<SourceLocation> srcLocs;
    if (fi.hasDebugInfo) srcLocs = source_locations(functionIndex);

    // Declare the scope variables, or `count = ...;` would assign to an
    // undeclared identifier (ReferenceError in strict mode).
    std::string body;
    std::string declared;
    for (const auto& name : envSlotNames) {
        if (!isValidIdentifier(name)) continue;
        if (!declared.empty()) declared += ", ";
        declared += name;
    }
    if (!declared.empty()) body += "  let " + declared + ";\n";
    body += structureCFG(cfgData, fi.exceptionHandlers, srcLocs, ctx);

    const uint32_t jsParams = fi.paramCount > 0 ? fi.paramCount - 1 : 0;  // paramCount includes `this`
    std::string params;
    for (uint32_t i = 0; i < jsParams; i++) {
        if (i > 0) params += ", ";
        params += "param_" + std::to_string(i);
    }

    // CallDirect is rare in RN: user functions are mostly closures (CreateClosure*).
    bool calledDirectly = false;
    for (const auto& cs : call_graph())
        if (cs.calleeIndex == functionIndex && cs.isDirect) { calledDirectly = true; break; }

    int32_t moduleId = -1;
    for (const auto& mod : recover_modules())
        if (mod.functionIndex == functionIndex) { moduleId = static_cast<int32_t>(mod.moduleId); break; }

    const std::string name = headerName(fi, functionIndex);

    std::ostringstream out;
    out << "// func[" << functionIndex << "] " << fi.name << " \xe2\x80\x94 decompiled by hermit\n";
    out << "// paramCount = " << fi.paramCount << " (this + " << jsParams
        << (jsParams == 1 ? " param" : " params") << "), JS params = " << jsParams << "\n";
    if (moduleId >= 0) out << "// module #" << moduleId << "\n";

    if (calledDirectly)
        out << "function " << name << "(" << params << ") {\n";
    else
        out << "const " << name << " = function(" << params << ") {\n";
    out << body;
    out << "}\n";

    return out.str();
}

} // namespace hbc
