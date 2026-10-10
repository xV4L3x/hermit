"""buffer commit (Ctrl+X -> Y) -> in-memory image patch.

No queue: on commit the modified lines are re-encoded
(`encode_committed_lines`) and applied IMMEDIATELY to the file image
(`HermitProject.apply_patch`), after an integrity check (re-parse via
`HBCFile.from_bytes` + comparison of instruction boundaries). The
`patch` command then writes the image to a new `.hbc`.
"""

import hashlib
import subprocess
from pathlib import Path

import pytest

import _libhbc
from cli.editor import EditorState, encode_committed_lines
from testutil import (
    CORPUS,
    CX,
    DOWN,
    ENTER,
    HERMES,
    RIGHT,
    drive_instructions,
    load_project,
)


def _editor(proj, func_idx):
    hbc = proj.hbc
    f = hbc.functions()[func_idx]
    return EditorState(
        f, hbc.disasm(func_idx),
        strings=hbc.strings(),
        opcodes=_libhbc.opcode_table(hbc.version()),
        version=hbc.version(),
    )


def _commit(proj, func_idx, keys):
    """Drives the editor, and if the user confirms applies the patch to the project.
    Reproduces exactly what `HermitREPL.cmd_edit` does."""
    st = _editor(proj, func_idx)
    drive_instructions(st, keys + CX + "y")
    assert st.commit is True
    patches = encode_committed_lines(st)
    proj.apply_patch(func_idx, patches)
    return st, patches


def _idx(proj, name):
    return next(f.index for f in proj.hbc.functions() if f.name == name)


def _find(hbc, mnemonic):
    for f in hbc.functions():
        for k, ins in enumerate(hbc.disasm(f.index)):
            if ins.mnemonic == mnemonic:
                return f.index, k, (ins.operands[0].u32 if ins.operands else None)
    return None


# --- HBCFile.from_bytes ---------------------------------------------


def test_from_bytes_matches_open():
    p = CORPUS / "hello.hbc"
    if not p.exists():
        pytest.skip("missing corpus: hello.hbc")
    raw = p.read_bytes()
    a = _libhbc.HBCFile.open(str(p))
    b = _libhbc.HBCFile.from_bytes(raw)
    assert b.version() == a.version()
    assert b.header().file_length == a.header().file_length
    assert len(b.functions()) == len(a.functions())
    assert b.strings() == a.strings()
    assert b.disasm(0)[0].mnemonic == a.disasm(0)[0].mnemonic


# --- commit: Ctrl+X -> Y/N -----------------------------------------


def test_ctrlx_yes_applies_patch_in_memory():
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")
    old_handle = proj.hbc

    _st, patches = _commit(proj, idx, RIGHT + ENTER + "r1" + ENTER)

    assert proj.is_patched is True
    assert proj.hbc is not old_handle
    # the new handle reflects the edit
    assert proj.hbc.disasm(idx)[0].operands[0].u32 == 1
    assert list(patches) == [proj.hbc.functions()[idx].offset + 0]


def test_ctrlx_no_discards():
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")
    st = _editor(proj, idx)
    drive_instructions(st, RIGHT + ENTER + "r1" + ENTER + CX + "n")
    assert st.commit is False
    assert proj.is_patched is False


def test_ctrlx_incomplete_line_blocks_exit():
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")
    st = _editor(proj, idx)
    l2 = st.lines[st.nav_indices[2]]
    assert l2.mnemonic == "JGreater"
    drive_instructions(st, DOWN + DOWN + ENTER + "JNotGreater" + ENTER + CX)   # swap, no refill
    assert st.mode == "nav"
    assert st.commit is False
    assert "incomplete" in st.edit_err.lower()


def test_clean_buffer_ctrlx_exits_without_prompt():
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")
    st = _editor(proj, idx)
    drive_instructions(st, CX)
    assert st.mode == "nav"
    assert st.commit is False


# --- encode_committed_lines: guards -------------------------------


def test_encode_requires_version():
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")
    hbc = proj.hbc
    st = EditorState(hbc.functions()[idx], hbc.disasm(idx),
                     strings=hbc.strings(),
                     opcodes=_libhbc.opcode_table(hbc.version()))  # no version
    st.lines[st.nav_indices[0]].dirty = True
    with pytest.raises(ValueError, match="version"):
        encode_committed_lines(st)


def test_encode_rejects_incomplete_line():
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")
    st = _editor(proj, idx)
    ln = st.lines[st.nav_indices[0]]
    ln.dirty = True
    ln.slots[0].value = None
    with pytest.raises(ValueError, match="incomplete"):
        encode_committed_lines(st)


def test_encode_rejects_switchimm():
    proj = load_project("switch_dense.hbc")   # dense cases compile to SwitchImm
    tgt = _find(proj.hbc, "SwitchImm")
    assert tgt is not None, "no SwitchImm in switch_dense.hbc"
    fidx, k, _reg = tgt
    st = _editor(proj, fidx)
    st.lines[st.nav_indices[k]].dirty = True
    with pytest.raises(ValueError, match="SwitchImm"):
        encode_committed_lines(st)


# --- apply_patch: integrity check ---------------------------------


