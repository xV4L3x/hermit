"""`modules` and `cfg module:<id>` (Metro modules): row builders, DOT
generation and REPL dispatch, with the viewer and the pager stubbed."""

import re

import pytest

import _libhbc
import cli.repl as repl_mod
from cli import cfg_dot, display, modules_view
from cli.repl import HermitREPL
from testutil import corpus_path, strip_ansi


def _open(name):
    return _libhbc.HBCFile.open(str(corpus_path(name)), _libhbc.ValidationMode.Strict)


def _module(mods, mod_id):
    return next(m for m in mods if m.module_id == mod_id)


@pytest.fixture
def shown(monkeypatch):
    """Records what the REPL would show: (title, rows) per show_list call."""
    calls = []
    monkeypatch.setattr(repl_mod, "show_list",
                        lambda rows, title="", **kw: calls.append((title, rows)))
    return calls


@pytest.fixture
def graphs(monkeypatch):
    calls = []
    monkeypatch.setattr(cfg_dot, "show_module_graph",
                        lambda mod, mods, funcs, entries, **kw:
                        calls.append((mod.module_id, kw)))
    return calls


def _repl(name):
    r = HermitREPL()
    r._open_file(str(corpus_path(name)))
    return r


# --- row builders -----------------------------------------------------------


def test_module_rows_hermitvault():
    f = _open("hermitvault.hbc")
    mods, funcs = f.recover_modules(), f.functions()
    counts = modules_view.module_function_counts(f.call_graph(), mods)
    rows = [strip_ansi(r) for r in modules_view.modules_to_ansi_rows(mods, funcs, [106, 0], counts)]
    assert len(rows) == 534
    assert rows[0].startswith("#0 ")
    assert "entry" in rows[0] and "deps   4 [1, 2, 513, 533]" in rows[0]
    assert "entry" not in rows[1] and "deps   0 []" in rows[1]
    assert "used by" not in rows[0]
    assert f"{counts[0]:5d} functions" in rows[0]
    # module 2 has 98 dependencies: the list is cut after 16
    assert "… +82" in rows[2]
    # #106 (InitializeCore) is only its factory
    assert "    1 function  entry" in rows[106]


def test_module_function_counts_cover_every_module_function():
    f = _open("hermitvault.hbc")
    cg, mods = f.call_graph(), f.recover_modules()
    counts = modules_view.module_function_counts(cg, mods)
    assert set(counts) == {m.module_id for m in mods}
    for m in mods[:50]:
        assert counts[m.module_id] == len(modules_view.module_functions(cg, m))


def test_callers_are_the_reverse_of_callees():
    f = _open("hermitvault.hbc")
    mods = f.recover_modules()
    by_id = {m.module_id: m for m in mods}
    for m in mods:
        for caller, idx in modules_view.module_callers(m, mods):
            for i in idx:
                assert caller.dependencies[i] == m.module_id
    edges = {(m.module_id, d) for m in mods for d in m.dependencies if d is not None}
    reverse = {(c.module_id, m.module_id) for m in mods
               for c, _ in modules_view.module_callers(m, mods)}
    assert reverse == edges
    assert all(d in by_id for _, d in edges)


def test_module_callers_and_callees_rows():
    f = _open("hermitvault.hbc")
    mods, funcs = f.recover_modules(), f.functions()
    callees = [strip_ansi(r) for r in modules_view.module_callees_rows(_module(mods, 0), mods, funcs)]
    assert len(callees) == 4
    assert callees[3].startswith("dependencyMap[3]  #533")
    callers = [strip_ansi(r) for r in modules_view.module_callers_rows(_module(mods, 107), mods, funcs)]
    assert any(r.startswith("#106 ") and r.endswith("as dependencyMap[0]") for r in callers)
    assert modules_view.module_callers_rows(_module(mods, 0), mods, funcs) == []


def test_module_detail_rows():
    f = _open("hermitvault.hbc")
    mods, funcs = f.recover_modules(), f.functions()
    rows = [strip_ansi(r) for r in
            modules_view.module_detail_rows(_module(mods, 0), mods, funcs, [106, 0], 12)]
    text = "\n".join(rows)
    assert rows[0] == "module #0  entry (__r)"
    assert "functions  12" in text
    assert "dependencies (4)" in text
    assert "dependencyMap[3]  #533" in text
    assert "who includes it: xrefs module:0" in text


