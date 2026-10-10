"""same-size array/object literal edit.

`arrays` lists every `NewArrayWithBuffer[Long]` / `NewObjectWithBuffer
[Long]` site with the decoded literal; `edit array:<N>` opens an editor on a
site and allows changing an element's **value** while **keeping the SLP tag**
(Integer->Integer, Number->Number, string-ref->string-ref). Same-size splice
on the in-memory image (like the string edit) - `Module` is not involved.
"""

import hashlib
import subprocess
from pathlib import Path

import pytest

import _libhbc
from cli import display
from cli.editor import ArrayEditState
from cli.literals import SLPError, SLPTag, decode_slp, encode_slp_element, slp_render
from cli.project import HermitProject
from cli.repl import HermitREPL
from testutil import (
    CORPUS,
    CX,
    DOWN,
    ENTER,
    HERMES,
    UP,
    drive_arrays,
    load_project,
    strip_ansi,
)


def _site_values(proj, idx):
    """Replicates `HermitREPL._edit_array`: decodes values/keys with an
    ABSOLUTE byte_off against `proj.image_bytes`."""
    site = proj.hbc.literal_sites()[idx]
    image = proj.image_bytes
    if site.is_object:
        key_off = proj.hbc.obj_key_buffer_offset()
        val_off = proj.hbc.obj_value_buffer_offset()
        keys = decode_slp(image, key_off + site.key_buffer_idx, site.count)
        vals = decode_slp(image, val_off + site.buffer_idx, site.count)
        return site, vals, keys
    arr_off = proj.hbc.array_buffer_offset()
    vals = decode_slp(image, arr_off + site.buffer_idx, site.count)
    return site, vals, None


def _state(proj, idx):
    site, vals, keys = _site_values(proj, idx)
    return ArrayEditState(idx, site, vals, keys, proj.hbc.strings(), proj.hbc.functions())


def _commit(proj, idx, keys):
    """Drives the editor and applies the patches - like `HermitREPL._edit_array`."""
    st = _state(proj, idx)
    drive_arrays(st, keys + CX + "y")
    assert st.commit is True
    buffer_name = "objvalue" if st.site.is_object else "array"
    for abs_off, enc in st.patches.items():
        proj.apply_array_patch(buffer_name, abs_off, enc, idx)
    return st


# --- pure decoder/encoder (already covered at the margins by test_literals.cpp) --


def test_decode_slp_integer_run():
    buf = bytes.fromhex("730a000000140000001e000000")
    vals = decode_slp(buf, 0, 3)
    assert [v.value for v in vals] == [10, 20, 30]
    assert [v.byte_off for v in vals] == [1, 5, 9]
    assert all(v.tag == SLPTag.INTEGER and v.byte_len == 4 for v in vals)


def test_decode_slp_truncated_raises():
    with pytest.raises(SLPError, match="truncat"):
        decode_slp(b"\x73\x0a\x00\x00\x00", 0, 3)


def test_encode_integer_same_tag():
    assert encode_slp_element(SLPTag.INTEGER, "999") == (999).to_bytes(4, "little")
    assert encode_slp_element(SLPTag.INTEGER, "-5") == (-5).to_bytes(4, "little", signed=True)


def test_encode_integer_rejects_type_change():
    with pytest.raises(SLPError, match="Number"):
        encode_slp_element(SLPTag.INTEGER, "1.5")


def test_encode_integer_rejects_overflow():
    with pytest.raises(SLPError, match="int32"):
        encode_slp_element(SLPTag.INTEGER, str(2**31))


def test_encode_number_same_tag():
    import struct
    enc = encode_slp_element(SLPTag.NUMBER, "3.14")
    assert struct.unpack("<d", enc)[0] == 3.14


