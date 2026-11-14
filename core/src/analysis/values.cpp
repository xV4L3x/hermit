// Value analysis (HBCFile::value_flow() and friends).
//
// A forward dataflow per function tracks what each register provably holds
// (AbstractValue). It runs per group of functions sharing environments: a Metro
// module (factory + nested functions) or the functions outside every module.
// Environment slots are the bridge between functions: a slot whose stores all
// agree on one value yields that value at every load (env_accesses() resolves
// which slot an instruction touches). Slot values feed back into the functions,
// so the group is re-analyzed until the slot table is stable.
//
// Metro rules (inside a module only):
//   LoadParam i in the factory                 -> FactoryParam(i)
//   dependencyMap[k]                            -> DepIndex(k)
//   require(DepIndex(k))                        -> Module(dependencyMap[k])
//   M(Module(X)), M's module.exports being Babel's _interopRequireDefault
//                                               -> ImportDefault(X)
//   PutById(module, "exports", v)                -> the group's module.exports
// Parameters: LoadParam i in a function of the group gets the value every call
// of its closure passes (IIFE / UMD wrappers like `function (exports) {...}(m)`),
// only when the closure never escapes (it is only called, moved, or kept in an
// environment slot) so no other caller can exist.
// Exports (module_exports()): stores on `exports` / `module.exports`, and
// Object.defineProperty(exports, "X", descriptor) with the descriptor's object
// literal fields (`get` closure -> the value its returns agree on, `value`).

#include "analysis/analysis.h"
#include "analysis/dataflow.h"
#include "util/slp.h"
#include "format/opcode_categories.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace hbc {

// ---------------------------------------------------------------------------
// AbstractValue
// ---------------------------------------------------------------------------

bool AbstractValue::operator==(const AbstractValue& o) const {
    if (kind != o.kind || index != o.index || name != o.name) return false;
    if (kind == Kind::Number || kind == Kind::Boolean) {
        if (!(number == o.number || (std::isnan(number) && std::isnan(o.number)))) return false;
    }
    if (static_cast<bool>(base) != static_cast<bool>(o.base)) return false;
    return !base || *base == *o.base;
}

namespace {

bool isIdentifier(const std::string& s) {
    if (s.empty()) return false;
    auto ok0 = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; };
    if (!ok0(s[0])) return false;
    for (char c : s)
        if (!(ok0(c) || std::isdigit(static_cast<unsigned char>(c)))) return false;
    return true;
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        out += c;
    }
    return out + "\"";
}

const char* factoryParamName(uint32_t i) {
    static const char* names[] = {"this", "global", "require", "importDefault",
                                  "importAll", "module", "exports", "dependencyMap"};
    return i < 8 ? names[i] : "param";
}

} // namespace

std::string AbstractValue::toString() const {
    switch (kind) {
        case Kind::GlobalObject:  return "globalThis";
        case Kind::Global:        return name;
        case Kind::FactoryParam:  return factoryParamName(index);
        case Kind::Closure:       return "func_" + std::to_string(index);
        case Kind::Module:        return "module_" + std::to_string(index);
        case Kind::ImportDefault: return "importDefaultOf(module_" + std::to_string(index) + ")";
        case Kind::DepIndex:      return "dependencyMap[" + std::to_string(index) + "]";
        case Kind::Prop: {
            const std::string b = base ? base->toString() : "?";
            return isIdentifier(name) ? b + "." + name : b + "[" + quoted(name) + "]";
        }
        case Kind::Number: {
            std::ostringstream os;
            os << number;
            return os.str();
        }
        case Kind::Object:    return "{...}";
        case Kind::String:    return quoted(name);
        case Kind::Undefined: return "undefined";
        case Kind::Null:      return "null";
        case Kind::Boolean:   return number != 0 ? "true" : "false";
    }
    return "?";
}

