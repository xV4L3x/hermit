"""add/remove array/object literal element, END ONLY.

Extends the retype edit: `a` (nav mode) adds an element (array) or a key-value
pair (object) AT THE END; `d` always removes the LAST one - never an
arbitrary position. The key of a new property must reference an
EXISTING string:N (never a new key typed inline). A session is either
editing existing elements or a SINGLE add/remove, never both.
`Module::resize_literal` touches count/sizeHint/bufferIdx/keyBufferIdx - up
to two buffers for objects (so far always read-only: here it becomes
writable only to add/remove the last pair).
"""

import subprocess

import pytest

from cli.editor import ArrayEditState, _SLP_TAG_ORDER
from cli.literals import SLPTag, decode_slp, encode_slp_sequence, reencode_value_bytes
from cli.project import HermitProject
from cli.repl import HermitREPL
from testutil import CORPUS, CX, ENTER, HERMES, drive_arrays, load_project


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


def _state(proj, idx):
    site, vals, keys = _site_values(proj, idx)
    return ArrayEditState(idx, site, vals, keys, proj.hbc.strings(), proj.hbc.functions())


def _commit(proj, idx, keys):
    """Drives the editor and applies the commit - replicates the add/remove dispatch
    in `HermitREPL._edit_array` (added on top of the existing paths)."""
    st = _state(proj, idx)
    drive_arrays(st, keys + CX + "y")
    assert st.commit is True
    site = st.site
    if st.is_resize:
        new_val_buf = encode_slp_sequence(
            [(v.tag, reencode_value_bytes(v.tag, v.value)) for v in st.values])
        new_key_buf = encode_slp_sequence(
            [(k.tag, reencode_value_bytes(k.tag, k.value)) for k in st.keys]
        ) if site.is_object else b""
        proj.apply_array_resize(site.function_index, site.instruction_offset, site.is_object,
                                st.pending_add, new_val_buf, new_key_buf, idx)
    elif not st.needs_relocation:
        buffer_name = "objvalue" if site.is_object else "array"
        for abs_off, enc in st.patches.items():
            proj.apply_array_patch(buffer_name, abs_off, enc, idx)
    else:
        new_buffer_bytes = encode_slp_sequence(
            [(v.tag, reencode_value_bytes(v.tag, v.value)) for v in st.values])
        proj.apply_array_relocation(site.function_index, site.instruction_offset, site.is_object,
                                    new_buffer_bytes, idx, st.changed_indices)
    return st


# --- ArrayEditState: add/remove state machine ------------------------------


def test_begin_add_array_skips_key_stage():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.begin_add()
    assert st.mode == "add_type_pick"
    assert st.add_stage == "value"   # no keys for an array


def test_begin_add_object_starts_with_key_stage():
    proj = load_project("medium.hbc")
    idx = next(i for i, s in enumerate(proj.hbc.literal_sites()) if s.is_object)
    st = _state(proj, idx)
    st.begin_add()
    assert st.mode == "add_type_pick"
    assert st.add_stage == "key"


def test_add_type_pick_rejects_non_string_key_tag():
    proj = load_project("medium.hbc")
    idx = next(i for i, s in enumerate(proj.hbc.literal_sites()) if s.is_object)
    st = _state(proj, idx)
    st.begin_add()
    st.type_pick_sel = _SLP_TAG_ORDER.index(SLPTag.INTEGER)
    st.add_type_pick_confirm()
    assert st.mode == "add_type_pick"   # rejected, stays there
    assert "string" in st.edit_err


def test_full_add_flow_array():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.begin_add()
    st.type_pick_sel = _SLP_TAG_ORDER.index(SLPTag.INTEGER)
    st.add_type_pick_confirm()
    assert st.mode == "add_edit"
    st.buf = "555"
    st.commit_add_field()
    assert st.mode == "nav"
    assert st.pending_add is True
    assert [v.value for v in st.values] == [10, 20, 30, 555]
    assert st.is_resize is True
    assert st.dirty_count == 1


