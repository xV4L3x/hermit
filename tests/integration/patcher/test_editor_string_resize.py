"""String length change (extends the same-length edit, add_string rebuild).

`edit string:<idx>` no longer has the same-length edit's fixed-length constraint: a new
value of a different length (or on a shared entry, whatever the
length) routes to `HermitProject.apply_string_relocation` - appends to the end
of the string storage + re-points ONLY that entry, via `Module`. A
shared entry (suffix/prefix-sharing) is no longer rejected pre-open: the editor
always opens, shows a banner, and the commit detaches it from the group.
"""

import hashlib
import subprocess
from pathlib import Path

import pytest

import _libhbc
from cli.editor import (
    StringEditState,
    _StrEditError,
    encode_string_new_value,
    encode_string_relocation_commit,
)
from cli.repl import HermitREPL
from testutil import CORPUS, CX, HERMES, drive_strings, load_project


BS = "\x7f"      # backspace


def _idx(proj, value):
    for i, s in enumerate(proj.hbc.strings()):
        if s == value:
            return i
    raise AssertionError(f"no string == {value!r}")


def _shared_with(proj, idx):
    """Replicates the scan in `cli.repl._edit_string` (informational, not
    a rejection)."""
    entries = proj.hbc.string_entries()
    e = entries[idx]
    a0, a1 = e.storage_offset, e.storage_offset + e.byte_length
    if e.byte_length == 0:
        return []
    return [j for j, o in enumerate(entries)
            if j != idx and o.byte_length > 0
            and a0 < o.storage_offset + o.byte_length
            and o.storage_offset < a1]


def _state(proj, idx):
    entries = proj.hbc.string_entries()
    kinds = proj.hbc.string_kinds()
    strings = proj.hbc.strings()
    e = entries[idx]
    off = e.storage_offset
    img = proj.image_bytes
    lo = max(0, off - 16)
    shared = _shared_with(proj, idx)
    return StringEditState(idx, strings, kinds, entries, (), (),
                           context_bytes=img[lo:off + e.byte_length + 16],
                           context_base=lo,
                           is_shared=bool(shared), shared_with=shared)


def _commit(proj, idx, keys):
    """Drives the editor and applies the commit - replicates the dispatch of
    `HermitREPL._edit_string`."""
    st = _state(proj, idx)
    drive_strings(st, keys + CX + "y")
    assert st.commit is True
    entries = proj.hbc.string_entries()
    e = entries[idx]
    enc = encode_string_relocation_commit(st)
    assert enc is not None
    same_length = len(enc) == e.byte_length
    if same_length and not st.shared_with:
        proj.apply_string_patch(idx, enc, st.value)
    else:
        proj.apply_string_relocation(idx, enc, st.value)
    return st


# --- pure encoder ----------------------------------------------------------

Entry = type("Entry", (), {})


def _e8(n):
    e = Entry(); e.is_utf16 = False; e.byte_length = n; return e


def _e16(n):
    e = Entry(); e.is_utf16 = True; e.byte_length = n; return e


def test_encode_new_value_grow_shrink_8bit():
    assert encode_string_new_value("a much longer value", _e8(10)) == \
        b"a much longer value"
    assert encode_string_new_value("hi", _e8(10)) == b"hi"


def test_encode_new_value_grow_shrink_utf16():
    assert encode_string_new_value("WXYZ", _e16(4)) == "WXYZ".encode("utf-16-le")
    assert encode_string_new_value("W", _e16(4)) == "W".encode("utf-16-le")


def test_encode_new_value_rejects_empty():
    with pytest.raises(_StrEditError, match="empty"):
        encode_string_new_value("", _e8(5))


def test_encode_new_value_rejects_non_ascii():
    with pytest.raises(_StrEditError, match="non-ASCII"):
        encode_string_new_value("café", _e8(5))


def test_encode_new_value_rejects_non_bmp():
    with pytest.raises(_StrEditError, match="BMP"):
        encode_string_new_value("a\U0001F600b", _e16(6))


def test_encode_string_relocation_commit_noop_when_unchanged():
    proj = load_project("hello.hbc")
    st = _state(proj, _idx(proj, "hermit"))
    assert encode_string_relocation_commit(st) is None


def test_encode_string_relocation_commit_rejects_identifier():
    proj = load_project("hello.hbc")
    st = _state(proj, _idx(proj, "log"))
    st.value = "LOG"
    with pytest.raises(_StrEditError, match="Identifier"):
        encode_string_relocation_commit(st)


# --- headless editor: shared entry always opens, shows a banner --------


def test_shared_entry_opens_with_banner_not_rejected():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "global")   # shared with "log"
    st = _state(proj, idx)
    assert st.is_shared is True
    assert st.shared_with == [_idx(proj, "log")]


def test_non_shared_entry_has_no_banner():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "hermit")   # standalone
    st = _state(proj, idx)
    assert st.is_shared is False
    assert st.shared_with == []


# --- dispatch: same-size+non-shared -> same-length edit, otherwise -> resize -----------


def test_shared_same_length_uses_relocation_not_patch():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "global")           # 6 chars
    log_idx = _idx(proj, "log")
    before_log = proj.hbc.strings()[log_idx]

    _commit(proj, idx, BS * 6 + "planet")   # 6 chars, same length

    assert proj.hbc.strings()[idx] == "planet"
    assert proj.hbc.strings()[log_idx] == before_log   # clean detach


def test_non_shared_same_length_uses_apply_string_patch():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "hermit")
    _commit(proj, idx, BS * 6 + "HERMIT")   # same length, not shared
    assert proj.hbc.strings()[idx] == "HERMIT"