namespace {

using V     = AbstractValue;
using VKind = AbstractValue::Kind;
using RegState = std::map<uint32_t, V>;
using SlotKey  = std::pair<EnvKey, uint32_t>;
using ParamKey = std::pair<uint32_t, uint32_t>;  // (function, parameter index)

constexpr int kMaxPropDepth = 6;

int propDepth(const V& v) {
    int d = 0;
    for (const V* p = &v; p && p->kind == VKind::Prop; p = p->base.get()) ++d;
    return d;
}

V make(VKind k, uint32_t index = 0) { V v; v.kind = k; v.index = index; return v; }
V makeNum(double d) { V v; v.kind = VKind::Number; v.number = d; return v; }
V makeStr(VKind k, std::string s) { V v; v.kind = k; v.name = std::move(s); return v; }
V makeProp(const V& base, const std::string& name) {
    V v; v.kind = VKind::Prop; v.name = name; v.base = std::make_shared<const V>(base); return v;
}

// Everything the transfer function needs besides the register state.
struct Ctx {
    const HBCFile&                     file;
    uint32_t                           function;
    const std::vector<std::string>&    strings;
    const MetroModule*                  module;     // nullptr outside modules
    bool                               isFactory;  // the function is the module's factory
    const std::map<SlotKey, V>&        slots;      // agreed slot values
    const std::map<ParamKey, V>&       params;     // agreed parameter values
    const std::unordered_map<uint32_t, EnvAccess>& envAt;  // offset -> resolved access
    const std::function<bool(uint32_t)>& isInteropHelper;  // module id -> bool
};

std::optional<V> regValue(const RegState& st, const Operand& op) {
    if (!isRegisterOperand(op)) return std::nullopt;
    auto it = st.find(op.u32);
    if (it == st.end()) return std::nullopt;
    return it->second;
}

// Value of the destination of `instr`, from the state before it.
std::optional<V> resultOf(const Instruction& instr, const RegState& st, const Ctx& c) {
    const auto& ops = instr.operands;
    const std::string& mn = instr.mnemonic;
    auto r = [&](size_t i) { return i < ops.size() ? regValue(st, ops[i]) : std::nullopt; };

    if (mn == "Mov" || mn == "MovLong") return r(1);
    if (mn == "GetGlobalObject") return make(VKind::GlobalObject);
    if (mn == "LoadConstZero") return makeNum(0);
    if (mn == "LoadConstUInt8" && ops.size() >= 2) return makeNum(ops[1].u32);
    if (mn == "LoadConstInt" && ops.size() >= 2) return makeNum(ops[1].i32);
    if (mn == "LoadConstDouble" && ops.size() >= 2) return makeNum(ops[1].f64);
    if (mn == "LoadConstUndefined") return make(VKind::Undefined);
    if (mn == "LoadConstNull") return make(VKind::Null);
    if (mn == "LoadConstTrue")  { V v = make(VKind::Boolean); v.number = 1; return v; }
    if (mn == "LoadConstFalse") return make(VKind::Boolean);
    if ((mn == "LoadConstString" || mn == "LoadConstStringLongIndex") && ops.size() >= 2 &&
        ops[1].u32 < c.strings.size())
        return makeStr(VKind::String, c.strings[ops[1].u32]);

    if ((mn == "LoadParam" || mn == "LoadParamLong") && ops.size() >= 2) {
        if (c.module && c.isFactory)
            return ops[1].u32 >= 1 && ops[1].u32 <= 7 ? std::optional<V>(make(VKind::FactoryParam, ops[1].u32))
                                                      : std::nullopt;
        auto p = c.params.find({c.function, ops[1].u32});
        if (p != c.params.end()) return p->second;
        return std::nullopt;
    }

    if (isClosureCreation(mn) && ops.size() >= 3) return make(VKind::Closure, ops[2].u32);
    if (mn == "NewObject" || mn == "NewObjectWithBuffer" || mn == "NewObjectWithBufferLong") {
        V v = make(VKind::Object, instr.offset);
        v.number = c.function;
        return v;
    }

    if (isGetById(mn) && ops.size() >= 4) {
        const auto obj = r(1);
        const uint32_t sid = ops[3].u32;
        if (sid >= c.strings.size()) return std::nullopt;
        const std::string& prop = c.strings[sid];
        if (mn.rfind("Try", 0) == 0 || (obj && obj->kind == VKind::GlobalObject))
            return makeStr(VKind::Global, prop);
        if (obj && propDepth(*obj) < kMaxPropDepth) return makeProp(*obj, prop);
        return std::nullopt;
    }

    if (mn == "GetByVal" && ops.size() >= 3) {
        const auto obj = r(1), key = r(2);
        if (obj && key && obj->kind == VKind::FactoryParam && obj->index == 7 &&
            key->kind == VKind::Number && key->number >= 0 && key->number == std::floor(key->number))
            return make(VKind::DepIndex, static_cast<uint32_t>(key->number));
        if (obj && key && key->kind == VKind::String && propDepth(*obj) < kMaxPropDepth)
            return makeProp(*obj, key->name);
        return std::nullopt;
    }

    if (isEnvLoad(mn)) {
        auto a = c.envAt.find(instr.offset);
        if (a == c.envAt.end() || !a->second.resolved) return std::nullopt;
        auto s = c.slots.find({a->second.env, a->second.slot});
        if (s == c.slots.end()) return std::nullopt;
        return s->second;
    }

    // Call2 dst, callee, this, arg: require(dependencyMap[k]) and the Babel
    // interop helper applied to a module.
    if (mn == "Call2" && ops.size() >= 4 && c.module) {
        const auto callee = r(1), arg = r(3);
        if (!callee || !arg) return std::nullopt;
        if (callee->kind == VKind::FactoryParam && callee->index == 2 &&
            arg->kind == VKind::DepIndex) {
            const auto& deps = c.module->dependencies;
            if (c.module->dependenciesKnown && arg->index < deps.size() && deps[arg->index])
                return make(VKind::Module, *deps[arg->index]);
            return std::nullopt;
        }
        if (callee->kind == VKind::Module && arg->kind == VKind::Module &&
            c.isInteropHelper(callee->index))
            return make(VKind::ImportDefault, arg->index);
    }
    return std::nullopt;
}

void apply(const Instruction& instr, RegState& st, const Ctx& c) {
    const auto v = resultOf(instr, st, c);
    const auto written = writtenRegisters(instr);
    for (uint32_t w : written) st.erase(w);
    if (v && !written.empty()) st[written.front()] = *v;
}

using ObjKey = std::pair<uint32_t, uint32_t>;  // (function, allocation offset)

// A field of an object literal: `name: value`, or `get name() {...}`
// (getter = the getter closure's function index).
struct ObjectField {
    ObjKey                  object;
    std::string             name;
    std::optional<V>        value;
    std::optional<uint32_t> getter;
};

// A store to `exports` (target 0) or `module.exports` (target 1).
struct ExportEvent {
    int                        target = 0;
    std::optional<std::string> name;        // nullopt: not a constant (dynamic)
    std::optional<V>           value;
    std::optional<V>           descriptor;  // defineProperty: the descriptor value
};

struct FunctionRun {
    std::vector<InstrValues>          flow;
    std::vector<std::pair<SlotKey, std::optional<V>>> stores;  // every slot store
    std::vector<std::optional<V>>     moduleExportStores;        // PutById(module, "exports", v)
    std::vector<ExportEvent>          exportEvents;
    std::vector<ObjectField>          objectFields;
    // Direct calls of a closure value: callee function -> argument values by
    // parameter index (0 = this); nullopt list = arguments not known (Call/CallLong).
    std::vector<std::pair<uint32_t, std::optional<std::vector<std::optional<V>>>>> calls;
    std::vector<uint32_t>             escapes;  // closures used other than called/moved/kept
};

bool isFactoryParam(const std::optional<V>& v, uint32_t i) {
    return v && v->kind == VKind::FactoryParam && v->index == i;
}

// `module.exports` (target 1), `exports` (target 0), or -1.
int exportTarget(const std::optional<V>& v) {
    if (isFactoryParam(v, 6)) return 0;
    if (v && v->kind == VKind::Prop && v->name == "exports" && v->base &&
        v->base->kind == VKind::FactoryParam && v->base->index == 5)
        return 1;
    return -1;
}

std::optional<ObjKey> objectKey(const std::optional<V>& v) {
    if (!v || v->kind != VKind::Object) return std::nullopt;
    return ObjKey{static_cast<uint32_t>(v->number), v->index};
}

bool isPutById(const std::string& mn) {
    return mn == "PutById" || mn == "PutByIdLong" || mn == "PutNewOwnById" ||
           mn == "PutNewOwnByIdShort" || mn == "PutNewOwnByIdLong" ||
           mn == "PutNewOwnNEById" || mn == "PutNewOwnNEByIdLong";
}

FunctionRun runFunction(uint32_t f, const Ctx& c) {
    FunctionRun run;
    const CFGData& cfg = c.file.cfg(f);
    const auto in = forwardDataflow(
        cfg, RegState{},
        [&](const BasicBlock& bb, RegState& st) {
            for (const auto& instr : bb.instructions) apply(instr, st, c);
        },
        [](RegState& into, const RegState& from) { return intersectEqual(into, from); });

    for (const auto& bb : cfg.blocks) {
        if (bb.id >= in.size() || !in[bb.id]) continue;
        RegState st = *in[bb.id];
        for (const auto& instr : bb.instructions) {
            const auto& ops = instr.operands;
            const std::string& mn = instr.mnemonic;

            InstrValues iv;
            iv.offset = instr.offset;
            bool any = false;
            iv.operands.reserve(ops.size());
            for (const auto& op : ops) {
                auto v = regValue(st, op);
                any |= v.has_value();
                iv.operands.push_back(std::move(v));
            }
            iv.result = resultOf(instr, st, c);
            any |= iv.result.has_value();

            if (isEnvStore(mn)) {
                auto a = c.envAt.find(instr.offset);
                if (a != c.envAt.end() && a->second.resolved && ops.size() >= 3)
                    run.stores.push_back({{a->second.env, a->second.slot}, regValue(st, ops[2])});
            }
            // Property stores: module.exports = v, exports.X = v,
            // module.exports.X = v, object literal fields.
            std::optional<V> storeObj, storeVal;
            std::optional<std::string> storeName;
            bool isStore = false;
            if (isPutById(mn) && ops.size() >= 3) {
                isStore = true;
                storeObj = regValue(st, ops[0]);
                storeVal = regValue(st, ops[1]);
                if (ops.back().u32 < c.strings.size()) storeName = c.strings[ops.back().u32];
            } else if ((mn == "PutByVal" || mn == "PutOwnByVal") && ops.size() >= 3) {
                isStore = true;
                storeObj = regValue(st, ops[0]);
                storeVal = regValue(st, ops[2]);
                const auto key = regValue(st, ops[1]);
                if (key && key->kind == VKind::String) storeName = key->name;
            }
            if (isStore) {
                if (isFactoryParam(storeObj, 5) && storeName == std::optional<std::string>("exports")) {
                    run.moduleExportStores.push_back(storeVal);
                } else if (const int t = exportTarget(storeObj); t >= 0) {
                    run.exportEvents.push_back({t, storeName, storeVal, std::nullopt});
                } else if (auto k = objectKey(storeObj); k && storeName) {
                    run.objectFields.push_back({*k, *storeName, storeVal, std::nullopt});
                }
            }
            // Object literal accessor: PutOwnGetterSetterByVal obj, key, getter, setter, enum
            if (mn == "PutOwnGetterSetterByVal" && ops.size() >= 4) {
                const auto obj = regValue(st, ops[0]), key = regValue(st, ops[1]),
                           get = regValue(st, ops[2]);
                if (auto k = objectKey(obj); k && key && key->kind == VKind::String)
                    run.objectFields.push_back(
                        {*k, key->name, std::nullopt,
                         get && get->kind == VKind::Closure ? std::optional<uint32_t>(get->index)
                                                            : std::nullopt});
            }
            // Object.defineProperty(target, name, descriptor)
            if (mn == "Call4" && ops.size() >= 6) {
                const auto callee = regValue(st, ops[1]);
                if (callee && callee->kind == VKind::Prop && callee->name == "defineProperty" &&
                    callee->base && callee->base->kind == VKind::Global &&
                    callee->base->name == "Object") {
                    const auto target = regValue(st, ops[3]);
                    if (const int t = exportTarget(target); t >= 0) {
                        const auto name = regValue(st, ops[4]);
                        ExportEvent ev{t, std::nullopt, std::nullopt, regValue(st, ops[5])};
                        if (name && name->kind == VKind::String) ev.name = name->name;
                        run.exportEvents.push_back(std::move(ev));
                    }
                }
            }
            // Closure uses: calls (with their arguments) and escapes.
            {
                const bool isCall = mn.rfind("Call", 0) == 0 || mn.rfind("Construct", 0) == 0;
                const auto written = writtenRegisters(instr);
                const bool writesOp0 = !written.empty() && !ops.empty() &&
                                       isRegisterOperand(ops[0]) && written.front() == ops[0].u32;
                for (size_t k = 0; k < ops.size(); ++k) {
                    if (k == 0 && writesOp0) continue;
                    const auto v = regValue(st, ops[k]);
                    if (!v || v->kind != VKind::Closure) continue;
                    const bool benign = (isCall && k == 1) ||
                                        ((mn == "Mov" || mn == "MovLong") && k == 1) ||
                                        (mn.find("ToEnvironment") != std::string::npos && k == 2);
                    if (!benign) run.escapes.push_back(v->index);
                }
                if (isCall && ops.size() >= 2) {
                    const auto callee = regValue(st, ops[1]);
                    if (callee && callee->kind == VKind::Closure) {
                        const bool fixedArgs = mn == "Call1" || mn == "Call2" || mn == "Call3" ||
                                               mn == "Call4";
                        std::optional<std::vector<std::optional<V>>> args;
                        if (fixedArgs) {
                            args.emplace();
                            for (size_t k = 2; k < ops.size(); ++k) args->push_back(regValue(st, ops[k]));
                        }
                        run.calls.emplace_back(callee->index, std::move(args));
                    }
                }
            }
            // Constant fields of NewObjectWithBuffer literals.
            if ((mn == "NewObjectWithBuffer" || mn == "NewObjectWithBufferLong") && ops.size() >= 5) {
                const auto keys = decodeSLP(c.file.obj_key_buffer(), ops[3].u32, ops[2].u32);
                const auto vals = decodeSLP(c.file.obj_value_buffer(), ops[4].u32, ops[2].u32);
                if (keys && vals && keys->size() == vals->size())
                    for (size_t k = 0; k < keys->size(); ++k) {
                        const auto& key = (*keys)[k];
                        if (!isSLPStringTag(key.tag) || key.stringIndex >= c.strings.size()) continue;
                        const auto& e = (*vals)[k];
                        std::optional<V> v;
                        switch (e.tag) {
                            case SLPTag::Null:    v = make(VKind::Null); break;
                            case SLPTag::True:    { V b = make(VKind::Boolean); b.number = 1; v = b; break; }
                            case SLPTag::False:   v = make(VKind::Boolean); break;
                            case SLPTag::Number:  v = makeNum(e.number); break;
                            case SLPTag::Integer: v = makeNum(e.integer); break;
                            default:
                                if (isSLPStringTag(e.tag) && e.stringIndex < c.strings.size())
                                    v = makeStr(VKind::String, c.strings[e.stringIndex]);
                        }
                        run.objectFields.push_back({ObjKey{c.function, instr.offset},
                                                    c.strings[key.stringIndex], v, std::nullopt});
                    }
            }

            if (any) run.flow.push_back(std::move(iv));
            apply(instr, st, c);
        }
    }
    std::sort(run.flow.begin(), run.flow.end(),
              [](const InstrValues& a, const InstrValues& b) { return a.offset < b.offset; });
    return run;
}

// Babel's _interopRequireDefault(e) { return e && e.__esModule ? e : {default: e}; }
// compiled: 1 parameter, reads `__esModule`, builds an object with own
// property `default`, small.
bool looksLikeInteropRequireDefault(const HBCFile& file, uint32_t f) {
    const auto& funcs = file.functions();
    if (f >= funcs.size() || funcs[f].paramCount != 2) return false;
    const auto& instrs = file.disasm(f);
    if (instrs.size() > 16) return false;
    const auto& strings = file.strings();
    bool esModule = false, defaultKey = false, newObject = false;
    for (const auto& i : instrs) {
        if (i.mnemonic == "NewObject") newObject = true;
        for (const auto& op : i.operands)
            if (op.type == Operand::Type::StringID && op.u32 < strings.size()) {
                if (strings[op.u32] == "__esModule" && isGetById(i.mnemonic)) esModule = true;
                if (strings[op.u32] == "default" && i.mnemonic.rfind("PutNewOwnById", 0) == 0)
                    defaultKey = true;
            }
    }
    return esModule && defaultKey && newObject;
}

// Agreement over a list of stored values: Babel's `exports.X = void 0`
// placeholders are ignored when a real store exists.
std::optional<V> agree(std::vector<std::optional<V>> vals) {
    if (vals.size() > 1)
        vals.erase(std::remove_if(vals.begin(), vals.end(),
                                  [](const auto& v) { return v && v->kind == VKind::Undefined; }),
                   vals.end());
    if (vals.empty() || !vals.front()) return std::nullopt;
    for (const auto& v : vals)
        if (!v || *v != *vals.front()) return std::nullopt;
    return vals.front();
}

ModuleExports buildExports(
    const HBCFile& self, const ValueGroup& group,
    const std::vector<std::optional<V>>& moduleStores,
    const std::vector<ExportEvent>& events,
    const std::vector<ObjectField>& objectFields)
{
    ModuleExports ex;
    ex.reassigned = !moduleStores.empty();
    ex.value = agree(moduleStores);

    // The value every Ret of `fn` returns.
    auto returnValue = [&](uint32_t fn) -> std::optional<V> {
        auto fl = group.flow.find(fn);
        std::map<uint32_t, const InstrValues*> at;
        if (fl != group.flow.end())
            for (const auto& iv : fl->second) at[iv.offset] = &iv;
        std::vector<std::optional<V>> rets;
        for (const auto& instr : self.disasm(fn))
            if (instr.mnemonic == "Ret") {
                auto it = at.find(instr.offset);
                rets.push_back(it != at.end() && !it->second->operands.empty()
                                   ? it->second->operands[0] : std::nullopt);
            }
        return agree(rets);
    };

    // object -> field -> stored values (a getter contributes its return value)
    std::map<ObjKey, std::map<std::string, std::vector<std::optional<V>>>> fields;
    std::map<ObjKey, std::map<std::string, bool>> fieldGetter;
    for (const auto& f : objectFields) {
        if (f.getter) {
            fields[f.object][f.name].push_back(returnValue(*f.getter));
            fieldGetter[f.object][f.name] = true;
        } else {
            fields[f.object][f.name].push_back(f.value);
        }
    }
    auto field = [&](const std::optional<V>& obj, const std::string& name) -> std::optional<std::optional<V>> {
        auto k = objectKey(obj);
        if (!k) return std::nullopt;
        auto o = fields.find(*k);
        if (o == fields.end()) return std::nullopt;
        auto f = o->second.find(name);
        if (f == o->second.end()) return std::nullopt;
        return agree(f->second);
    };
    // Names on the object that ends up exported: module.exports when it is
    // reassigned (its own properties, plus the fields of an object literal),
    // otherwise the original `exports` object.
    const int target = ex.reassigned ? 1 : 0;
    std::map<std::string, std::vector<std::optional<V>>> values;
    std::map<std::string, bool> getter;
    for (const auto& ev : events) {
        if (ev.target != target) continue;
        if (!ev.name) { ex.dynamicNames = true; continue; }
        if (*ev.name == "__esModule") { ex.esModule = true; continue; }
        if (ev.descriptor) {
            if (auto g = field(ev.descriptor, "get"); g && *g && (*g)->kind == VKind::Closure) {
                values[*ev.name].push_back(returnValue((*g)->index));
                getter[*ev.name] = true;
            } else if (auto v = field(ev.descriptor, "value")) {
                values[*ev.name].push_back(*v);
            } else {
                values[*ev.name].push_back(std::nullopt);
            }
        } else {
            values[*ev.name].push_back(ev.value);
        }
    }
    if (ex.reassigned && ex.value && ex.value->kind == VKind::Object)
        if (auto o = fields.find(*objectKey(ex.value)); o != fields.end())
            for (const auto& [name, vs] : o->second) {
                if (name == "__esModule") { ex.esModule = true; continue; }
                for (const auto& v : vs) values[name].push_back(v);
                if (fieldGetter[o->first][name]) getter[name] = true;
            }
    for (auto& [name, vs] : values)
        ex.names.push_back({name, agree(vs), getter[name]});
    return ex;
}

// A value taken from module M's exports, made independent of M: its
// `module.exports` / `exports` become Module(M); other factory-relative values
// (parameters, dependencyMap) have no meaning outside M and are dropped.
std::optional<V> rebase(const V& v, uint32_t m) {
    if (v.kind == VKind::FactoryParam) {
        if (v.index == 6) return make(VKind::Module, m);
        return std::nullopt;
    }
    if (v.kind == VKind::DepIndex) return std::nullopt;
    if (v.kind == VKind::Prop && v.base) {
        if (v.name == "exports" && v.base->kind == VKind::FactoryParam && v.base->index == 5)
            return make(VKind::Module, m);
        auto b = rebase(*v.base, m);
        if (!b) return std::nullopt;
        return makeProp(*b, v.name);
    }
    return v;
}

const ModuleExport* exportNamed(const ModuleExports& ex, const std::string& name) {
    for (const auto& e : ex.names)
        if (e.name == name) return &e;
    return nullptr;
}

AbstractValue resolveImpl(const V& v, const std::function<const ValueGroup*(int64_t)>& group,
                          int depth) {
    if (depth > 12) return v;
    auto exportsOf = [&](uint32_t m) -> const ModuleExports* {
        const ValueGroup* g = group(m);
        return g ? &g->exports : nullptr;
    };
    switch (v.kind) {
        case VKind::Module: {
            // module.exports = w: the module *is* w
            const ModuleExports* ex = exportsOf(v.index);
            if (ex && ex->reassigned && ex->value && ex->value->kind != VKind::Object)
                if (auto r = rebase(*ex->value, v.index)) return resolveImpl(*r, group, depth + 1);
            return v;
        }
        case VKind::Prop: {
            if (!v.base) return v;
            const V base = resolveImpl(*v.base, group, depth + 1);
            // property of a module namespace: its export
            if (base.kind == VKind::Module) {
                if (const ModuleExports* ex = exportsOf(base.index))
                    if (const ModuleExport* e = exportNamed(*ex, v.name); e && e->value)
                        if (auto r = rebase(*e->value, base.index)) return resolveImpl(*r, group, depth + 1);
                return makeProp(base, v.name);
            }
            // _interopRequireDefault(m): m itself for an ES module, {default: m} otherwise
            if (base.kind == VKind::ImportDefault) {
                const ModuleExports* ex = exportsOf(base.index);
                if (ex && ex->esModule)
                    return resolveImpl(makeProp(make(VKind::Module, base.index), v.name), group, depth + 1);
                if (ex && v.name == "default")
                    return resolveImpl(make(VKind::Module, base.index), group, depth + 1);
                return makeProp(base, v.name);
            }
            return makeProp(base, v.name);
        }
        default:
            return v;
    }
}

} // namespace