def test_full_add_flow_object_two_stages():
    proj = load_project("medium.hbc")
    idx = next(i for i, s in enumerate(proj.hbc.literal_sites()) if s.is_object)
    st = _state(proj, idx)
    st.begin_add()
    st.type_pick_sel = _SLP_TAG_ORDER.index(SLPTag.SHORT_STRING)
    st.add_type_pick_confirm()
    st.buf = "0"
    st.commit_add_field()
    assert st.mode == "add_type_pick"
    assert st.add_stage == "value"
    st.type_pick_sel = _SLP_TAG_ORDER.index(SLPTag.INTEGER)
    st.add_type_pick_confirm()
    st.buf = "42"
    st.commit_add_field()
    assert st.pending_add is True
    assert st.values[-1].value == 42
    assert st.keys[-1].value == 0


def test_request_remove_last_pops_preview():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.request_remove_last()
    assert st.pending_remove is True
    assert [v.value for v in st.values] == [10, 20]


def test_remove_on_empty_values_sets_error():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.values = []
    st.request_remove_last()
    assert st.pending_remove is False
    assert "no elements" in st.edit_err


def test_add_then_add_again_is_refused():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.begin_add()
    st.type_pick_sel = _SLP_TAG_ORDER.index(SLPTag.INTEGER)
    st.add_type_pick_confirm()
    st.buf = "1"
    st.commit_add_field()
    assert st.pending_add is True
    st.begin_add()
    assert st.mode == "nav"   # rejected, stays in nav (add_type_pick never reopened)
    assert st.edit_err


def test_edit_after_add_is_refused():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.begin_add()
    st.type_pick_sel = _SLP_TAG_ORDER.index(SLPTag.INTEGER)
    st.add_type_pick_confirm()
    st.buf = "1"
    st.commit_add_field()
    st.begin_edit()
    assert st.mode == "nav"   # rejected
    assert st.edit_err


def test_add_after_edit_is_refused():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.begin_edit()
    st.buf = "999"
    st.commit_element()
    assert st.changed_indices
    st.begin_add()
    assert st.mode == "nav"
    assert st.edit_err


def test_request_exit_asks_when_resize_pending():
    proj = load_project("literals_g0.hbc")
    st = _state(proj, 0)
    st.request_remove_last()
    events = []

    class _FakeApp:
        def invalidate(self):
            events.append("invalidate")
        def exit(self):
            events.append("exit")

    class _FakeEvent:
        app = _FakeApp()

    st.request_exit(_FakeEvent())
    assert st.mode == "exit_ask"
    assert events == ["invalidate"]


# --- end-to-end dispatch ----------------------------------------------------


def test_add_element_end_to_end_array():
    proj = load_project("literals_g0.hbc")
    site, vals, _ = _site_values(proj, 0)
    assert [v.value for v in vals] == [10, 20, 30]

    # 'a' -> add_type_pick (default Integer) -> Enter confirms type -> "999" -> Enter.
    st = _commit(proj, 0, "a" + ENTER + "999" + ENTER)

    assert proj.is_patched is True
    site2, vals2, _ = _site_values(proj, 0)
    assert [v.value for v in vals2] == [10, 20, 30, 999]
    assert site2.count == 4
    assert site2.buffer_idx > site.buffer_idx


def test_remove_last_end_to_end_array():
    proj = load_project("literals_g0.hbc")
    st = _commit(proj, 0, "d")

    assert proj.is_patched is True
    site2, vals2, _ = _site_values(proj, 0)
    assert [v.value for v in vals2] == [10, 20]
    assert site2.count == 2


