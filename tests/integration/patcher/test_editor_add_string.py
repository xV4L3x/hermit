"""`add_string` + first multi-section rebuild.

`edit string:new` (or `Ctrl+N` from an `edit string:<idx>`) opens the editor in
"new string" mode; `Ctrl+X -> Y` calls `HermitProject.apply_add_string`,
which via `Module` appends the string (kind `String`) to the string kind table /
string table / string storage and re-lays-out every subsequent section.
"""

import hashlib
import subprocess
from pathlib import Path

import pytest
from prompt_toolkit.input.defaults import create_pipe_input
from prompt_toolkit.output import DummyOutput

import _libhbc
from cli.editor import (
    EditorState,
    StringEditState,
    encode_committed_lines,
    encode_new_string,
    run_editor,
    _StrEditError,
)
from testutil import (
    CC,
    CN,
    CORPUS,
    CX,
    DOWN,
    ENTER,
    ESC,
    HERMES,
    RIGHT,
    drive_strings,
    load_project,
)


def _add_state(proj):
    return StringEditState(0, proj.hbc.strings(), proj.hbc.string_kinds(),
                           proj.hbc.string_entries(), (), (), is_add=True)


# --- pure encoder ----------------------------------------------------------


def test_encode_new_string_ascii():
    assert encode_new_string("Hello 123") == b"Hello 123"


def test_encode_new_string_rejects_empty():
    with pytest.raises(_StrEditError, match="empty"):
        encode_new_string("")


def test_encode_new_string_rejects_non_ascii():
    with pytest.raises(_StrEditError, match="non-ASCII"):
        encode_new_string("café")


# --- apply_add_string --------------------------------------------------


def test_apply_add_string_appends():
    proj = load_project("hello.hbc")
    before = list(proj.hbc.strings())
    n0 = proj.hbc.header().string_count

    idx = proj.apply_add_string("HERMITADD")

    assert idx == n0
    assert proj.is_patched is True
    assert proj.hbc.strings()[idx] == "HERMITADD"
    assert list(proj.hbc.strings())[:idx] == before
    h = proj.hbc.header()
    assert h.string_count == n0 + 1
    assert h.string_storage_size == 44 + len("HERMITADD")
    assert h.string_kind_count == 3          # was 2, last run Identifier -> +1
    assert proj.hbc.string_kinds()[idx] == _libhbc.StringKind.String


def test_apply_add_string_on_file_with_array_literal():
    # medium.hbc has array/object buffers: exercises the section_map fix (align 4).
    proj = load_project("medium.hbc")
    n0 = proj.hbc.header().string_count
    idx = proj.apply_add_string("xyz")
    assert idx == n0
    assert proj.hbc.strings()[idx] == "xyz"
    # every function re-disassembles identically (the bodies are only shifted)
    for f in proj.hbc.functions():
        assert all(i.mnemonic != "???" for i in proj.hbc.disasm(f.index))


def test_apply_add_string_sequential():
    proj = load_project("hello.hbc")
    n0 = proj.hbc.header().string_count
    a = proj.apply_add_string("first")
    b = proj.apply_add_string("second")
    assert (a, b) == (n0, n0 + 1)
    assert proj.hbc.strings()[a] == "first"
    assert proj.hbc.strings()[b] == "second"


def test_apply_add_string_rejects_non_ascii():
    proj = load_project("hello.hbc")
    with pytest.raises(ValueError):
        proj.apply_add_string("caffè")
    assert proj.is_patched is False


def test_apply_add_string_overflow():
    proj = load_project("hello.hbc")
    big = "Z" * 300
    idx = proj.apply_add_string(big)
    assert proj.hbc.strings()[idx] == big
    assert proj.hbc.header().overflow_string_count == 1


# --- headless editor -----------------------------------------------------


def test_editor_add_mode_happy_path():
    proj = load_project("hello.hbc")
    st = _add_state(proj)
    drive_strings(st, "abc" + CX + "y")
    assert st.commit is True
    assert st.is_add is True
    assert st.value == "abc"

    idx = proj.apply_add_string(st.value)
    assert proj.hbc.strings()[idx] == "abc"


def test_editor_add_mode_empty_exits_without_commit():
    proj = load_project("hello.hbc")
    st = _add_state(proj)
    drive_strings(st, CX)                    # nothing typed
    assert st.commit is False
    assert proj.is_patched is False


