"""`edit identifier:new` (B3 M4): a new Identifier string from the REPL.

The string editor in "new string" mode with kind Identifier; `Ctrl+X -> Y`
calls `HermitProject.apply_add_identifier`, which appends the string, extends
or opens an Identifier run in the kind table and appends its hash to the
Identifier Hash Table (`Module.add_identifier`). End to end: the new name is used
by a `PutById` edited with `edit func:` and the VM runs the patched file.

Sample: `seeder/samples/identifiers.js`, compiled at test time.
"""

import subprocess

import pytest
from prompt_toolkit.input.defaults import create_pipe_input
from prompt_toolkit.output import DummyOutput

import _libhbc
from cli.editor import (
    EditorState, StringEditState, _StrEditError, encode_committed_lines,
    encode_new_identifier, run_editor,
)
from cli.project import HermitProject
from test_identifier_add import EXPECTED, SAMPLE, _compiler
from testutil import CC, CX, ENTER, HERMES, RIGHT, DOWN, drive_strings

ID = _libhbc.StringKind.Identifier


@pytest.fixture
def sample(tmp_path):
    if HERMES is None:
        pytest.skip("hermes not available")
    out = tmp_path / "identifiers_g0.hbc"
    r = subprocess.run([_compiler(96), "-emit-binary", "-g0", "-out", str(out), str(SAMPLE)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    return out


@pytest.fixture
def proj(sample):
    return HermitProject.from_hbc(str(sample))


def _add_state(proj):
    return StringEditState(0, proj.hbc.strings(), proj.hbc.string_kinds(),
                           proj.hbc.string_entries(), (), (), is_add=True, add_kind=ID)


# --- encoder ---------------------------------------------------------------


def test_encode_new_identifier_rules(proj):
    strings, kinds = proj.hbc.strings(), proj.hbc.string_kinds()
    assert encode_new_identifier("epsilon", strings, kinds) == b"epsilon"
    with pytest.raises(_StrEditError, match="empty"):
        encode_new_identifier("", strings, kinds)
    with pytest.raises(_StrEditError, match="non-ASCII"):
        encode_new_identifier("è", strings, kinds)
    with pytest.raises(_StrEditError, match="already exists: string:"):
        encode_new_identifier("alpha", strings, kinds)
    # A String with that text does not count: the name is not a symbol yet.
    assert encode_new_identifier("beta value", strings, kinds) == b"beta value"


# --- apply_add_identifier ----------------------------------------------------


def test_apply_add_identifier(proj):
    h0 = proj.hbc.header()
    hashes0 = list(proj.hbc.identifier_hashes())
    idx = proj.apply_add_identifier("epsilon")

    h1 = proj.hbc.header()
    assert idx == h0.string_count
    assert proj.is_patched is True
    assert proj.hbc.strings()[idx] == "epsilon"
    assert proj.hbc.string_kinds()[idx] == ID
    assert h1.identifier_count == h0.identifier_count + 1
    assert h1.string_kind_count == h0.string_kind_count      # last run was Identifier
    assert list(proj.hbc.identifier_hashes()) == hashes0 + [_libhbc.identifier_hash(b"epsilon")]


def test_apply_add_identifier_after_add_string_opens_a_run(proj):
    k0 = proj.hbc.header().string_kind_count
    proj.apply_add_string("plain")                            # opens a String run
    proj.apply_add_identifier("epsilon")                      # opens an Identifier run
    assert proj.hbc.header().string_kind_count == k0 + 2
    assert proj.hbc.string_kinds()[-2:] == [_libhbc.StringKind.String, ID]


def test_apply_add_identifier_refuses_existing(proj):
    with pytest.raises(ValueError, match="already exists"):
        proj.apply_add_identifier("alpha")
    assert proj.is_patched is False


def test_apply_add_identifier_refuses_non_ascii(proj):
    with pytest.raises(ValueError):
        proj.apply_add_identifier("caffè")
    assert proj.is_patched is False


# --- headless editor -------------------------------------------------------


def test_editor_identifier_mode_happy_path(proj):
    st = _add_state(proj)
    assert st.adds_identifier is True
    drive_strings(st, "epsilon" + CX + "y")
    assert st.commit is True
    assert st.value == "epsilon"


def test_editor_identifier_mode_duplicate_stays_in_edit(proj):
    st = _add_state(proj)
    drive_strings(st, "alpha" + CX)
    assert st.mode == "edit"
    assert st.commit is False
    assert "already exists" in st.edit_err


def test_editor_string_mode_accepts_identifier_text(proj):
    """`edit string:new` is unchanged: a String with an existing name is allowed."""
    st = StringEditState(0, proj.hbc.strings(), proj.hbc.string_kinds(),
                         proj.hbc.string_entries(), (), (), is_add=True)
    assert st.adds_identifier is False
    drive_strings(st, "alpha" + CX + "y")
    assert st.commit is True


# --- REPL --------------------------------------------------------------------


def test_repl_finish_add_identifier(sample):
    from cli.repl import HermitREPL
    r = HermitREPL()
    r._open_file(str(sample))
    proj = r.project
    st = _add_state(proj)
    st.commit = True
    st.value = "VIA_REPL"
    r._finish_add_string(proj, st)
    assert proj.is_patched is True
    assert proj.hbc.strings()[-1] == "VIA_REPL"
    assert proj.hbc.string_kinds()[-1] == ID


def test_repl_edit_identifier_usage(sample, capsys):
    from cli.repl import HermitREPL
    r = HermitREPL()
    r._open_file(str(sample))
    r.cmd_edit(["identifier:3"])
    assert r.project.is_patched is False


# --- end to end on the VM --------------------------------------------------


def test_new_identifier_used_by_putbyid_runs(proj, tmp_path):
    """`o.delta = 4` becomes `o.epsilon = 4` through `edit func:`: `o.delta` reads
    undefined, so the sum prints NaN; the VM does not crash."""
    idx = proj.apply_add_identifier("epsilon")

    main = next(f for f in proj.hbc.functions() if f.name == "main")
    instrs = proj.hbc.disasm(main.index)
    delta = list(proj.hbc.strings()).index("delta")
    k = next(i for i, ins in enumerate(instrs)
             if ins.mnemonic == "PutById" and ins.operands[3].u32 == delta)
    es = EditorState(main, instrs, strings=proj.hbc.strings(),
                     opcodes=_libhbc.opcode_table(proj.hbc.version()),
                     version=proj.hbc.version())
    with create_pipe_input() as inp:
        inp.send_text(DOWN * k + RIGHT * 4 + ENTER + str(idx) + ENTER + CX + "y" + CC)
        run_editor(es.func, [], _input=inp, _output=DummyOutput(), _state=es)
    proj.apply_patch(main.index, encode_committed_lines(es))

    out = tmp_path / "patched.hbc"
    proj.write_patched(str(out))
    r = subprocess.run([HERMES, str(out)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines() == ["1", "1", "NaN", "7", "beta value"]
    assert r.stdout.splitlines() != EXPECTED