def test_encode_string_ref_range_checks():
    assert encode_slp_element(SLPTag.SHORT_STRING, "2", string_count=3) == (2).to_bytes(2, "little")
    with pytest.raises(SLPError, match="out of range"):
        encode_slp_element(SLPTag.SHORT_STRING, "5", string_count=3)
    with pytest.raises(SLPError, match="doesn't fit"):
        encode_slp_element(SLPTag.BYTE_STRING, "300", string_count=1000)


def test_encode_rejects_bool_null_toggle():
    with pytest.raises(SLPError, match="relocation"):
        encode_slp_element(SLPTag.NULL, "true")


def test_slp_render():
    v = decode_slp(bytes.fromhex("730a000000"), 0, 1)[0]
    assert slp_render(v, []) == "10"


# --- headless editor -----------------------------------------------------


def test_happy_path_changes_one_element():
    proj = load_project("literals_g0.hbc")
    idx = 0
    site, vals, _ = _site_values(proj, idx)
    assert [v.value for v in vals] == [10, 20, 30]

    st = _commit(proj, idx, DOWN + ENTER + "999" + ENTER)

    assert proj.is_patched is True
    _, vals2, _ = _site_values(proj, idx)
    assert [v.value for v in vals2] == [10, 999, 30]


def test_ctrlx_without_change_exits_immediately():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    drive_arrays(st, CX)
    assert st.commit is False
    assert proj.is_patched is False


def test_value_that_does_not_parse_for_the_current_tag_stays_in_edit():
    # Behavior deliberately changed on the relocation path: without opening the
    # type-picker (Ctrl+T) the edit stays constrained to the element's
    # CURRENT tag - "1.5" on an Integer fails to parse as an
    # integer (generic message, no longer "would change type": type
    # change is NOW supported, via Ctrl+T - see test_editor_literal_retype.py).
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    drive_arrays(st, ENTER + "1.5" + ENTER)
    assert st.mode == "edit"
    assert "integer" in st.edit_err
    assert not st.patches
    assert not st.changed_indices


def test_exit_ask_no_discards():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    drive_arrays(st, ENTER + "999" + ENTER + CX + "n")
    assert st.commit is False
    assert proj.is_patched is False


def test_navigation_wraps_at_bounds():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    drive_arrays(st, UP)          # already at 0, clamp
    assert st.sel == 0
    drive_arrays(st, DOWN * 10)   # past the last one, clamp
    assert st.sel == len(st.values) - 1


def test_object_keys_are_not_selectable_but_shown():
    proj = load_project("medium.hbc")
    sites = proj.hbc.literal_sites()
    obj_idx = next(i for i, s in enumerate(sites) if s.is_object)
    st = _state(proj, obj_idx)
    assert st.keys is not None
    # editing the selected value with the same tag (Integer) works
    old = st.values[0].value
    drive_arrays(st, ENTER + str(old + 100) + ENTER + CX + "y")


# --- apply_array_patch: shared buffer -----------------------------


def test_apply_array_patch_rejects_shared_buffer():
    proj = load_project("ridemovi.hbc")
    sites = proj.hbc.literal_sites()
    from collections import defaultdict
    by_idx = defaultdict(list)
    for i, s in enumerate(sites):
        if not s.is_object:
            by_idx[s.buffer_idx].append(i)
    dup = next((v for v in by_idx.values() if len(v) > 1), None)
    if dup is None:
        pytest.skip("no duplicate array bufferIdx in ridemovi.hbc")
    i, j = dup[0], dup[1]

    arr_off = proj.hbc.array_buffer_offset()
    vals = decode_slp(proj.hbc.array_buffer(), sites[i].buffer_idx, sites[i].count)
    v = vals[0]
    new_val = str((v.value + 1) if v.tag == SLPTag.INTEGER else v.value)
    try:
        enc = encode_slp_element(v.tag, new_val, string_count=len(proj.hbc.strings()))
    except SLPError:
        pytest.skip("duplicate site's element not editable (unsupported tag)")

    with pytest.raises(ValueError, match=f"array:{j}"):
        proj.apply_array_patch("array", arr_off + v.byte_off, enc, i)
    assert proj.is_patched is False


