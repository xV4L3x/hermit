"""free-type array/object literal edit + silent detach.

Extends the same-size edit: an element can change TAG (incl. toggling Null/True/False, not
just the value at equal tag) via a type-picker (Ctrl+T in edit mode);
a buffer shared with another site is no longer rejected, it always opens with
a banner and the commit always does a detach (append-only at the end of the
buffer + re-points ONLY that site's `bufferIdx`, via `Module.set_literal_value`).
A function with shared bytecode remains a pre-open rejection (the edit
would touch the bufferIdx operand of every function in the group together).
"""

import struct
import subprocess

import pytest

from cli.editor import ArrayEditState, _SLP_TAG_ORDER
from cli.literals import (
    SLPError, SLPTag, decode_slp, decode_value_bytes, encode_slp_sequence,
    encode_slp_value, rebuild_site_bytes, reencode_value_bytes,
)
from cli.project import HermitProject
from cli.repl import HermitREPL
from testutil import (
    CORPUS,
    CT,
    CX,
    DOWN,
    ENTER,
    HERMES,
    LEFT,
    RIGHT,
    drive_arrays,
    load_project,
)


def _site_values(proj, idx):
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


def _state(proj, idx, *, shared_with=()):
    site, vals, keys = _site_values(proj, idx)
    return ArrayEditState(idx, site, vals, keys, proj.hbc.strings(), proj.hbc.functions(),
                          is_shared=bool(shared_with), shared_with=shared_with)


def _commit(proj, idx, keys, *, shared_with=()):
    """Drives the editor and applies the commit - replicates the dispatch of
    `HermitREPL._edit_array` (same-tag fast-path or relocation)."""
    st = _state(proj, idx, shared_with=shared_with)
    drive_arrays(st, keys + CX + "y")
    assert st.commit is True
    buffer_name = "objvalue" if st.site.is_object else "array"
    if not st.needs_relocation:
        for abs_off, enc in st.patches.items():
            proj.apply_array_patch(buffer_name, abs_off, enc, idx)
    else:
        new_buffer_bytes = encode_slp_sequence(
            [(v.tag, reencode_value_bytes(v.tag, v.value)) for v in st.values])
        proj.apply_array_relocation(
            st.site.function_index, st.site.instruction_offset, st.site.is_object,
            new_buffer_bytes, idx, st.changed_indices)
    return st


# --- pure encoder: encode_slp_value / reencode_value_bytes / sequence -----


def test_encode_slp_value_zero_width_tags_ignore_raw():
    assert encode_slp_value(SLPTag.NULL, "qualunque cosa") == b""
    assert encode_slp_value(SLPTag.TRUE, "") == b""
    assert encode_slp_value(SLPTag.FALSE, "x") == b""


def test_encode_slp_value_integer_and_number():
    assert encode_slp_value(SLPTag.INTEGER, "42") == (42).to_bytes(4, "little", signed=True)
    assert encode_slp_value(SLPTag.NUMBER, "3.5") == struct.pack("<d", 3.5)


def test_encode_slp_value_integer_rejects_non_integer():
    with pytest.raises(SLPError, match="integer"):
        encode_slp_value(SLPTag.INTEGER, "1.5")


def test_encode_slp_value_integer_rejects_overflow():
    with pytest.raises(SLPError, match="range"):
        encode_slp_value(SLPTag.INTEGER, str(2**31))


def test_encode_slp_value_string_ref_range_checks():
    with pytest.raises(SLPError, match="out of range"):
        encode_slp_value(SLPTag.SHORT_STRING, "5", string_count=3)
    assert encode_slp_value(SLPTag.SHORT_STRING, "2", string_count=3) == (2).to_bytes(2, "little")


def test_encode_slp_value_string_ref_too_wide_for_tag():
    with pytest.raises(SLPError, match="doesn't fit"):
        encode_slp_value(SLPTag.BYTE_STRING, "300", string_count=1000)


