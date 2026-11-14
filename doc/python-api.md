# Python API (`_libhbc`)

The C++ core is exposed to Python as the native module `_libhbc` (built by
pybind11). It is not installed into the venv; it lives next to the build, so put
it on `PYTHONPATH` first:

```bash
export PYTHONPATH="$PWD/build/bindings:$PYTHONPATH"
python -c "import _libhbc; print(_libhbc.HBCFile)"
```

A type stub (`_libhbc.pyi`) is shipped next to the module, so editors show the
signatures and return types below. It is generated from the compiled module with
`bindings/genstub.py` (regenerate it after changing the bindings).

Everything here is read-only analysis. Structural patching goes through
`_libhbc.Module` and the `HBCFile.add_*/set_*` family, documented separately.

## Opening a file

```python
import _libhbc
f = _libhbc.HBCFile.open("app.hbc")                 # from a path
f = _libhbc.HBCFile.from_bytes(open("app.hbc","rb").read())   # from memory
```

`open(path, mode=ValidationMode.Strict)` / `from_bytes(data, mode=...)`. Use
`ValidationMode.Permissive` to parse past minor inconsistencies. Parsing is lazy
and memoized: opening is cheap, each analysis is computed on first use and cached
on the object.

## Method reference

Function and module identifiers are indices: `func_N` is `functions()[N]`,
`module_M` is the Metro module with `module_id == M`.

### File structure

| Method | Returns | Notes |
|---|---|---|
| `header()` | `HBCFileHeader` | counts, sizes, source hash |
| `version()` | `int` | HBC bytecode version (e.g. 96) |
| `functions()` | `list[FunctionInfo]` | one per function, index order |
| `strings()` | `list[str]` | the string table, decoded |
| `string_kinds()` | `list[StringKind]` | parallel to `strings()` |
| `string_entries()` | `list[StringEntry]` | raw per-string metadata |
| `disasm(func_index)` | `list[Instruction]` | decoded instructions of one function |
| `cfg(func_index)` | `CFGData` | basic blocks + edges |
| `decompile(func_index)` | `str` | deterministic JavaScript |

### Metro modules

| Method | Returns | Notes |
|---|---|---|
| `recover_modules()` | `list[MetroModule]` | reconstructed `__d(...)` modules + dependency map |
| `metro_entry_modules()` | `list[int]` | module ids run at startup |
| `function_module(func_index)` | `int \| None` | the module a function belongs to (`None` if outside any) |
| `module_exports(module_id)` | `ModuleExports` | the module's exported names and values |

### Value analysis

| Method | Returns | Notes |
|---|---|---|
| `value_flow(func_index)` | `list[InstrValues]` | abstract value of each register per instruction |
| `resolve_value(value)` | `AbstractValue` | follow a value through module exports/closures |
| `resolved_calls(func_index)` | `list[ResolvedCall]` | the calls in a function whose callee resolves to a function |
| `function_callers(func_index)` | `list[CallerSite]` | every call that resolves to this function, file-wide |
| `build_caller_index(progress=None)` | `None` | build the reverse index up front (see below) |
| `env_accesses(func_index)` | `list[EnvAccess]` | closure env loads/stores |
| `environments(func_index)` | `list[EnvironmentInfo]` | environments the function creates |
| `captured_environment(func_index)` | `EnvKey \| None` | the env captured from the parent |
| `env_slot_uses(env, slot)` | `list[EnvSlotUse]` | who reads/writes a closure slot |
| `env_slot_value(env, slot)` | `AbstractValue \| None` | the value a slot holds, if constant |

### Cross-references

| Method | Returns | Notes |
|---|---|---|
| `call_graph()` | `list[CallSite]` | direct (static) call edges |
| `string_xrefs()` | `list[StringXRef]` | each referenced string and its use sites (operands + literal elements) |

## Return shapes

The objects above are plain field holders (each has a `__repr__` showing its key
fields). The important ones:

**`FunctionInfo`** — `index`, `name`, `offset`, `bytecodeSizeInBytes`,
`paramCount`, `frameSize`, `isStrictMode`, `isAsync`, `isGenerator`,
`hasExceptionHandler`, `overflowed`, `shared`, `shareCount`, `exceptionHandlers`.

**`MetroModule`** — `module_id`, `function_index` (the factory), `dependencies`
(`list[int | None]`, the dependency map; `None` where unresolved),
`dependencies_known`, `define_offset`.

**`ModuleExports`** — `names` (`list[ModuleExport]`), `reassigned`, `value`
(`AbstractValue | None`, for `module.exports = v`), `es_module`, `dynamic_names`.

**`ModuleExport`** — `name`, `value` (`AbstractValue | None`), `via_getter`,
and `function_index` (`int | None`): the exported function's index when the
export is a closure. Use it instead of parsing `repr(value)`.

**`AbstractValue`** — `kind` (`AbstractValue.Kind`), `index`, `number`, `name`,
`base`, and `func_index` (`int | None`): the function index when
`kind == Kind.Closure`, else `None`. `str(v)` renders it (`module_2.AppRegistry`,
`func_84`, `require`, `dependencyMap[3]`, ...).

**`ResolvedCall`** — `offset`, `callee` (function index), `via`
(`ResolvedCall.Via.Closure` / `.Export`), `is_construct`, `callee_value`.

**`CallerSite`** — `caller` (function index), `offset`, `via`, `is_construct`.

**`CallSite`** — `caller_index`, `callee_index` (`0xFFFFFFFF` when unresolved,
see `is_resolved`), `offset`, `is_direct`, `mnemonic`.

**`InstrValues`** — `offset`, `operands` (`list[AbstractValue | None]`, parallel
to the instruction operands), `result` (`AbstractValue | None`).

**`StringXRef`** — `string_index`, `string_value`, `sites` (`list[XRefSite]`).
**`XRefSite`** — `function_index`, `instruction_offset`, `mnemonic`, `kind`
(`XRefKind`), `element_index`, `element_count`.

## `function_callers` and `build_caller_index`

`function_callers(f)` is served from a reverse index (callee -> callers) built
once over the whole file. That build is the heavy step: it resolves calls across
every function, which on a large bundle (e.g. ~150k functions) takes tens of
seconds. It runs on the first `function_callers` call and is cached; every call
after it is an O(1) lookup.

To pay that cost up front and show progress, call `build_caller_index` first:

```python
f.build_caller_index(lambda done, total: print(f"\r{done}/{total}", end=""))
callers = f.function_callers(84123)        # instant now
```

`progress(done, total)` is optional and called periodically; the call is a no-op
once the index exists.

## Recipes

Export names of every module (the fast way, no index scan):

```python
for m in f.recover_modules():
    for e in f.module_exports(m.module_id).names:
        if e.function_index is not None:
            print(f"module_{m.module_id}.{e.name} -> func_{e.function_index}")
```

Resolve the calls a function makes:

```python
for rc in f.resolved_calls(84123):
    kind = "new " if rc.is_construct else ""
    print(f"@{rc.offset:#x}: {kind}func_{rc.callee} (via {rc.via})")
```

Trace callers of a function (build the index once, then query freely):

```python
f.build_caller_index()
for c in f.function_callers(34569):
    print(f"func_{c.caller} @ {c.offset:#x}")
```