def test_editor_add_mode_non_ascii_stays_in_edit():
    proj = load_project("hello.hbc")
    st = _add_state(proj)
    drive_strings(st, "cafè" + CX)
    assert st.mode == "edit"
    assert st.commit is False
    assert "non-ASCII" in st.edit_err


def test_ctrl_n_switches_existing_edit_to_add():
    proj = load_project("hello.hbc")
    strings = proj.hbc.strings()
    entries = proj.hbc.string_entries()
    idx = strings.index("Hello, ")
    e = entries[idx]
    off = e.storage_offset
    img = proj.image_bytes
    st = StringEditState(idx, strings, proj.hbc.string_kinds(), entries, (), (),
                         context_bytes=img[max(0, off - 16):off + e.byte_length + 16],
                         context_base=max(0, off - 16))
    assert st.is_add is False
    drive_strings(st, CN + "NEWONE")
    assert st.is_add is True
    assert st.buf == "NEWONE"
    # Esc returns to the original string
    drive_strings(st, ESC)
    assert st.is_add is False
    assert st.buf == "Hello, "


def test_edit_string_new_via_repl():
    from cli.repl import HermitREPL
    if not (CORPUS / "hello.hbc").exists():
        pytest.skip("missing corpus: hello.hbc")
    r = HermitREPL()
    r._open_file(str(CORPUS / "hello.hbc"))
    proj = r.project

    st = StringEditState(0, proj.hbc.strings(), proj.hbc.string_kinds(),
                         proj.hbc.string_entries(), (), (), is_add=True)
    st.commit = True
    st.value = "VIA_REPL"
    r._finish_add_string(proj, st)
    assert proj.is_patched is True
    assert proj.hbc.strings()[-1] == "VIA_REPL"


# --- write_patched + hermes oracle --------------------------------


def test_write_patched_after_add_string(tmp_path):
    proj = load_project("hello.hbc")
    orig_len = len(Path(proj.hbc_path).read_bytes())
    idx = proj.apply_add_string("APPENDED")

    out = tmp_path / "hello_add_string.hbc"
    n = proj.write_patched(str(out))
    data = out.read_bytes()

    assert n == len(data) == len(proj.image_bytes)
    assert len(data) > orig_len                       # the file has grown
    assert hashlib.sha1(data[:-20]).digest() == data[-20:]
    chk = _libhbc.HBCFile.open(str(out))
    assert chk.strings()[idx] == "APPENDED"
    assert chk.header().file_length == len(data)


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_hermes_runs_file_with_added_string_referenced(tmp_path):
    name = "greeting_g0.hbc"
    if not (CORPUS / name).exists():
        pytest.skip(f"missing corpus: {name}")
    proj = load_project(name)
    r_orig = subprocess.run([HERMES, str(CORPUS / name)],
                            capture_output=True, text=True, timeout=30)
    assert r_orig.returncode == 0

    idx = proj.apply_add_string("M8_STRING_OK")

    # re-point "Hello, world"'s LoadConstString to the new index (same-size)
    target = None
    for f in proj.hbc.functions():
        for k, ins in enumerate(proj.hbc.disasm(f.index)):
            if (ins.mnemonic == "LoadConstString" and len(ins.operands) >= 2
                    and proj.hbc.strings()[ins.operands[1].u32] == "Hello, world"):
                target = (f.index, k)
    if target is None:
        pytest.skip("greeting_g0.hbc: no LoadConstString 'Hello, world'")
    fidx, k = target
    es = EditorState(proj.hbc.functions()[fidx], proj.hbc.disasm(fidx),
                     strings=proj.hbc.strings(),
                     opcodes=_libhbc.opcode_table(proj.hbc.version()),
                     version=proj.hbc.version())
    with create_pipe_input() as inp:
        inp.send_text(DOWN * k + RIGHT + RIGHT + ENTER + str(idx) + ENTER + CX + "y" + CC)
        run_editor(es.func, [], _input=inp, _output=DummyOutput(), _state=es)
    proj.apply_patch(fidx, encode_committed_lines(es))

    out = tmp_path / "greeting_add_string.hbc"
    proj.write_patched(str(out))
    r_new = subprocess.run([HERMES, str(out)], capture_output=True, text=True, timeout=30)
    assert r_new.returncode == 0
    assert "M8_STRING_OK" in r_new.stdout
    assert "Hello, world" not in r_new.stdout