def test_reencode_value_bytes_is_exact_inverse_of_decode():
    enc = encode_slp_value(SLPTag.NUMBER, "3.14159", string_count=0)
    value = decode_value_bytes(SLPTag.NUMBER, enc)
    assert reencode_value_bytes(SLPTag.NUMBER, value) == enc

    enc2 = encode_slp_value(SLPTag.INTEGER, "-17", string_count=0)
    value2 = decode_value_bytes(SLPTag.INTEGER, enc2)
    assert reencode_value_bytes(SLPTag.INTEGER, value2) == enc2

    assert reencode_value_bytes(SLPTag.NULL, None) == b""


def test_encode_slp_sequence_groups_consecutive_equal_tags():
    items = [(SLPTag.INTEGER, (1).to_bytes(4, "little")),
             (SLPTag.INTEGER, (2).to_bytes(4, "little")),
             (SLPTag.NULL, b""),
             (SLPTag.INTEGER, (3).to_bytes(4, "little"))]
    out = encode_slp_sequence(items)
    # Integer run(2) -> tag_byte 0x72, Null run(1) -> 0x01, Integer run(1) -> 0x71
    assert out == bytes([0x72]) + (1).to_bytes(4, "little") + (2).to_bytes(4, "little") \
                 + bytes([0x01]) \
                 + bytes([0x71]) + (3).to_bytes(4, "little")


def test_encode_slp_sequence_round_trips_through_decode_slp():
    items = [(SLPTag.INTEGER, (10).to_bytes(4, "little")),
             (SLPTag.NUMBER, struct.pack("<d", 2.5)),
             (SLPTag.SHORT_STRING, (7).to_bytes(2, "little"))]
    data = encode_slp_sequence(items)
    decoded = decode_slp(data, 0, 3)
    assert decoded[0].tag == SLPTag.INTEGER and decoded[0].value == 10
    assert decoded[1].tag == SLPTag.NUMBER and decoded[1].value == 2.5
    assert decoded[2].tag == SLPTag.SHORT_STRING and decoded[2].value == 7


def test_rebuild_site_bytes_matches_hand_computed_bytes():
    proj = load_project("literals_g0.hbc")
    site, vals, _ = _site_values(proj, 0)
    assert [v.value for v in vals] == [10, 20, 30]

    out = rebuild_site_bytes(vals, 1, SLPTag.NULL, b"")
    # same payload used in tests/unit/patcher/test_module.cpp for
    # "set_literal_value grows the array buffer ..." - cross-check C++/Python.
    expected = bytes([0x71]) + (10).to_bytes(4, "little") + bytes([0x01]) \
             + bytes([0x71]) + (30).to_bytes(4, "little")
    assert out == expected


# --- ArrayEditState: type-picker + needs_relocation ------------------------


def test_type_pick_sets_edit_tag_and_returns_to_edit():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.sel = 1
    st.begin_edit()
    st.open_type_pick()
    assert st.mode == "type_pick"
    assert st.type_pick_sel == _SLP_TAG_ORDER.index(SLPTag.INTEGER)
    st.type_pick_move(-4)   # Integer(4) -> Null(0) in _SLP_TAG_ORDER
    assert st.type_pick_sel == 0
    st.type_pick_confirm()
    assert st.mode == "edit"
    assert st.edit_tag == SLPTag.NULL
    assert st.buf == ""


def test_type_pick_cancel_keeps_original_tag():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.begin_edit()
    st.open_type_pick()
    st.type_pick_move(+1)
    st.type_pick_cancel()
    assert st.mode == "edit"
    assert st.edit_tag is None   # no tag chosen: keeps the original one


def test_needs_relocation_false_for_same_tag_same_width_edit():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.begin_edit()
    st.buf = "999"
    st.commit_element()
    assert st.changed_indices == {0}
    assert st.needs_relocation is False


