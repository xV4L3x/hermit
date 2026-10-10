"""same-length string edit (`edit string:<idx>`).

Same-size mechanics applied to the string storage: `Ctrl+X -> Y` re-encodes
the new value (identical byte length), `HermitProject.apply_string_patch`
splices it into the in-memory `.hbc` image (SHA-1 footer, re-parse, string-section
invariants). The `.hbc` on disk is written by `patch`.
"""

import hashlib
import subprocess
from collections import namedtuple
from pathlib import Path

import pytest

import _libhbc
from cli.editor import (
    StringEditState,
    _StrEditError,
    encode_string_commit,
    encode_string_value,
)
from cli.repl import HermitREPL
from testutil import CORPUS, CX, ESC, HERMES, drive_strings, load_project


BS = "\x7f"      # backspace


def _idx(proj, value):
    for i, s in enumerate(proj.hbc.strings()):
        if s == value:
            return i
    raise AssertionError(f"no string == {value!r}")


def _state(proj, idx, *, with_xrefs=False):
    entries = proj.hbc.string_entries()
    kinds = proj.hbc.string_kinds()
    strings = proj.hbc.strings()
    e = entries[idx]
    off = e.storage_offset
    img = proj.image_bytes
    lo = max(0, off - 16)
    xrefs = ([x for x in proj.hbc.string_xrefs() if x.string_index == idx]
             if with_xrefs else [])
    funcs = proj.hbc.functions() if with_xrefs else []
    return StringEditState(idx, strings, kinds, entries, xrefs, funcs,
                           context_bytes=img[lo:off + e.byte_length + 16],
                           context_base=lo)


def _commit(proj, idx, keys):
    """Drives the editor and, if the user confirms, applies the patch - like
    `HermitREPL._edit_string`."""
    st = _state(proj, idx)
    drive_strings(st, keys + CX + "y")
    assert st.commit is True
    patches = encode_string_commit(st)
    (blob,) = patches.values()
    proj.apply_string_patch(idx, blob, st.value)
    return st, patches


# --- pure encoder ----------------------------------------------------------

Entry = namedtuple("Entry", "is_utf16 byte_length storage_offset")


def _e8(n, off=0):
    return Entry(False, n, off)


def _e16(n, off=0):
    return Entry(True, n, off)


def test_encode_ascii_same_length():
    assert encode_string_value("HELLO, ", _e8(7)) == b"HELLO, "


def test_encode_rejects_wrong_length():
    with pytest.raises(_StrEditError, match="≠ 7"):
        encode_string_value("Hello!", _e8(7))


def test_encode_rejects_non_ascii_on_8bit():
    with pytest.raises(_StrEditError, match="non-ASCII"):
        encode_string_value("Héllo, ", _e8(7))


def test_encode_rejects_empty():
    with pytest.raises(_StrEditError, match="empty"):
        encode_string_value("", _e8(7))


def test_encode_utf16_bmp():
    assert encode_string_value("WXYZ", _e16(8)) == b"W\x00X\x00Y\x00Z\x00"


def test_encode_utf16_rejects_non_bmp():
    with pytest.raises(_StrEditError, match="BMP"):
        encode_string_value("WXY\U0001F600", _e16(8))


def test_encode_utf16_wrong_length():
    with pytest.raises(_StrEditError, match="≠ 8"):
        encode_string_value("WXY", _e16(8))


def test_encode_string_commit_noop_when_unchanged():
    st = _state(load_project("hello.hbc"), 1)
    assert encode_string_commit(st) == {}


def test_encode_string_commit_rejects_identifier():
    proj = load_project("hello.hbc")
    st = _state(proj, _idx(proj, "log"))   # Identifier
    st.value = "LOG"
    with pytest.raises(_StrEditError, match="Identifier"):
        encode_string_commit(st)


# --- headless editor -----------------------------------------------------


def test_happy_path_rewrites_string_in_memory():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "Hello, ")
    e = proj.hbc.string_entries()[idx]
    off = e.storage_offset
    others_before = [s for j, s in enumerate(proj.hbc.strings()) if j != idx]

    st, patches = _commit(proj, idx, BS * 7 + "HELLO, ")

    assert st.value == "HELLO, "
    assert patches == {off: b"HELLO, "}
    assert proj.is_patched is True
    assert proj.hbc.strings()[idx] == "HELLO, "
    assert [s for j, s in enumerate(proj.hbc.strings()) if j != idx] == others_before
    h = proj.hbc.header()
    assert h.string_count == 8
    assert h.string_storage_size == 44