def test_add_property_end_to_end_object():
    proj = load_project("medium.hbc")
    idx = next(i for i, s in enumerate(proj.hbc.literal_sites()) if s.is_object)
    _, vals0, keys0 = _site_values(proj, idx)

    # 'a' -> key stage (ShortString default) -> Enter -> "0" -> Enter ->
    # value stage (Integer default) -> Enter -> "42" -> Enter.
    st = _commit(proj, idx, "a" + ENTER + "0" + ENTER + ENTER + "42" + ENTER)

    assert proj.is_patched is True
    site2, vals2, keys2 = _site_values(proj, idx)
    assert [v.value for v in vals2] == [v.value for v in vals0] + [42]
    assert [k.value for k in keys2] == [k.value for k in keys0] + [0]
    assert site2.count == len(vals0) + 1


def test_remove_property_end_to_end_object():
    proj = load_project("medium.hbc")
    idx = next(i for i, s in enumerate(proj.hbc.literal_sites()) if s.is_object)
    _, vals0, keys0 = _site_values(proj, idx)

    st = _commit(proj, idx, "d")

    assert proj.is_patched is True
    site2, vals2, keys2 = _site_values(proj, idx)
    assert [v.value for v in vals2] == [v.value for v in vals0][:-1]
    assert [k.value for k in keys2] == [k.value for k in keys0][:-1]


# --- rejections ---------------------------------------------------------


def test_apply_array_resize_rejects_removing_when_empty():
    proj = load_project("literals_g0.hbc")
    for _ in range(3):   # [10,20,30] -> [10,20] -> [10] -> []
        site, vals, _ = _site_values(proj, 0)
        new_buf = encode_slp_sequence(
            [(v.tag, reencode_value_bytes(v.tag, v.value)) for v in vals[:-1]])
        proj.apply_array_resize(site.function_index, site.instruction_offset, False, False,
                                new_buf, b"", 0)
    site, vals, _ = _site_values(proj, 0)
    assert vals == []
    with pytest.raises(ValueError, match="has no elements to remove"):
        proj.apply_array_resize(site.function_index, site.instruction_offset, False, False,
                                b"", b"", 0)


def test_apply_array_resize_mismatched_site_rejected():
    proj = load_project("literals_g0.hbc")
    with pytest.raises(ValueError, match="does not match"):
        proj.apply_array_resize(999, 999, False, True, b"", b"", 0)


def test_edit_array_rejects_shared_function_body_for_resize(capsys):
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


# --- write_patched + hermes oracle ------------------------------------------


def test_write_patched_after_add(tmp_path):
    proj = load_project("literals_resize_g0.hbc")
    _commit(proj, 0, "a" + ENTER + "40" + ENTER)

    out = tmp_path / "literals_resize_add.hbc"
    proj.write_patched(str(out))
    chk = HermitProject.from_hbc(str(out))
    _, vals, _ = _site_values(chk, 0)
    assert [v.value for v in vals] == [10, 20, 30, 40]


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_hermes_prints_new_length_after_add(tmp_path):
    name = "literals_resize_g0.hbc"
    if not (CORPUS / name).exists():
        pytest.skip(f"missing corpus: {name} - "
                    "`seeder.compile --g0 seeder/samples/literals_resize.js`")
    r0 = subprocess.run([HERMES, str(CORPUS / name)], capture_output=True, text=True, timeout=30)
    assert r0.stdout.strip() == "3"

    proj = load_project(name)
    _commit(proj, 0, "a" + ENTER + "40" + ENTER)

    out = tmp_path / "literals_resize_add.hbc"
    proj.write_patched(str(out))
    r1 = subprocess.run([HERMES, str(out)], capture_output=True, text=True, timeout=30)
    assert r1.returncode == 0
    assert r1.stdout.strip() == "4"


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_hermes_prints_new_length_after_remove(tmp_path):
    name = "literals_resize_g0.hbc"
    if not (CORPUS / name).exists():
        pytest.skip(f"missing corpus: {name}")

    proj = load_project(name)
    _commit(proj, 0, "d")

    out = tmp_path / "literals_resize_remove.hbc"
    proj.write_patched(str(out))
    r1 = subprocess.run([HERMES, str(out)], capture_output=True, text=True, timeout=30)
    assert r1.returncode == 0
    assert r1.stdout.strip() == "2"