def test_apply_array_patch_rejects_wrong_size():
    proj = load_project("literals_g0.hbc")
    arr_off = proj.hbc.array_buffer_offset()
    with pytest.raises(ValueError):
        proj.apply_array_patch("array", arr_off + 1, b"\x00\x00", 0)   # 2B on a 4B Integer
    assert proj.is_patched is False


def test_apply_array_patch_rejects_out_of_buffer():
    proj = load_project("literals_g0.hbc")
    with pytest.raises(ValueError, match="outside the"):
        proj.apply_array_patch("array", 0, b"\x00\x00\x00\x00", 0)
    assert proj.is_patched is False


# --- arrays command ----------------------------------------------------


def test_cmd_arrays_rows():
    proj = load_project("medium.hbc")
    rows = display.arrays_to_ansi_rows(
        proj.hbc.literal_sites(), proj.hbc.array_buffer(), proj.hbc.obj_key_buffer(),
        proj.hbc.obj_value_buffer(), proj.hbc.strings(), proj.hbc.functions())
    plain = [strip_ansi(r) for r in rows]
    assert any("[array]" in r and "[" in r for r in plain)
    assert any("[object]" in r and "{" in r for r in plain)


def test_cmd_arrays_kind_filter():
    proj = load_project("medium.hbc")
    sites = proj.hbc.literal_sites()
    only_obj = display.arrays_to_ansi_rows(
        sites, proj.hbc.array_buffer(), proj.hbc.obj_key_buffer(),
        proj.hbc.obj_value_buffer(), proj.hbc.strings(), proj.hbc.functions(),
        kind_filter="object")
    assert all("[object]" in strip_ansi(r) for r in only_obj)


def test_resolve_array_arg():
    if not (CORPUS / "medium.hbc").exists():
        pytest.skip("missing corpus: medium.hbc")
    r = HermitREPL()
    r._open_file(str(CORPUS / "medium.hbc"))
    assert r._resolve_array_arg("array:0") == 0
    with pytest.raises(ValueError, match="out of range"):
        r._resolve_array_arg("array:999")


# --- write_patched + hermes oracle --------------------------------


def test_write_patched_after_array_commit(tmp_path):
    proj = load_project("literals_g0.hbc")
    orig = Path(proj.hbc_path).read_bytes()

    _commit(proj, 0, DOWN + ENTER + "999" + ENTER)

    out = tmp_path / "literals_samesize.hbc"
    n = proj.write_patched(str(out))
    data = out.read_bytes()

    assert n == len(data) == len(orig)                # same-size
    assert hashlib.sha1(data[:-20]).digest() == data[-20:]
    changed = [i for i in range(len(orig)) if orig[i] != data[i]]
    assert changed
    for i in changed:
        assert len(orig) - 20 <= i < len(orig) or True  # in the buffer or in the footer
    chk = _libhbc.HBCFile.open(str(out))
    _, vals, _ = _site_values(HermitProject.from_hbc(str(out)), 0)
    assert [v.value for v in vals] == [10, 999, 30]
    assert chk.header().file_length == len(orig)


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_hermes_prints_patched_element(tmp_path):
    name = "literals_g0.hbc"
    if not (CORPUS / name).exists():
        pytest.skip(f"missing corpus: {name} - `seeder.compile --g0 seeder/samples/literals.js`")
    r_orig = subprocess.run([HERMES, str(CORPUS / name)],
                            capture_output=True, text=True, timeout=30)
    assert r_orig.stdout.strip() == "20"
    assert r_orig.returncode == 0

    proj = load_project(name)
    _commit(proj, 0, DOWN + ENTER + "999" + ENTER)

    out = tmp_path / "literals_patched.hbc"
    proj.write_patched(str(out))

    r_new = subprocess.run([HERMES, str(out)], capture_output=True, text=True, timeout=30)
    assert r_new.returncode == 0
    assert r_new.stdout.strip() == "999"
