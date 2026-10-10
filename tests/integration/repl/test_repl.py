"""REPL command-level tests (`cli/repl.py`).

Drives `HermitREPL._dispatch(line)`, the entry point of `run()`, with `show_list`
made headless by a monkeypatch (a pipe input that sends "q", so the real
Application is built and rendered) and `cfg_dot.show_cfg` replaced by a stub
(the external `less`/`graph-easy` pager isn't reliably testable in CI).
"""

import pytest
from prompt_toolkit.input.defaults import create_pipe_input
from prompt_toolkit.output import DummyOutput

import cli.repl as repl_mod
from cli.repl import HermitREPL
from testutil import corpus_path


@pytest.fixture(autouse=True)
def _headless_show_list(monkeypatch):
    """Replaces cli.repl.show_list with a version that opens the view on a
    pipe input, immediately sends 'q' and closes it - exercises the REAL
    Application (layout, key bindings, rendering) instead of a mock, without
    blocking."""
    from cli.viewer import show_list as real_show_list

    def _fake(rows, title="", alt_rows=None, alt_title=""):
        if not rows:
            return
        with create_pipe_input() as inp:
            inp.send_text("q")
            real_show_list(rows, title=title, alt_rows=alt_rows, alt_title=alt_title,
                           _input=inp, _output=DummyOutput())

    monkeypatch.setattr(repl_mod, "show_list", _fake)


@pytest.fixture(autouse=True)
def _stub_show_cfg(monkeypatch):
    """cfg_dot.show_cfg spawns `less`/`graph-easy` (external subprocesses, not
    reliable in headless CI) - just records the call."""
    calls = []
    monkeypatch.setattr(repl_mod.cfg_dot, "show_cfg",
                        lambda cfg, func, **kw: calls.append((func.index, kw)))
    return calls


def _repl(corpus_name="hello.hbc"):
    r = HermitREPL()
    r._open_file(str(corpus_path(corpus_name)))
    return r


# --- basic dispatch -----------------------------------------------------


def test_dispatch_unknown_command_does_not_raise():
    r = HermitREPL()
    r._dispatch("this_command_does_not_exist")   # just console.print, never raises


def test_dispatch_empty_line_does_not_raise():
    r = HermitREPL()
    r._dispatch("")


def test_dispatch_malformed_quoting_prints_parse_error():
    r = HermitREPL()
    r._dispatch('strings "unterminated')   # shlex.split raises ValueError, caught


@pytest.mark.parametrize("line", [
    "info", "strings", "arrays", "functions", "disasm func:0", "src",
    "cfg func:0", "xrefs", "decompile func:0", "edit func:0",
    "patch out.hbc",
])
def test_commands_require_open_project(line):
    r = HermitREPL()
    with pytest.raises(RuntimeError, match="No file open"):
        r._dispatch(line)


# --- open / info ---------------------------------------------------------


def test_open_wrong_arg_count_prints_usage():
    r = HermitREPL()
    r._dispatch("open")   # no path -> Usage, not an error
    assert r.project is None


def test_open_missing_file_raises():
    r = HermitREPL()
    with pytest.raises(FileNotFoundError):
        r._dispatch("open /nonexistent/path/x.hbc")


def test_open_and_info():
    r = _repl()
    assert r.project is not None
    r._dispatch("info")


# --- strings / arrays / functions (show_list) -----------------------------


def test_strings_all_variants():
    r = _repl()
    r._dispatch("strings")
    r._dispatch("strings --identifiers")
    r._dispatch("strings --strings")
    r._dispatch("strings filter:Hello")


def test_arrays_needs_literal_sites_corpus():
    r = _repl("literals_g0.hbc")
    r._dispatch("arrays")
    r._dispatch("arrays --arrays")
    r._dispatch("arrays --objects")
    r._dispatch("arrays filter:x")
    r._dispatch("arrays func:0")


def test_functions_lists_all():
    r = _repl()
    r._dispatch("functions")


# --- disasm / src ----------------------------------------------------------


def test_disasm_valid_function():
    r = _repl()
    r._dispatch("disasm func:1")   # greet - has debug info, exercises alt_rows


def test_disasm_wrong_arg_count_prints_usage():
    r = _repl()
    r._dispatch("disasm")


def test_disasm_out_of_range_raises():
    r = _repl()
    with pytest.raises(ValueError, match="out of range"):
        r._dispatch("disasm func:999")