def test_null_dependency_is_shown_as_optional():
    class Mod:  # dependencyMap [5, null] (rocketchat.hbc has such modules)
        module_id, function_index, define_offset = 7, 1, 0x10
        dependencies, dependencies_known = [5, None], True
    class Func:
        name = "factory"
    rows = [strip_ansi(r) for r in modules_view.module_callees_rows(Mod, [Mod], [Func, Func])]
    assert rows == ["dependencyMap[0]  #5  not defined in this bundle",
                    "dependencyMap[1]  null (optional dependency, unresolved)"]
    # a null entry includes no module, so it has no caller edge
    assert modules_view.module_callers(Mod, [Mod]) == []


# --- DOT ----------------------------------------------------------------------


def test_module_graph_to_dot():
    f = _open("hermitvault.hbc")
    mods, funcs = f.recover_modules(), f.functions()
    dot = cfg_dot.module_graph_to_dot(_module(mods, 0), mods, funcs, [106, 0])
    assert dot.startswith('digraph "module_0" {')
    assert "[entry]" in dot
    for i, d in enumerate([1, 2, 513, 533]):
        assert f'M0 -> M{d} [label="{i}"];' in dot


def test_module_graph_compact_caps_dependencies():
    f = _open("hermitvault.hbc")
    mods, funcs = f.recover_modules(), f.functions()
    m2 = _module(mods, 2)
    assert len(m2.dependencies) > cfg_dot._MODULE_DEPS_THRESHOLD
    dot = cfg_dot.module_graph_to_dot(m2, mods, funcs, [], compact=True)
    assert len(re.findall(r"M2 -> M\d", dot)) == cfg_dot._MODULE_DEPS_THRESHOLD
    assert f"+{len(m2.dependencies) - cfg_dot._MODULE_DEPS_THRESHOLD} more" in dot
    full = cfg_dot.module_graph_to_dot(m2, mods, funcs, [], compact=False)
    assert len(re.findall(r"M2 -> M\d", full)) == len(m2.dependencies)
    assert "MORE" not in full


def test_show_module_graph_raises_without_graph_easy(monkeypatch):
    f = _open("metro_module.hbc")
    mods = f.recover_modules()
    monkeypatch.setattr(cfg_dot, "_find_graph_easy", lambda: None)
    with pytest.raises(RuntimeError, match="graph-easy"):
        cfg_dot.show_module_graph(mods[0], mods, f.functions(), [])


# --- REPL -----------------------------------------------------------------------


def test_modules_command_lists_all(shown):
    _repl("metro_module.hbc")._dispatch("modules")
    (title, rows), = shown
    assert title.startswith("modules (3)") and "entries (__r): [2]" in title
    assert len(rows) == 3


def test_modules_command_single_module(shown):
    _repl("metro_module.hbc")._dispatch("modules module:2")
    (title, rows), = shown
    assert title.startswith("module #2")
    assert "entry" in strip_ansi(rows[0])


def test_modules_on_non_metro_file(shown, capsys):
    _repl("hello.hbc")._dispatch("modules")
    assert shown == []
    assert "No Metro modules" in capsys.readouterr().out


@pytest.mark.parametrize("arg, msg", [
    ("module:99", "No module #99"),
    ("module:x", "numeric module id"),
])
def test_bad_module_arg_raises(arg, msg):
    with pytest.raises(ValueError, match=msg):
        _repl("metro_module.hbc")._dispatch(f"modules {arg}")


def test_modules_bad_usage_prints_message(shown, capsys):
    _repl("metro_module.hbc")._dispatch("modules func:0")
    assert shown == []
    assert "Usage: modules" in capsys.readouterr().out


def test_cfg_module_calls_show_module_graph(graphs):
    r = _repl("metro_module.hbc")
    r._dispatch("cfg module:1")
    r._dispatch("cfg module:2 --compact=false")
    assert graphs == [(1, {"force_compact": None}), (2, {"force_compact": False})]


# --- functions --module ---------------------------------------------------------


def test_module_functions_partition_the_bundle():
    """Modules are disjoint; outside them only the global function and the
    Metro prelude (polyfills run before the first __d)."""
    f = _open("hermitvault.hbc")
    cg, mods = f.call_graph(), f.recover_modules()
    owner = {}
    for m in mods:
        fns = modules_view.module_functions(cg, m)
        assert fns[0] <= m.function_index and m.function_index in fns
        for i in fns:
            assert i not in owner, f"func {i} in modules {owner.get(i)} and {m.module_id}"
            owner[i] = m.module_id
    outside = sorted(set(range(len(f.functions()))) - set(owner))
    assert f.header().global_code_index in outside
    first_factory = min(m.function_index for m in mods)
    assert all(i < first_factory for i in outside)


