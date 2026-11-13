"""recover_modules() / metro_entry_modules() against Metro's own module table.

hermitvault_truth.json is extracted by gen_truth.py from the Metro JS bundle
(hermit-vault/build/main.js) that hermesc compiled into seeder/corpus/hermitvault.hbc:
every module id, its dependencyMap and the __r entry points must come back
exactly from the bytecode.
"""

import json
from pathlib import Path

import _libhbc

from testutil import corpus_path

_TRUTH = json.loads((Path(__file__).parent / "hermitvault_truth.json").read_text())


def test_hermitvault_modules_match_metro_js():
    f = _libhbc.HBCFile.open(str(corpus_path("hermitvault.hbc")), _libhbc.ValidationMode.Strict)
    mods = f.recover_modules()

    assert all(m.dependencies_known for m in mods)
    recovered = {str(m.module_id): list(m.dependencies) for m in mods}
    assert len(recovered) == len(mods)          # unique ids
    assert recovered == _TRUTH["modules"]
    assert f.metro_entry_modules() == _TRUTH["entries"]


def test_factories_are_distinct_functions():
    f = _libhbc.HBCFile.open(str(corpus_path("hermitvault.hbc")), _libhbc.ValidationMode.Strict)
    mods = f.recover_modules()
    gidx = f.header().global_code_index
    factories = [m.function_index for m in mods]
    assert len(set(factories)) == len(factories)
    assert gidx not in factories


def test_hermitvault_requires_resolved_like_metro_js():
    """Value analysis (value_flow): in every module, the dependencyMap indices
    passed to `require` in the bytecode are exactly those of the Metro JS, and
    every require call resolves to Module(dependencyMap[k])."""
    f = _libhbc.HBCFile.open(str(corpus_path("hermitvault.hbc")), _libhbc.ValidationMode.Strict)
    K = _libhbc.AbstractValue.Kind
    deps = {m.module_id: list(m.dependencies) for m in f.recover_modules()}
    required: dict[int, set] = {mid: set() for mid in deps}
    unresolved = 0
    for fn in f.functions():
        mid = f.function_module(fn.index)
        if mid is None:
            continue
        instrs = {i.offset: i for i in f.disasm(fn.index)}
        for iv in f.value_flow(fn.index):
            if not instrs[iv.offset].mnemonic.startswith("Call") or len(iv.operands) < 4:
                continue
            callee, arg = iv.operands[1], iv.operands[3]
            if callee is None or callee.kind != K.FactoryParam or callee.index != 2:
                continue
            if iv.result is None or iv.result.kind != K.Module or arg is None:
                unresolved += 1
                continue
            assert arg.kind == K.DepIndex
            assert iv.result.index == deps[mid][arg.index]
            required[mid].add(arg.index)
    assert unresolved == 0
    assert {str(k): sorted(v) for k, v in required.items()} == _TRUTH["requires"]


# Modules whose JS sets names on an inner function parameter called `exports`
# that receives the module's exports only on some paths, so the bytecode cannot
# prove they are exported (gen_truth.py's regexes do not see scopes):
#   185 regenerator-runtime: function (exports) {...}(typeof module === "object" ? module.exports : {})
#   203 whatwg-fetch UMD: factory(exports) | define(['exports'], factory) | factory(global.WHATWGFetch = {})
_EXPORTS_NOT_PROVABLE = {"185", "203"}


def test_hermitvault_exports_match_metro_js():
    """module_exports(): names, __esModule and computed (export *) names equal
    the Metro JS for every module, except the ones above."""
    f = _libhbc.HBCFile.open(str(corpus_path("hermitvault.hbc")), _libhbc.ValidationMode.Strict)
    for mid, want in _TRUTH["exports"].items():
        e = f.module_exports(int(mid))
        if mid in _EXPORTS_NOT_PROVABLE:
            assert {n.name for n in e.names} <= set(want)
            continue
        assert sorted(n.name for n in e.names) == want, mid
        assert e.es_module == (int(mid) in _TRUTH["esModule"]), mid
        assert e.dynamic_names == (int(mid) in _TRUTH["dynamicExports"]), mid