def test_src_all_and_single_function():
    r = _repl()
    r._dispatch("src")
    r._dispatch("src func:1")


def test_src_bad_usage_prints_message():
    r = _repl()
    r._dispatch("src func:1 extra")


# --- cfg (pager stubbed) ----------------------------------------------------


def test_cfg_calls_show_cfg(_stub_show_cfg):
    r = _repl()
    r._dispatch("cfg func:1")
    assert _stub_show_cfg == [(1, {"force_compact": None})]


def test_cfg_compact_flag(_stub_show_cfg):
    r = _repl()
    r._dispatch("cfg func:1 --compact=true")
    assert _stub_show_cfg[-1] == (1, {"force_compact": True})
    r._dispatch("cfg func:1 --compact=false")
    assert _stub_show_cfg[-1] == (1, {"force_compact": False})


def test_cfg_bad_compact_value_prints_error(_stub_show_cfg):
    r = _repl()
    r._dispatch("cfg func:1 --compact=maybe")
    assert _stub_show_cfg == []


def test_cfg_wrong_arg_count_prints_usage(_stub_show_cfg):
    r = _repl()
    r._dispatch("cfg")
    assert _stub_show_cfg == []


# --- xrefs -------------------------------------------------------------


def test_xrefs_no_args_lists_all_referenced_strings():
    r = _repl()
    r._dispatch("xrefs")


def test_xrefs_by_string_index_and_value():
    r = _repl()
    r._dispatch("xrefs string:1")
    r._dispatch('xrefs string:"Hello, "')


def test_xrefs_string_out_of_range_prints_error():
    r = _repl()
    r._dispatch("xrefs string:99999")


def test_xrefs_unknown_string_value_prints_no_references():
    r = _repl()
    r._dispatch("xrefs string:nope_not_a_real_string")


def test_xrefs_by_function_callers_and_callees():
    r = _repl()
    r._dispatch("xrefs func:1 --callers")
    r._dispatch("xrefs func:0 --callees")
    r._dispatch("xrefs func:0")   # no flag -> both


def test_xrefs_bad_usage_prints_message():
    r = _repl()
    r._dispatch("xrefs bogus:1")


# --- decompile -----------------------------------------------------------


def test_decompile_single_function():
    r = _repl()
    r._dispatch("decompile func:1")


def test_decompile_bad_usage_prints_message():
    r = _repl()
    r._dispatch("decompile")


def test_decompile_all_writes_files(tmp_path):
    r = _repl()
    out_dir = tmp_path / "decompiled"
    r._dispatch(f"decompile all --output={out_dir}")
    written = list(out_dir.glob("func_*.js"))
    assert len(written) == len(r.project.hbc.functions())


# --- patch ---------------------------------------------------------------


def test_patch_without_modifications_prints_hint():
    r = _repl()
    assert r.project.is_patched is False
    r._dispatch("patch out.hbc")   # no changes applied -> just a hint, no file


def test_patch_refuses_to_overwrite_original(tmp_path):
    r = _repl()
    r.project.apply_add_string("bonus_test_string")
    assert r.project.is_patched is True
    r._dispatch(f"patch {r.project.hbc_path}")   # same path as the original -> rejected


def test_patch_writes_file(tmp_path):
    r = _repl()
    r.project.apply_add_string("bonus_test_string_2")
    out_path = tmp_path / "hello_patched_repl.hbc"
    r._dispatch(f"patch {out_path}")
    assert out_path.exists()
    assert out_path.stat().st_size > 0


# --- help / exit / quit -----------------------------------------------------


def test_help_lists_every_command():
    r = HermitREPL()
    r._dispatch("help")   # no project required


def test_exit_raises_system_exit_when_clean():
    r = _repl()
    with pytest.raises(SystemExit):
        r._dispatch("exit")


def test_quit_is_an_alias_for_exit():
    r = _repl()
    with pytest.raises(SystemExit):
        r._dispatch("quit")


def test_history_survives_missing_home(tmp_path, monkeypatch):
    """Without a writable home directory the history is simply not saved."""
    monkeypatch.setenv("HOME", str(tmp_path / "does-not-exist"))
    r = HermitREPL()
    r.session.history.append_string("info")   # must not raise
    assert not (tmp_path / "does-not-exist").exists()