def test_non_shared_different_length_grows():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "hermit")
    others_before = [s for j, s in enumerate(proj.hbc.strings()) if j != idx]
    _commit(proj, idx, BS * 6 + "HERMIT-LONGER-VALUE")
    assert proj.hbc.strings()[idx] == "HERMIT-LONGER-VALUE"
    assert [s for j, s in enumerate(proj.hbc.strings()) if j != idx] == others_before


def test_non_shared_different_length_shrinks():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "use strict")   # 10 char
    _commit(proj, idx, BS * 10 + "ok")
    assert proj.hbc.strings()[idx] == "ok"


# --- Case B: promotion to overflow rejected --------------


def test_apply_string_relocation_rejects_overflow_promotion():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "use strict")
    big = "x" * 300
    with pytest.raises(ValueError, match="overflow"):
        proj.apply_string_relocation(idx, big.encode("ascii"), big)
    assert proj.is_patched is False


# --- empty entry -> real string ---------------------------------------


def test_empty_entry_grows_into_real_string():
    proj = load_project("empty_string_g0.hbc")
    strings = proj.hbc.strings()
    idx = next(i for i, s in enumerate(strings) if s == "")
    entries = proj.hbc.string_entries()
    assert entries[idx].byte_length == 0

    proj.apply_string_relocation(idx, b"HELLO EMPTY", "HELLO EMPTY")
    assert proj.hbc.strings()[idx] == "HELLO EMPTY"
    assert proj.is_patched is True


# --- apply_string_relocation: other invariants -------------------------


def test_apply_string_relocation_rejects_identifier():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "log")
    with pytest.raises(ValueError, match="Identifier"):
        proj.apply_string_relocation(idx, b"LOG", "LOG")
    assert proj.is_patched is False


def test_apply_string_relocation_rejects_out_of_range():
    proj = load_project("hello.hbc")
    with pytest.raises(ValueError):
        proj.apply_string_relocation(999, b"x", "x")
    assert proj.is_patched is False


def test_apply_string_relocation_rejects_wrong_new_bytes_for_expected():
    proj = load_project("hello.hbc")
    idx = _idx(proj, "hermit")
    with pytest.raises(ValueError, match="expected"):
        proj.apply_string_relocation(idx, b"XYZ", "not-what-was-encoded")
    assert proj.is_patched is False


# --- sequential ----------------------------------------------------------


def test_sequential_relocations_stack():
    proj = load_project("hello.hbc")
    i3 = _idx(proj, "hermit")
    i4 = _idx(proj, "use strict")

    proj.apply_string_relocation(i3, b"HERMIT-GROWN", "HERMIT-GROWN")
    proj.apply_string_relocation(i4, b"tiny", "tiny")

    assert proj.hbc.strings()[i3] == "HERMIT-GROWN"
    assert proj.hbc.strings()[i4] == "tiny"


# --- write_patched -------------------------------------------------


def test_write_patched_after_relocation(tmp_path):
    proj = load_project("hello.hbc")
    idx = _idx(proj, "hermit")
    orig = Path(proj.hbc_path).read_bytes()

    proj.apply_string_relocation(idx, b"HERMIT-EXTRA-LONG", "HERMIT-EXTRA-LONG")

    out = tmp_path / "hello_resize.hbc"
    n = proj.write_patched(str(out))
    data = out.read_bytes()

    assert n == len(data) == len(proj.image_bytes)
    assert len(data) > len(orig)   # storage grew
    assert hashlib.sha1(data[:-20]).digest() == data[-20:]

    chk = _libhbc.HBCFile.open(str(out))
    assert chk.strings()[idx] == "HERMIT-EXTRA-LONG"
    assert chk.header().file_length == len(data)


# --- _resolve_string_arg / REPL wiring --------------------------------


def test_resolve_string_arg_unaffected_by_resize():
    if not (CORPUS / "hello.hbc").exists():
        pytest.skip("missing corpus: hello.hbc")
    r = HermitREPL()
    r._open_file(str(CORPUS / "hello.hbc"))
    assert r._resolve_string_arg("string:3") == 3


# --- hermes oracle -----------------------------------------------


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_hermes_prints_a_longer_string(tmp_path):
    name = "greeting_g0.hbc"
    if not (CORPUS / name).exists():
        pytest.skip(f"missing corpus: {name}")
    proj = load_project(name)
    idx = _idx(proj, "Hello, world")

    r_orig = subprocess.run([HERMES, str(CORPUS / name)],
                            capture_output=True, text=True, timeout=30)
    assert "Hello, world" in r_orig.stdout
    assert r_orig.returncode == 0

    new_val = "Hello, world - patched with extra length"
    proj.apply_string_relocation(idx, new_val.encode("ascii"), new_val)

    out = tmp_path / "greeting_resize.hbc"
    proj.write_patched(str(out))
    reopened = _libhbc.HBCFile.open(str(out))
    assert reopened.strings()[idx] == new_val

    r_new = subprocess.run([HERMES, str(out)],
                           capture_output=True, text=True, timeout=30)
    assert r_new.returncode == 0
    assert new_val in r_new.stdout


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_hermes_detach_of_shared_entry(tmp_path):
    name = "hello.hbc"
    proj = load_project(name)
    idx = _idx(proj, "global")
    log_idx = _idx(proj, "log")

    _commit(proj, idx, BS * 6 + "planet")

    out = tmp_path / "hello_detach.hbc"
    proj.write_patched(str(out))
    reopened = _libhbc.HBCFile.open(str(out))
    assert reopened.strings()[idx] == "planet"
    assert reopened.strings()[log_idx] == "log"

    r = subprocess.run([HERMES, str(out)], capture_output=True, text=True, timeout=30)
    assert r.returncode in (0, 1)   # bare hello.hbc uses `console`, undefined -> rc 1 both original and patched