def test_apply_patch_rejects_wrong_size_blob():
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")
    off = proj.hbc.functions()[idx].offset + 0
    with pytest.raises(ValueError):
        proj.apply_patch(idx, {off: b"\x00\x00\x00\x00\x00"})   # 5B on a 3B instruction
    # image intact
    assert proj.is_patched is False
    assert proj.image_bytes == Path(proj.hbc_path).read_bytes()


def test_apply_patch_rejects_out_of_range():
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")
    huge = len(Path(proj.hbc_path).read_bytes())
    with pytest.raises(ValueError, match="out of range"):
        proj.apply_patch(idx, {huge: b"\x00\x00"})
    assert proj.is_patched is False


# --- write_patched: same-size, footer, layout ----------------------


def test_write_patched_is_same_size_and_localised(tmp_path):
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")
    fi = proj.hbc.functions()[idx]
    orig = Path(proj.hbc_path).read_bytes()

    _commit(proj, idx, RIGHT + ENTER + "r1" + ENTER)

    out = tmp_path / "cf_patched.hbc"
    n = proj.write_patched(str(out))
    new = out.read_bytes()

    assert n == len(new) == len(orig)                # PATCHER-1: fileLength unchanged

    lo, hi = fi.offset, fi.offset + fi.bytecodeSizeInBytes
    changed = [i for i in range(len(orig)) if orig[i] != new[i]]
    assert changed
    for i in changed:
        assert (lo <= i < hi) or (len(orig) - 20 <= i < len(orig)), \
            f"byte {i:#x} modified outside the allowed region"

    chk = _libhbc.HBCFile.open(str(out))
    assert chk.header().file_length == len(orig)
    assert chk.disasm(idx)[0].operands[0].u32 == 1


def test_write_patched_recomputes_footer_sha1(tmp_path):
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")
    orig = Path(proj.hbc_path).read_bytes()

    _commit(proj, idx, RIGHT + ENTER + "r1" + ENTER)

    out = tmp_path / "cf_patched.hbc"
    proj.write_patched(str(out))
    data = out.read_bytes()

    assert hashlib.sha1(data[:-20]).digest() == data[-20:]   # PATCHER-2
    assert data[-20:] != orig[-20:]                          # footer changed


def test_write_patched_empty_copies_body(tmp_path):
    proj = load_project("control_flow.hbc")
    out = tmp_path / "copy.hbc"
    n = proj.write_patched(str(out))
    orig = Path(proj.hbc_path).read_bytes()
    assert n == len(orig)
    assert out.read_bytes()[:-20] == orig[:-20]


# --- multiple edits accumulate on the image ------------------------


def test_sequential_edits_stack_on_image():
    proj = load_project("control_flow.hbc")
    idx = _idx(proj, "max")

    _commit(proj, idx, RIGHT + ENTER + "r1" + ENTER)          # slot0: r0 -> r1
    _commit(proj, idx, RIGHT + RIGHT + ENTER + "2" + ENTER)   # slot1: 1  -> 2

    ins0 = proj.hbc.disasm(idx)[0]
    assert ins0.operands[0].u32 == 1
    assert ins0.operands[1].u32 == 2

    orig = Path(proj.hbc_path).read_bytes()
    img = proj.image_bytes
    assert len(img) == len(orig)
    assert sum(a != b for a, b in zip(orig[:-20], img[:-20])) == 2   # two body bytes


# --- opcode swap end-to-end + hermes oracle -------------------------


def test_opcode_swap_end_to_end(tmp_path):
    proj = load_project("hello.hbc")
    tgt = _find(proj.hbc, "LoadConstUndefined")
    if tgt is None:
        pytest.skip("no LoadConstUndefined in hello.hbc")
    fidx, k, reg0 = tgt

    _commit(proj, fidx,
            DOWN * k + ENTER + "LoadConstNull" + ENTER + ENTER + f"r{reg0}" + ENTER)

    assert proj.hbc.disasm(fidx)[k].mnemonic == "LoadConstNull"
    assert proj.hbc.disasm(fidx)[k].operands[0].u32 == reg0

    out = tmp_path / "hello_patched.hbc"
    proj.write_patched(str(out))
    assert len(out.read_bytes()) == len(Path(proj.hbc_path).read_bytes())


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_hermes_runs_patched_file(tmp_path):
    # control_flow.hbc runs clean under bare `hermes` (no `console`): rc=0.
    proj = load_project("control_flow.hbc")
    tgt = _find(proj.hbc, "LoadConstUndefined")
    if tgt is None:
        pytest.skip("no LoadConstUndefined in control_flow.hbc")
    fidx, k, reg0 = tgt

    src = Path(proj.hbc_path)
    r_orig = subprocess.run([HERMES, str(src)], capture_output=True, text=True, timeout=30)

    _commit(proj, fidx,
            DOWN * k + ENTER + "LoadConstNull" + ENTER + ENTER + f"r{reg0}" + ENTER)

    out = tmp_path / "cf_patched.hbc"
    proj.write_patched(str(out))

    r_new = subprocess.run([HERMES, str(out)], capture_output=True, text=True, timeout=30)
    assert r_new.returncode == r_orig.returncode, (
        f"hermes: patched rc={r_new.returncode} ≠ orig rc={r_orig.returncode}\n{r_new.stderr}"
    )