def test_ctrlx_without_change_exits_immediately():
    proj = load_project("hello.hbc")
    st = _state(proj, _idx(proj, "Hello, "))
    drive_strings(st, CX)
    assert st.commit is False
    assert st.mode == "edit"
    assert proj.is_patched is False


def test_wrong_length_no_longer_blocks_in_editor():
    """The resize path (`cli.editor.StringEditState.commit_buf`) calls
    `encode_string_new_value` (no length constraint) instead of
    `encode_string_value` - a different length in the interactive EDITOR
    now proceeds to `exit_ask` instead of staying stuck in `edit`. The same-length
    constraint stays intact as a pure function (`encode_string_value`, see
    `test_encode_rejects_wrong_length` above) and in the same-size path
    `apply_string_patch`/`encode_string_commit` - only the interactive
    editor's commit changes, now dispatching to `apply_string_relocation`
    (see `test_editor_string_resize.py`)."""
    proj = load_project("hello.hbc")
    st = _state(proj, _idx(proj, "Hello, "))
    drive_strings(st, BS * 7 + "HI")
    drive_strings(st, CX)
    assert st.mode == "exit_ask"   # same-length: stayed "edit"; resize: proceeds
    assert st.value == "HI"


def test_bad_encoding_still_blocks_in_edit():
    """The encoding (ASCII on an 8-bit entry) remains a blocking constraint even
    on the resize path - only the LENGTH constraint was removed."""
    proj = load_project("hello.hbc")
    st = _state(proj, _idx(proj, "Hello, "))
    drive_strings(st, BS * 7 + "café")
    drive_strings(st, CX)
    assert st.mode == "edit"       # never reaches exit_ask
    assert st.commit is False
    assert st.edit_err
    assert st.value == "Hello, "   # unchanged


def test_exit_ask_no_discards():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "Hello, ")
    st = _state(proj, idx)
    drive_strings(st, BS * 7 + "HELLO, " + CX + "n")
    assert st.commit is False
    assert proj.is_patched is False


def test_escape_reverts_buffer():
    proj = load_project("hello.hbc")
    st = _state(proj, _idx(proj, "Hello, "))
    drive_strings(st, BS * 7 + "ZZZZZZZ" + ESC)
    assert st.buf == "Hello, "
    assert st.value == "Hello, "
    assert st.modified is False


def test_editor_opens_with_xrefs_body():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "Hello, ")
    st = _state(proj, idx, with_xrefs=True)
    drive_strings(st, CX)   # just opening + clean exit
    assert st.commit is False


# --- pre-open: rejections in the REPL, the editor doesn't open -------


def _repl(name):
    r = HermitREPL()
    r._open_file(str(CORPUS / name))
    return r


def test_edit_string_rejects_identifier_before_opening():
    if not (CORPUS / "hello.hbc").exists():
        pytest.skip("missing corpus: hello.hbc")
    r = _repl("hello.hbc")
    idx = _idx(r.project, "log")   # Identifier + suffix-shared
    r._edit_string(r.project, f"string:{idx}")
    assert r.project.is_patched is False


# NOTE: the pre-open rejection for suffix-sharing (`global`, only
# shared, kind String) was REMOVED - the editor now always opens, shows
# a "shared" banner and does the detach on commit. The old test
# `test_edit_string_rejects_suffix_shared_before_opening` described a
# behavior that no longer exists; coverage of the new behavior
# (opens + banner + detach) is in `test_editor_string_resize.py`.


def test_resolve_string_arg():
    if not (CORPUS / "hello.hbc").exists():
        pytest.skip("missing corpus: hello.hbc")
    r = _repl("hello.hbc")
    assert r._resolve_string_arg("string:1") == 1
    assert r._resolve_string_arg("string:Hello, ") == 1
    with pytest.raises(ValueError, match="out of range"):
        r._resolve_string_arg("string:999")
    with pytest.raises(ValueError, match="No string equal"):
        r._resolve_string_arg("string:definitely-not-here")


# --- sequential edits accumulate on the image ------------------------