def test_needs_relocation_true_after_tag_change():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.sel = 1
    st.begin_edit()
    st.open_type_pick()
    st.type_pick_sel = _SLP_TAG_ORDER.index(SLPTag.NULL)
    st.type_pick_confirm()
    st.commit_element()
    assert st.values[1].tag == SLPTag.NULL
    assert st.needs_relocation is True


def test_needs_relocation_true_when_shared_even_at_same_tag():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0, shared_with=[7])   # synthetic, only for the property
    st.begin_edit()
    st.buf = "999"
    st.commit_element()
    assert st.needs_relocation is True


def test_orig_values_never_mutated_by_commit_element():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.begin_edit()
    st.buf = "999"
    st.commit_element()
    assert st.orig_values[0].value == 10
    assert st.values[0].value == 999


# --- end-to-end dispatch on literals_g0.hbc --------------------------------


def test_toggle_integer_to_null_uses_relocation():
    proj = load_project("literals_g0.hbc")
    site, vals, _ = _site_values(proj, 0)
    assert [v.value for v in vals] == [10, 20, 30]

    # select element 1, open edit, Ctrl+T, 4x left (Integer -> Null),
    # Enter (confirms type), Enter (empty buffer, valid for Null), Ctrl+X, y.
    st = _commit(proj, 0, DOWN + ENTER + CT + LEFT * 4 + ENTER + ENTER)

    assert proj.is_patched is True
    _, vals2, _ = _site_values(proj, 0)
    assert [v.tag for v in vals2] == [SLPTag.INTEGER, SLPTag.NULL, SLPTag.INTEGER]
    assert [v.value for v in vals2] == [10, None, 30]
    # append-only: the site's bufferIdx moved to the end, count unchanged.
    site2 = proj.hbc.literal_sites()[0]
    assert site2.count == 3
    assert site2.buffer_idx > site.buffer_idx


def test_change_integer_to_string_ref_uses_relocation():
    proj = load_project("literals_g0.hbc")
    n_strings = len(proj.hbc.strings())
    if n_strings == 0:
        pytest.skip("no strings in the test file")

    # element 0: Integer -> ShortString(index 0). Ctrl+T from Integer(4) to
    # ShortString(5) = 1 step right in the picker.
    RIGHT = "\x1b[C"
    st = _commit(proj, 0, ENTER + CT + RIGHT + ENTER + "0" + ENTER)

    assert proj.is_patched is True
    _, vals2, _ = _site_values(proj, 0)
    assert vals2[0].tag == SLPTag.SHORT_STRING
    assert vals2[0].value == 0
    assert [v.value for v in vals2[1:]] == [20, 30]


def test_relocation_rejects_when_multiple_elements_and_only_one_relocates():
    # Edit of TWO elements in the same session (one changes tag, the other
    # stays the same tag): both end up in the relocation (the regenerated
    # payload reflects the CURRENT state of state.values as a whole).
    proj = load_project("literals_g0.hbc")
    st = _commit(proj, 0,
                DOWN + ENTER + "777" + ENTER +                 # elem 1: 20 -> 777 (same tag)
                DOWN + ENTER + CT + LEFT * 4 + ENTER + ENTER)   # elem 2: 30 -> Null
    assert proj.is_patched is True
    _, vals2, _ = _site_values(proj, 0)
    assert [v.value for v in vals2] == [10, 777, None]
    assert [v.tag for v in vals2] == [SLPTag.INTEGER, SLPTag.INTEGER, SLPTag.NULL]


# --- shared buffer: always opens, silent detach ---------------------