def test_module_functions_single_function_module():
    """hermitvault #106 is InitializeCore: `_$$_REQUIRE(_dependencyMap[0]).default();`,
    no nested function."""
    f = _open("hermitvault.hbc")
    m = _module(f.recover_modules(), 106)
    assert modules_view.module_functions(f.call_graph(), m) == [m.function_index]


@pytest.mark.parametrize("line", ["functions --module=2", "functions --module 2"])
def test_functions_module_flag(shown, line):
    r = _repl("metro_module.hbc")
    m = _module(r.project.hbc.recover_modules(), 2)
    expected = modules_view.module_functions(r.project.hbc.call_graph(), m)
    r._dispatch(line)
    (title, rows), = shown
    assert f"functions ({len(expected)})  module #2" in title
    assert [int(strip_ansi(row)[1:6]) for row in rows] == expected


def test_functions_module_flag_errors(shown, capsys):
    r = _repl("metro_module.hbc")
    with pytest.raises(ValueError, match="No module #9"):
        r._dispatch("functions --module=9")
    r._dispatch("functions --bogus")
    assert "Usage: functions" in capsys.readouterr().out
    with pytest.raises(ValueError, match="No Metro modules"):
        _repl("hello.hbc")._dispatch("functions --module=0")


# --- xrefs module: ----------------------------------------------------------------


def test_xrefs_module_shows_callers_by_default(shown):
    _repl("hermitvault.hbc")._dispatch("xrefs module:107")
    (title, callers), = shown
    assert "callers" in title and "modules that include it" in title
    assert any(strip_ansi(x).startswith("#106 ") for x in callers)


def test_xrefs_module_callees_flag(shown):
    _repl("hermitvault.hbc")._dispatch("xrefs module:0 --callees")
    (title, callees), = shown
    assert "callees (4)" in title and "modules it includes" in title
    assert len(callees) == 4


def test_xrefs_module_without_callers_prints_message(shown, capsys):
    _repl("hermitvault.hbc")._dispatch("xrefs module:0")
    assert shown == []
    assert "no module includes it" in capsys.readouterr().out


def test_xrefs_module_on_non_metro_file():
    with pytest.raises(ValueError, match="No Metro modules"):
        _repl("hello.hbc")._dispatch("xrefs module:0")


# --- functions: module tags -------------------------------------------------------


def test_function_rows_tag_module_functions():
    f = _open("hermitvault.hbc")
    mods, funcs, cg = f.recover_modules(), f.functions(), f.call_graph()
    fm = modules_view.function_modules(cg, mods)
    rows = [strip_ansi(r) for r in display.functions_to_ansi_rows(
        funcs, f.header().global_code_index, fm)]
    m0 = _module(mods, 0)
    members = modules_view.module_functions(cg, m0)
    assert rows[m0.function_index].endswith("# module 0 entry")
    nested = [i for i in members if i != m0.function_index]
    assert nested and all(rows[i].endswith("# module 0") for i in nested)
    # global function and prelude: no tag
    assert "# module" not in rows[f.header().global_code_index]
    assert len(fm) == sum(len(modules_view.module_functions(cg, m)) for m in mods)


def test_functions_command_tags(shown):
    _repl("metro_module.hbc")._dispatch("functions")
    (title, rows), = shown
    assert "3 modules" in title
    plain = [strip_ansi(r) for r in rows]
    assert sum(r.endswith(" entry") for r in plain) == 3


# --- disasm module: ----------------------------------------------------------------


def test_disasm_module_shows_every_function_in_sequence(monkeypatch):
    calls = []
    monkeypatch.setattr(repl_mod, "show_list",
                        lambda rows, title="", alt_rows=None, alt_title="", full_screen=False:
                        calls.append((title, rows, alt_rows, alt_title, full_screen)))
    r = _repl("hermitvault.hbc")
    hbc = r.project.hbc
    m = _module(hbc.recover_modules(), 0)
    expected = modules_view.module_functions(hbc.call_graph(), m)
    r._dispatch("disasm module:0")
    (title, rows, alt_rows, alt_title, full_screen), = calls
    assert full_screen

    n_instrs = sum(len(hbc.disasm(i)) for i in expected)
    assert title == f"disasm  module #0  {len(expected)} functions  ({n_instrs} instrs)"
    seps = [strip_ansi(x) for x in rows if strip_ansi(x).startswith("── func:")]
    assert [int(x[len("── func:"):x.index(" (")]) for x in seps] == expected
    assert "# module 0 entry" in seps[0]
    assert all(x.split("  ")[1].startswith("# module 0") and "entry" not in x
               for x in seps[1:])
    assert alt_title == "src table  module #0" and alt_rows


