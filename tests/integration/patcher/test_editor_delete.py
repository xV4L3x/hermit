"""delete instruction (`Ctrl+K` in the editor).

Reuses the insert structural machinery (`Module`): the body shrinks, the file
is rebuilt. The checks (deleted jump target / EH boundary) fire
at commit time, not in the editor.
"""


import pytest

import _libhbc
from cli.editor import EditorState, assemble_committed_function
from testutil import (
    CK,
    CX,
    CZ,
    DOWN,
    ENTER,
    HERMES,
    RIGHT,
    drive_instructions,
    load_project,
    run_hermes,
)


def _editor(proj, name):
    idx = next(f.index for f in proj.hbc.functions() if f.name == name)
    f = proj.hbc.functions()[idx]
    st = EditorState(f, proj.hbc.disasm(idx),
                     strings=proj.hbc.strings(),
                     opcodes=_libhbc.opcode_table(proj.hbc.version()),
                     version=proj.hbc.version())
    return st, idx


def _commit(proj, state):
    fidx, asm, frame, warn, _edall = assemble_committed_function(state)
    warn = list(warn) + list(proj.apply_insertion(fidx, asm, frame))
    return warn


# --- basic deletion ------------------------------------------------


def test_ctrlk_deletes_current_line():
    proj = load_project("loop_sum_g0.hbc")
    st, _ = _editor(proj, "loopSum")
    n0 = len(st.nav_indices)
    drive_instructions(st, DOWN * 2 + CK)
    assert len(st.nav_indices) == n0 - 1
    assert st.structural_change is True


def test_undo_restores_deleted_line():
    proj = load_project("loop_sum_g0.hbc")
    st, _ = _editor(proj, "loopSum")
    n0 = len(st.nav_indices)
    drive_instructions(st, DOWN * 3 + CK + CZ)
    assert len(st.nav_indices) == n0


def test_cannot_empty_function():
    proj = load_project("max_g0.hbc")
    st, _ = _editor(proj, "max")
    n0 = len(st.nav_indices)
    drive_instructions(st, CK * n0)          # try to delete them all
    assert len(st.nav_indices) == 1
    assert "last instruction" in st.edit_err


def test_ctrlx_after_delete_only_asks_to_save():
    # regression: a deletion doesn't count in dirty_count (the line is gone),
    # but Ctrl+X must still ask Y/N and not discard automatically.
    proj = load_project("max_g0.hbc")
    st, _ = _editor(proj, "max")
    drive_instructions(st, DOWN * 3 + CK + CX)          # delete line 3 (Ret 1), then Ctrl+X
    assert st.mode == "exit_ask"
    assert st.commit is False
    st, _ = _editor(proj, "max")
    drive_instructions(st, DOWN * 3 + CK + CX + "y")
    assert st.commit is True and st.structural_change is True


# --- round-trip insert + delete -------------------------------


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_insert_then_delete_roundtrips(tmp_path):
    proj = load_project("loop_sum_g0.hbc")
    idx = next(f.index for f in proj.hbc.functions() if f.name == "loopSum")
    src_body = proj.image_bytes  # = original (no patch yet)
    fi0 = proj.hbc.functions()[idx]
    orig_body = src_body[fi0.offset:fi0.offset + fi0.bytecodeSizeInBytes]
    r0 = run_hermes(proj.hbc_path)

    # session 1: insert Mov r0,r0 at line 2
    st, _ = _editor(proj, "loopSum")
    drive_instructions(st, DOWN * 2 + "o" + "Mov" + ENTER + ENTER + "r0" + ENTER + RIGHT + ENTER + "r0" + ENTER + CX + "y")
    _commit(proj, st)
    assert len(proj.hbc.disasm(idx)) == len(_libhbc.HBCFile.open(proj.hbc_path).disasm(idx)) + 1

    # session 2: fresh editor on the patched image, delete the Mov r0,r0
    st, _ = _editor(proj, "loopSum")
    d = proj.hbc.disasm(idx)
    mov_row = next(k for k, x in enumerate(d)
                   if x.mnemonic == "Mov" and x.operands[0].u32 == 0 and x.operands[1].u32 == 0)
    drive_instructions(st, DOWN * mov_row + CK + CX + "y")
    _commit(proj, st)

    fi = proj.hbc.functions()[idx]
    new_body = proj.image_bytes[fi.offset:fi.offset + fi.bytecodeSizeInBytes]
    assert new_body == orig_body            # body restored byte-identical

    out = tmp_path / "rt.hbc"
    proj.write_patched(str(out))
    r1 = run_hermes(out)
    assert r1.returncode == r0.returncode
    assert r1.stdout == r0.stdout


# --- commit-time guards ---------------------------------------


def test_delete_jump_target_is_refused():
    proj = load_project("loop_sum_g0.hbc")
    st, idx = _editor(proj, "loopSum")
    d = proj.hbc.disasm(idx)
    # last line = Ret, target of the JmpFalse
    drive_instructions(st, DOWN * (len(d) - 1) + CK)
    with pytest.raises(ValueError, match="jump target"):
        assemble_committed_function(st)


def test_delete_eh_boundary_is_refused():
    proj = load_project("try_catch_g0.hbc")
    st, idx = _editor(proj, "risky")
    d = proj.hbc.disasm(idx)
    ehs = proj.hbc.functions()[idx].exceptionHandlers
    bound = ehs[0].start
    row = next(k for k, x in enumerate(d) if x.offset == bound)
    drive_instructions(st, DOWN * row + CK)
    with pytest.raises(ValueError, match="exception handler"):
        assemble_committed_function(st)


def test_delete_inside_try_range_is_allowed():
    proj = load_project("try_catch_g0.hbc")
    st, idx = _editor(proj, "risky")
    d = proj.hbc.disasm(idx)
    e0 = proj.hbc.functions()[idx].exceptionHandlers[0]
    # an instruction inside (start, end), NOT a boundary and NOT a jump target
    inside = next(k for k, x in enumerate(d)
                  if e0.start < x.offset < e0.end
                  and x.offset not in (e0.start, e0.end, e0.target)
                  and not x.mnemonic.startswith("J"))
    drive_instructions(st, DOWN * inside + CK)
    # must not raise: the try-range shrinks
    fidx, asm, frame, warn, _edall = assemble_committed_function(st)
    assert len(asm) == len(d) - 1
