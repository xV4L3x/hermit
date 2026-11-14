"""Rendering of Metro module views for the REPL (recover_modules()).

Extracted from display.py. Shares the generic helpers `_func_name` and
`closure_children`, which stay in display.py (the latter is also used by the
function listing).
"""

from typing import Optional

from cli.display import _func_name, closure_children


def _dep_text(dep) -> str:
    """A dependencyMap entry: a module id, or None for a `null` entry (an
    optional dependency Metro could not resolve)."""
    return "null" if dep is None else str(dep)


def module_functions(call_graph: list, mod, children: Optional[dict] = None) -> list[int]:
    """Indices of the functions of a Metro module, sorted: its factory and every
    function lexically nested in it (reachable through closure_children()). Each
    function is created by exactly one parent, so the modules of a bundle are
    disjoint (verified on ridemovi.hbc). `children`: closure_children(call_graph),
    to reuse it across modules."""
    if children is None:
        children = closure_children(call_graph)
    seen = {mod.function_index}
    stack = [mod.function_index]
    while stack:
        for c in children.get(stack.pop(), ()):
            if c not in seen:
                seen.add(c)
                stack.append(c)
    return sorted(seen)


def function_modules(call_graph: list, mods: list) -> dict:
    """functionIndex -> (module id, is the module's factory) for every function
    that belongs to a Metro module (module_functions()). Functions outside every
    module (the global function, the Metro prelude) are absent."""
    children = closure_children(call_graph)
    out: dict[int, tuple] = {}
    for m in mods:
        for i in module_functions(call_graph, m, children):
            out[i] = (m.module_id, i == m.function_index)
    return out


def module_function_counts(call_graph: list, mods: list) -> dict:
    """module id -> number of functions the module implements (module_functions())."""
    children = closure_children(call_graph)
    return {m.module_id: len(module_functions(call_graph, m, children)) for m in mods}


def _module_ref(mod_id: int, by_id: dict, funcs: list) -> str:
    m = by_id.get(mod_id)
    if m is None:
        return f"\x1b[1m#{mod_id}\x1b[0m  \x1b[31mnot defined in this bundle\x1b[0m"
    return (f"\x1b[1m#{mod_id}\x1b[0m  "
            f"\x1b[2m[{m.function_index}]\x1b[0m {_func_name(funcs, m.function_index)}")


def modules_to_ansi_rows(mods: list, funcs: list, entries: list,
                         func_counts: Optional[dict] = None,
                         max_deps: int = 16) -> list[str]:
    """One row per Metro module, sorted by id: id, factory function, number of
    functions the module implements (`func_counts`, module_function_counts()),
    entry mark, then the dependencyMap (last, so the viewer's truncation only
    cuts long dependency lists)."""
    entry_set = set(entries)
    rows = []
    for m in sorted(mods, key=lambda m: m.module_id):
        entry = "\x1b[1;32mentry\x1b[0m" if m.module_id in entry_set else "     "
        n = (func_counts or {}).get(m.module_id)
        nfuncs = ("" if n is None else
                  f"\x1b[33m{n:5d} {'function ' if n == 1 else 'functions'}\x1b[0m ")
        if not m.dependencies_known:
            deps = "\x1b[31mdeps ?\x1b[0m"
        else:
            shown = ", ".join(_dep_text(d) for d in m.dependencies[:max_deps])
            if len(m.dependencies) > max_deps:
                shown += f", … +{len(m.dependencies) - max_deps}"
            deps = f"\x1b[36mdeps {len(m.dependencies):3d}\x1b[0m [{shown}]"
        rows.append(
            f"\x1b[1m#{m.module_id:<5d}\x1b[0m "
            f"\x1b[2m[{m.function_index:5d}]\x1b[0m "
            f"{_func_name(funcs, m.function_index):<24} "
            f"{nfuncs}{entry}  {deps}"
        )
    return rows


