"""Views of the closure environment model and the value analysis: disasm
annotations, `env func:<N>`, `xrefs func:<N>` through resolved calls.

Ground truth:
  hermitvault.hbc      module 0 (index.js), Metro JS:
                         var _App = _interopRequireDefault(_$$_REQUIRE(_dependencyMap[2]));
                         _reactNative.AppRegistry.registerComponent(_app.name, () => _App.default);
                       func 83 = factory, func 84 = the arrow function, func 86 =
                       _interopRequireDefault (module 1); module 513 is an ES module.
  closure_scope_g.hbc  makeCounter / increment (`count`), debug names.
  env_chain_g.hbc      outer > middle > inner (`x`, `y`), debug names.
"""

import pytest

import _libhbc
import cli.repl as repl_mod
from cli import display
from cli.repl import HermitREPL
from testutil import corpus_path, strip_ansi


def _open(name):
    return _libhbc.HBCFile.open(str(corpus_path(name)), _libhbc.ValidationMode.Strict)


def _named(f, name):
    return next(fi for fi in f.functions() if fi.name == name)


def _plain(rows):
    return [strip_ansi(r) for r in rows]


@pytest.fixture
def shown(monkeypatch):
    calls = []
    monkeypatch.setattr(repl_mod, "show_list",
                        lambda rows, title="", **kw: calls.append((title, rows)))
    return calls


def _repl(name):
    r = HermitREPL()
    r._open_file(str(corpus_path(name)))
    return r


# --- disasm annotations -----------------------------------------------------


def test_disasm_annotations_metro_factory():
    f = _open("hermitvault.hbc")
    notes = display.disasm_annotations(f, 83, f.functions())
    assert notes[0x10] == "require -> module_1"
    assert notes[0x1c] == "require -> module_2"
    assert notes[0x28] == "require -> module_513"
    # the callee is module 1's export (`module.exports = _interopRequireDefault`)
    assert notes[0x2d] == "module_1(...) = func:86 (_interopRequireDefault)"
    assert notes[0x32] == "func:83 (anonymous_func_83) slot 0 = module_513"
    assert notes[0x49] == "require -> module_533"
    # the outer slot read by the arrow function
    assert display.disasm_annotations(f, 84, f.functions()) == {
        0x3: "func:83 (anonymous_func_83) slot 0 = module_513"}


def test_disasm_annotations_debug_names():
    f = _open("env_chain_g.hbc")
    inner, outer, middle = (_named(f, n).index for n in ("inner", "outer", "middle"))
    notes = list(display.disasm_annotations(f, inner, f.functions()).values())
    assert notes == [f"func:{outer} (outer) slot 0 x", f"func:{middle} (middle) slot 0 y",
                     f"func:{outer} (outer) slot 0 x"]


def test_disasm_annotations_calls():
    f = _open("hermitvault.hbc")
    funcs = f.functions()
    # module 12 is CommonJS: importDefaultOf(module_12).default is module_12 itself
    assert display.disasm_annotations(f, 219, funcs)[0x43] == "module_12(...) = func:221 (invariant)"
    assert display.disasm_annotations(f, 219, funcs)[0xc] == "calls func:217 (requireModule)"
    # the export name (jsx) differs from the function's own name (jsxProd)
    assert (display.disasm_annotations(f, 190, funcs)[0x164]
            == "module_242.jsx(...) = func:2629 (jsxProd)")


def test_disasm_rows_show_annotations():
    f = _open("hermitvault.hbc")
    func = f.functions()[84]
    notes = display.disasm_annotations(f, 84, f.functions())
    rows = _plain(display.disasm_to_ansi_rows(func, f.disasm(84), [], None, notes))
    load = next(r for r in rows if "LoadFromEnvironment" in r)
    assert load.endswith("; func:83 (anonymous_func_83) slot 0 = module_513")
    assert not any(";" in r for r in rows if "GetEnvironment" in r)


def test_value_text_es_module_default_import():
    f = _open("hermitvault.hbc")
    v = f.env_slot_value(f.env_accesses(84)[0].env, 0)
    assert v.kind == _libhbc.AbstractValue.Kind.ImportDefault
    assert display.value_text(f, v, f.functions()) == "module_513"


# --- env func:<N> -----------------------------------------------------------