def test_disasm_module_errors():
    with pytest.raises(ValueError, match="No module #9"):
        _repl("metro_module.hbc")._dispatch("disasm module:9")
    with pytest.raises(ValueError, match="No Metro modules"):
        _repl("hello.hbc")._dispatch("disasm module:0")


def test_disasm_func_is_full_screen(monkeypatch):
    calls = []
    monkeypatch.setattr(repl_mod, "show_list",
                        lambda rows, title="", alt_rows=None, alt_title="", full_screen=False:
                        calls.append(full_screen))
    _repl("hello.hbc")._dispatch("disasm func:1")
    assert calls == [True]


# --- decompile: full-screen viewer, module: -------------------------------------


@pytest.fixture
def viewer_calls(monkeypatch):
    calls = []
    monkeypatch.setattr(repl_mod, "show_list",
                        lambda rows, title="", alt_rows=None, alt_title="", full_screen=False:
                        calls.append((title, rows, full_screen)))
    return calls


def test_decompile_func_opens_full_screen_viewer(viewer_calls):
    r = _repl("hello.hbc")
    r._dispatch("decompile func:1")
    (title, rows, full_screen), = viewer_calls
    assert full_screen and title == "decompile  func:1 (greet)"
    text = "\n".join(strip_ansi(x) for x in rows)
    assert "greet" in text
    assert "decompiled by hermit" not in text      # `//` banner stripped
    expected = r.project.hbc.decompile(1)
    assert strip_ansi(rows[-1]).rstrip() == expected.rstrip().split("\n")[-1].rstrip()


def test_decompile_module_shows_every_function(viewer_calls):
    r = _repl("hermitvault.hbc")
    hbc = r.project.hbc
    m = _module(hbc.recover_modules(), 0)
    expected = modules_view.module_functions(hbc.call_graph(), m)
    r._dispatch("decompile module:0")
    (title, rows, full_screen), = viewer_calls
    assert full_screen
    assert title == f"decompile  module #0  {len(expected)} functions"
    seps = [strip_ansi(x) for x in rows if strip_ansi(x).startswith("── func:")]
    assert [int(x[len("── func:"):x.index(" (")]) for x in seps] == expected
    assert "# module 0 entry" in seps[0]


def test_decompiled_rows_preserved_source():
    hbc = _open("show_source.hbc")
    fn = next(f for f in hbc.functions() if f.hasPreservedSource and not f.sourceIsHidden)
    rows = [strip_ansi(x) for x in display.decompiled_function_rows(hbc, fn)]
    assert rows[0] == "ORIGINAL SOURCE - NOT decompiled"
    assert "\n".join(rows[1:]).strip() == fn.preservedSource.strip()


def test_decompiled_rows_failure_is_a_red_row():
    class Broken:
        def decompile(self, idx):
            raise RuntimeError("boom")

    class Func:
        index, hasPreservedSource, sourceIsHidden = 3, False, False

    rows = display.decompiled_function_rows(Broken(), Func)
    assert [strip_ansi(x) for x in rows] == ["decompile failed: boom"]


def test_decompile_module_errors():
    with pytest.raises(ValueError, match="No Metro modules"):
        _repl("hello.hbc")._dispatch("decompile module:0")


def test_module_exports_rows():
    f = _open("hermitvault.hbc")
    rows = [strip_ansi(r) for r in modules_view.module_exports_rows(f.module_exports(533))]
    assert rows[0] == "exports (2)  module.exports = {...}"
    assert rows[1:] == ['  displayName  = "HermitDemo"', '  name         = "HermitDemo"']
    rows = [strip_ansi(r) for r in modules_view.module_exports_rows(f.module_exports(60))]
    assert "ES module" in rows[0] and "export *" in rows[0]
    assert rows[1].endswith("(get)")


def test_modules_detail_shows_exports(shown):
    _repl("hermitvault.hbc")._dispatch("modules module:533")
    (title, rows), = shown
    assert any(strip_ansi(r).startswith("exports (2)") for r in rows)