AbstractValue resolveThroughExports(
    const AbstractValue& v, const std::function<const ValueGroup*(int64_t)>& group) {
    return resolveImpl(v, group, 0);
}

std::vector<int64_t> computeFunctionGroups(const HBCFile& self) {
    const size_t n = self.functions().size();
    std::vector<int64_t> group(n, -1);
    std::vector<std::vector<uint32_t>> children(n);
    for (const auto& cs : self.call_graph())
        if (!cs.isDirect && cs.calleeIndex < n && cs.callerIndex < n)
            children[cs.callerIndex].push_back(cs.calleeIndex);
    for (const auto& m : self.recover_modules()) {
        if (m.functionIndex >= n) continue;
        std::vector<uint32_t> stack{m.functionIndex};
        while (!stack.empty()) {
            const uint32_t f = stack.back();
            stack.pop_back();
            if (group[f] != -1) continue;
            group[f] = m.moduleId;
            for (uint32_t c : children[f]) stack.push_back(c);
        }
    }
    return group;
}

ValueGroup computeValueGroup(
    const HBCFile& self, int64_t groupId, const std::vector<int64_t>& groupOf,
    const std::function<const ValueGroup*(int64_t)>& groupResult)
{
    ValueGroup out;
    const MetroModule* module = nullptr;
    std::vector<MetroModule> mods;
    if (groupId >= 0) {
        mods = self.recover_modules();
        for (const auto& m : mods)
            if (m.moduleId == static_cast<uint32_t>(groupId)) module = &m;
    }

    std::vector<uint32_t> funcs;
    for (uint32_t f = 0; f < groupOf.size(); ++f)
        if (groupOf[f] == groupId) funcs.push_back(f);

    std::unordered_map<uint32_t, std::unordered_map<uint32_t, EnvAccess>> envAt;
    for (uint32_t f : funcs)
        for (const auto& a : self.env_accesses(f)) envAt[f][a.offset] = a;

    std::map<uint32_t, bool> interopMemo;
    const std::function<bool(uint32_t)> isInteropHelper = [&](uint32_t moduleId) {
        auto it = interopMemo.find(moduleId);
        if (it != interopMemo.end()) return it->second;
        interopMemo[moduleId] = false;  // cycle guard
        bool yes = false;
        if (static_cast<int64_t>(moduleId) != groupId)
            if (const ValueGroup* g = groupResult(moduleId))
                yes = g->moduleExports && g->moduleExports->kind == VKind::Closure &&
                      looksLikeInteropRequireDefault(self, g->moduleExports->index);
        return interopMemo[moduleId] = yes;
    };

    const auto& strings = self.strings();

    // One pass over the group with the given slot table: the flow of every
    // function, plus the slot values the stores agree on.
    std::vector<ExportEvent> exportEvents;
    std::vector<ObjectField> objectFields;
    std::vector<std::optional<V>> exportStores;
    auto pass = [&](const std::map<SlotKey, V>& slots, const std::map<ParamKey, V>& params,
                    std::map<SlotKey, V>& agreedOut, std::map<ParamKey, V>& paramsOut) {
        std::map<SlotKey, std::optional<V>> agreed;  // nullopt = conflicting/unknown
        std::map<uint32_t, std::vector<std::optional<std::vector<std::optional<V>>>>> callsOf;
        std::set<uint32_t> escaped;
        exportStores.clear();
        exportEvents.clear();
        objectFields.clear();
        out.flow.clear();
        for (uint32_t f : funcs) {
            static const std::unordered_map<uint32_t, EnvAccess> none;
            auto e = envAt.find(f);
            const Ctx c{self, f, strings, module, module && module->functionIndex == f, slots,
                        params, e == envAt.end() ? none : e->second, isInteropHelper};
            FunctionRun run = runFunction(f, c);
            for (auto& [key, v] : run.stores) {
                auto it = agreed.find(key);
                if (it == agreed.end()) agreed.emplace(key, v);
                else if (it->second && (!v || *it->second != *v)) it->second.reset();
            }
            for (auto& v : run.moduleExportStores) exportStores.push_back(v);
            for (auto& e : run.exportEvents) exportEvents.push_back(std::move(e));
            for (auto& o : run.objectFields) objectFields.push_back(std::move(o));
            for (auto& [g, args] : run.calls) callsOf[g].push_back(std::move(args));
            escaped.insert(run.escapes.begin(), run.escapes.end());
            if (!run.flow.empty()) out.flow[f] = std::move(run.flow);
        }
        agreedOut.clear();
        for (auto& [key, v] : agreed)
            if (v) agreedOut.emplace(key, *v);
        // Parameters of non-escaping closures: the value every call passes.
        paramsOut.clear();
        for (auto& [g, sites] : callsOf) {
            if (escaped.count(g) || groupOf.size() <= g || groupOf[g] != groupId) continue;
            size_t width = 0;
            bool allKnown = true;
            for (const auto& a : sites) {
                if (!a) { allKnown = false; break; }
                width = std::max(width, a->size());
            }
            if (!allKnown) continue;
            for (uint32_t i = 0; i < width; ++i) {
                std::optional<V> agreedV;
                bool ok = true;
                for (const auto& a : sites) {
                    const auto& v = i < a->size() ? (*a)[i] : std::optional<V>(make(VKind::Undefined));
                    if (!v || (agreedV && *agreedV != *v)) { ok = false; break; }
                    agreedV = v;
                }
                if (ok && agreedV) paramsOut.emplace(ParamKey{g, i}, *agreedV);
            }
        }
        out.moduleExports.reset();
        if (!exportStores.empty() && exportStores.front() &&
            std::all_of(exportStores.begin(), exportStores.end(),
                        [&](const auto& v) { return v && *v == *exportStores.front(); }))
            out.moduleExports = exportStores.front();
    };

    // Iterate to a fixpoint: the flow was computed with exactly the slot and
    // parameter tables its stores and calls agree on. Without one within
    // kMaxRounds, a last pass with neither keeps only facts that do not depend
    // on them.
    constexpr int kMaxRounds = 4;
    std::map<SlotKey, V> slots, next;
    std::map<ParamKey, V> params, nextParams;
    bool stable = false;
    for (int round = 0; round < kMaxRounds && !stable; ++round) {
        pass(slots, params, next, nextParams);
        stable = next == slots && nextParams == params;
        slots = next;
        params = nextParams;
    }
    if (!stable) {
        slots.clear();
        params.clear();
        pass(slots, params, next, nextParams);
    }
    out.slots = std::move(slots);
    if (module) out.exports = buildExports(self, out, exportStores, exportEvents, objectFields);
    return out;
}

} // namespace hbc