def test_sequential_string_edits_stack():
    proj = load_project("hello.hbc")
    i3 = _idx(proj, "hermit")
    i4 = _idx(proj, "use strict")
    orig = Path(proj.hbc_path).read_bytes()

    _commit(proj, i3, BS * 6 + "HERMIT")
    _commit(proj, i4, BS * 10 + "USE STRICT")

    assert proj.hbc.strings()[i3] == "HERMIT"
    assert proj.hbc.strings()[i4] == "USE STRICT"
    img = proj.image_bytes
    assert len(img) == len(orig)
    # only the bytes actually different (the space in "use strict" stays a space)
    expected = (sum(a != b for a, b in zip("hermit", "HERMIT"))
                + sum(a != b for a, b in zip("use strict", "USE STRICT")))
    assert sum(a != b for a, b in zip(orig[:-20], img[:-20])) == expected


# --- apply_string_patch: integrity check ---------------------------


def test_apply_string_patch_rejects_wrong_size_blob():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "Hello, ")
    with pytest.raises(ValueError, match="length"):
        proj.apply_string_patch(idx, b"SHORT", "SHORT")
    assert proj.is_patched is False
    assert proj.image_bytes == Path(proj.hbc_path).read_bytes()


def test_apply_string_patch_rejects_identifier():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "log")
    with pytest.raises(ValueError, match="Identifier"):
        proj.apply_string_patch(idx, b"LOG", "LOG")
    assert proj.is_patched is False


def test_apply_string_patch_rejects_suffix_shared():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "global")   # kind String but suffix-shared with "log"
    with pytest.raises(ValueError, match="suffix-sharing"):
        proj.apply_string_patch(idx, b"GLOBAL", "GLOBAL")
    assert proj.is_patched is False


# --- write_patched after a string commit ----------------------


def test_write_patched_after_string_commit(tmp_path):
    proj = load_project("hello.hbc")
    idx = _idx(proj, "Hello, ")
    e = proj.hbc.string_entries()[idx]
    lo, hi = e.storage_offset, e.storage_offset + e.byte_length
    orig = Path(proj.hbc_path).read_bytes()

    _commit(proj, idx, BS * 7 + "HELLO, ")

    out = tmp_path / "hello_samesize.hbc"
    n = proj.write_patched(str(out))
    new = out.read_bytes()

    assert n == len(new) == len(orig)
    assert hashlib.sha1(new[:-20]).digest() == new[-20:]
    changed = [i for i in range(len(orig)) if orig[i] != new[i]]
    assert changed
    for i in changed:
        assert (lo <= i < hi) or (len(orig) - 20 <= i < len(orig)), \
            f"byte {i:#x} modified outside [string storage] ∪ [footer]"

    chk = _libhbc.HBCFile.open(str(out))
    assert chk.strings()[idx] == "HELLO, "
    assert chk.header().file_length == len(orig)


# --- hermes oracle: the runtime prints the new value --------


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_hermes_prints_patched_string(tmp_path):
    name = "greeting_g0.hbc"
    if not (CORPUS / name).exists():
        pytest.skip(f"missing corpus: {name} - `seeder.compile --g0 seeder/samples/greeting.js`")
    proj = load_project(name)
    try:
        idx = _idx(proj, "Hello, world")
    except AssertionError:
        pytest.skip("greeting_g0.hbc: unexpected string layout")
    e = proj.hbc.string_entries()[idx]
    if e.is_overflow or e.is_utf16:
        pytest.skip("greeting_g0.hbc: unexpected overflow/UTF-16 entry")

    r_orig = subprocess.run([HERMES, str(CORPUS / name)],
                            capture_output=True, text=True, timeout=30)
    assert "Hello, world" in r_orig.stdout
    assert r_orig.returncode == 0

    _commit(proj, idx, BS * 12 + "HELLO, world")

    out = tmp_path / "greeting_patched.hbc"
    proj.write_patched(str(out))
    reopened = _libhbc.HBCFile.open(str(out))
    assert reopened.strings()[idx] == "HELLO, world"
    assert reopened.header().file_length == len(Path(proj.hbc_path).read_bytes())
    data = out.read_bytes()
    assert hashlib.sha1(data[:-20]).digest() == data[-20:]

    r_new = subprocess.run([HERMES, str(out)],
                           capture_output=True, text=True, timeout=30)
    assert r_new.returncode == 0
    assert "HELLO, world" in r_new.stdout
    assert "Hello, world" not in r_new.stdout