def module_callees_rows(mod, mods: list, funcs: list) -> list[str]:
    """What `mod` includes: one row per dependencyMap entry, resolved to the
    module's factory (`null` = optional dependency Metro left unresolved)."""
    if not mod.dependencies_known:
        return ["\x1b[31mthe dependencyMap is not an array literal: dependencies unknown\x1b[0m"]
    by_id = {m.module_id: m for m in mods}
    rows = []
    for i, d in enumerate(mod.dependencies):
        ref = ("\x1b[2mnull (optional dependency, unresolved)\x1b[0m" if d is None
               else _module_ref(d, by_id, funcs))
        rows.append(f"\x1b[2mdependencyMap[{i}]\x1b[0m  {ref}")
    return rows


def module_callers(mod, mods: list) -> list:
    """(module, [dependencyMap indices]) for every module that includes `mod`,
    sorted by module id."""
    out = []
    for m in sorted(mods, key=lambda m: m.module_id):
        idx = [i for i, d in enumerate(m.dependencies) if d == mod.module_id]
        if idx:
            out.append((m, idx))
    return out


def module_callers_rows(mod, mods: list, funcs: list) -> list[str]:
    """Who includes `mod`: one row per module whose dependencyMap lists it, with
    the position(s) in that map (`_$$_REQUIRE(_dependencyMap[i])` in the caller)."""
    by_id = {m.module_id: m for m in mods}
    return [f"{_module_ref(m.module_id, by_id, funcs)}  "
            f"\x1b[2mas dependencyMap[{', '.join(map(str, idx))}]\x1b[0m"
            for m, idx in module_callers(mod, mods)]


def module_function_separator(func, mod) -> str:
    """Header line of one function in `disasm module:<id>`."""
    tag = f"# module {mod.module_id}" + (" entry" if func.index == mod.function_index else "")
    return (f"\x1b[1;36m── func:{func.index} ({func.name})\x1b[0m  "
            f"\x1b[36m{tag}\x1b[0m \x1b[2m{'─' * 20}\x1b[0m")


def module_exports_rows(exports) -> list[str]:
    """The exports of a module (HBCFile.module_exports()): one row per name with
    its value when every store agrees, `?` otherwise; `get` marks a getter."""
    flags = []
    if exports.es_module:
        flags.append("ES module")
    if exports.reassigned:
        flags.append("module.exports = "
                     + (str(exports.value) if exports.value is not None else "?"))
    if exports.dynamic_names:
        flags.append("+ names computed at runtime (export *)")
    rows = [f"\x1b[36mexports ({len(exports.names)})\x1b[0m"
            + (f"  \x1b[2m{', '.join(flags)}\x1b[0m" if flags else "")]
    width = max((len(e.name) for e in exports.names), default=0)
    for e in exports.names:
        value = str(e.value) if e.value is not None else "\x1b[2m?\x1b[0m"
        getter = "  \x1b[2m(get)\x1b[0m" if e.via_getter else ""
        rows.append(f"  \x1b[1m{e.name:<{width}}\x1b[0m  = {value}{getter}")
    return rows


def module_detail_rows(mod, mods: list, funcs: list, entries: list,
                       n_functions: Optional[int] = None, exports=None) -> list[str]:
    """`modules module:<id>`: factory, number of functions, where it is defined,
    its exports (module_exports_rows()) and its dependencyMap
    (module_callees_rows()). Who includes it: `xrefs module:<id>`."""
    rows = [
        f"\x1b[1mmodule #{mod.module_id}\x1b[0m"
        + ("  \x1b[1;32mentry\x1b[0m (__r)" if mod.module_id in entries else ""),
        f"  factory    \x1b[2m[{mod.function_index}]\x1b[0m {_func_name(funcs, mod.function_index)}",
    ]
    if n_functions is not None:
        rows.append(f"  functions  {n_functions}  \x1b[2m(functions --module={mod.module_id})\x1b[0m")
    rows += [f"  __d at     \x1b[33m{mod.define_offset:#06x}\x1b[0m in the global function", ""]
    if exports is not None:
        rows += module_exports_rows(exports) + [""]
    rows += [f"\x1b[36mdependencies ({len(mod.dependencies)})\x1b[0m"]
    rows += ["  " + r for r in module_callees_rows(mod, mods, funcs)]
    rows += ["", f"\x1b[2mwho includes it: xrefs module:{mod.module_id}\x1b[0m"]
    return rows