def test_shared_buffer_site_opens_and_detaches_on_real_bundle():
    proj = load_project("ridemovi.hbc")
    sites = proj.hbc.literal_sites()
    funcs = proj.hbc.functions()   # cache: functions() re-copies the whole vector on every call
    from collections import defaultdict
    # Long variant required to avoid hitting B5 (Short bufferIdx already
    # beyond 64 KB on ridemovi.hbc - see test_apply_array_patch_rejects_
    # shared_buffer in test_editor_literal_samesize.py, which instead stays on the
    # Short variant because the same-tag path never touches bufferIdx).
    by_idx = defaultdict(list)
    for i, s in enumerate(sites):
        if s.mnemonic == "NewArrayWithBufferLong" and not funcs[s.function_index].shared:
            by_idx[s.buffer_idx].append(i)
    dup = next((v for v in by_idx.values() if len(v) > 1), None)
    if dup is None:
        pytest.skip("no duplicate array bufferIdx (Long variant) in ridemovi.hbc")
    i, j = dup[0], dup[1]

    _, vals_i_before, _ = _site_values(proj, i)
    v0 = vals_i_before[0]
    new_val = str((v0.value + 1) if v0.tag == SLPTag.INTEGER else v0.value)
    try:
        encode_slp_value(v0.tag, new_val, string_count=len(proj.hbc.strings()))
    except SLPError:
        pytest.skip(f"element 0 of site {i} not editable with this encoder (tag {v0.tag})")

    _, vals_j_before, _ = _site_values(proj, j)

    st = _commit(proj, i, ENTER + new_val + ENTER, shared_with=[j])
    assert st.needs_relocation is True   # forced by is_shared even at unchanged tag/width
    assert proj.is_patched is True

    _, vals_i_after, _ = _site_values(proj, i)
    assert vals_i_after[0].value == decode_value_bytes(
        v0.tag, encode_slp_value(v0.tag, new_val, string_count=len(proj.hbc.strings())))
    # site j (which shared the buffer BEFORE the edit) was never
    # touched: its old bytes remain intact.
    _, vals_j_after, _ = _site_values(proj, j)
    assert [v.value for v in vals_j_after] == [v.value for v in vals_j_before]


# --- pre-open rejection: function with shared bytecode --------


def test_edit_array_rejects_shared_function_body(capsys):
    if not (CORPUS / "ridemovi.hbc").exists():
        pytest.skip("missing corpus: ridemovi.hbc")
    r = HermitREPL()
    r._open_file(str(CORPUS / "ridemovi.hbc"))

    sites = r.project.hbc.literal_sites()
    funcs = r.project.hbc.functions()
    shared_idx = next((i for i, s in enumerate(sites) if funcs[s.function_index].shared), None)
    if shared_idx is None:
        pytest.skip("no site on a shared-bytecode function in ridemovi.hbc")

    r._edit_array(r.project, f"array:{shared_idx}")
    out = capsys.readouterr().out
    assert "private clone" in out
    assert r.project.is_patched is False


# --- write_patched + hermes oracle -----------------------------------------


def test_write_patched_after_relocation(tmp_path):
    proj = load_project("literals_g0.hbc")
    _commit(proj, 0, DOWN + ENTER + CT + LEFT * 4 + ENTER + ENTER)

    out = tmp_path / "literals_retype.hbc"
    proj.write_patched(str(out))

    chk = HermitProject.from_hbc(str(out))
    _, vals, _ = _site_values(chk, 0)
    assert [v.value for v in vals] == [10, None, 30]


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_hermes_prints_null_after_toggle(tmp_path):
    name = "literals_g0.hbc"
    if not (CORPUS / name).exists():
        pytest.skip(f"missing corpus: {name}")
    r_orig = subprocess.run([HERMES, str(CORPUS / name)], capture_output=True, text=True, timeout=30)
    assert r_orig.stdout.strip() == "20"

    proj = load_project(name)
    _commit(proj, 0, DOWN + ENTER + CT + LEFT * 4 + ENTER + ENTER)

    out = tmp_path / "literals_null.hbc"
    proj.write_patched(str(out))

    r_new = subprocess.run([HERMES, str(out)], capture_output=True, text=True, timeout=30)
    assert r_new.returncode == 0
    assert r_new.stdout.strip() == "null"