def test_env_view_counter():
    f = _open("closure_scope_g.hbc")
    mk, inc = _named(f, "makeCounter"), _named(f, "increment")
    rows = _plain(display.env_view_rows(f, mk, f.functions(), f.call_graph()))
    assert rows[0] == f"func:{mk.index} (makeCounter)"
    assert "  environment  created at 0x0000, parent: func:0 (global)" in rows
    assert "slots (1)" in rows
    i = rows.index("  slot 0   count  = ?")  # stored twice with different values
    uses = rows[i + 1:i + 4]
    assert uses[0].split()[:3] == ["store", f"func:{mk.index}", "(makeCounter)"]
    assert uses[1].split()[:3] == ["load", f"func:{inc.index}", "(increment)"]
    assert uses[2].split()[:3] == ["store", f"func:{inc.index}", "(increment)"]


def test_env_view_captured_chain_and_accesses():
    f = _open("env_chain_g.hbc")
    inner, middle, outer = (_named(f, n) for n in ("inner", "middle", "outer"))
    rows = _plain(display.env_view_rows(f, inner, f.functions(), f.call_graph()))
    assert "  environment  none" in rows
    assert (f"  captured     func:{middle.index} (middle) -> func:{outer.index} (outer)"
            " -> func:0 (global)" in rows)
    acc = rows[rows.index("accesses (3)") + 1:]
    assert [r.split("  ")[-1].strip() for r in acc] == [
        f"func:{outer.index} (outer) slot 0 x", f"func:{middle.index} (middle) slot 0 y",
        f"func:{outer.index} (outer) slot 0 x"]
    assert [r.split()[2] for r in acc] == ["load", "load", "store"]


def test_env_view_metro_factory():
    f = _open("hermitvault.hbc")
    rows = _plain(display.env_view_rows(f, f.functions()[83], f.functions(), f.call_graph(),
                                        (0, True)))
    assert rows[0].endswith("# module 0 entry")
    i = rows.index("  slot 0   -  = module_513")
    assert rows[i + 1].split()[:2] == ["store", "func:83"]
    assert rows[i + 2].split()[:2] == ["load", "func:84"]


def test_env_command(shown):
    r = _repl("hermitvault.hbc")
    r._dispatch("env func:84")
    title, rows = shown[-1]
    assert title == "env  func:84 (anonymous_func_84)"
    plain = _plain(rows)
    assert plain[0].endswith("# module 0")
    assert "  captured     func:83 (anonymous_func_83) -> func:0 (global)" in plain


def test_env_command_bad_usage(shown, capsys):
    r = _repl("hermitvault.hbc")
    r._dispatch("env")
    r._dispatch("env module:0")
    assert capsys.readouterr().out.count("Usage: env func:<N|name>") == 2
    assert not shown


# --- xrefs func:<N> through resolved calls ----------------------------------


def test_function_callers_include_export_calls():
    f = _open("hermitvault.hbc")
    rows = _plain(display.function_callers_rows(f, f.functions()[86], f.functions(),
                                                f.call_graph()))
    # one row per caller: its calls, and separately the closure creation
    assert "func:83 (anonymous_func_83)  1 call  @ 0x002d" in rows
    assert "func:85 (anonymous_func_85)  creates the closure  @ 0x0005" in rows
    assert "func:187 (anonymous_func_187)  3 calls  @ 0x0053, 0x0068, 0x007d" in rows
    callers = [r.split(")")[0] for r in rows]
    assert len(callers) == len(set(callers))
    assert len(rows) > 100   # every module importing a default export


def test_function_callees_complete_unresolved_sites():
    f = _open("hermitvault.hbc")
    rows = _plain(display.function_callees_rows(f, f.functions()[83], f.functions(),
                                                f.call_graph()))
    assert rows == [
        "require -> module_1  @ 0x0010  (Call2)",
        "require -> module_2  @ 0x001c  (Call2)",
        "require -> module_513  @ 0x0028  (Call2)",
        "func:86 (_interopRequireDefault)  @ 0x002d  (Call2, as module_1(...))",
        "require -> module_533  @ 0x0049  (Call2)",
        "func:84 (anonymous_func_84)  @ 0x0053  (CreateClosure)",
        "[unresolved]  @ 0x0058  (Call3)",   # registerComponent: module_235 is a wildcard import (B41)
    ]


def test_xrefs_func_command_uses_resolved_calls(shown):
    r = _repl("hermitvault.hbc")
    r._dispatch("xrefs func:86")
    title, rows = shown[-1]
    assert title == "xrefs  func:86 (_interopRequireDefault)  callers"
    assert "func:83 (anonymous_func_83)  1 call  @ 0x002d" in _plain(rows)
