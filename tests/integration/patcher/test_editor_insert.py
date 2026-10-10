"""instruction insertion + structural serializer (`Module`).

`o`/`O` in the editor insert a new instruction; the commit (`Ctrl+X -> Y`)
reassembles the function body via `Module` (jump relocation, branch
relaxation Addr8->Addr32, EH relocation, layout + footer recomputation) and
re-parses its image.
"""


import pytest

import _libhbc
from cli.editor import EditorState, assemble_committed_function
from testutil import (
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


_MOV = next(d.opcode for d in _libhbc.opcode_table(96) if d.mnemonic == "Mov")


def _editor(proj, func_idx):
    f = proj.hbc.functions()[func_idx]
    return EditorState(
        f, proj.hbc.disasm(func_idx),
        strings=proj.hbc.strings(),
        opcodes=_libhbc.opcode_table(proj.hbc.version()),
        version=proj.hbc.version(),
    )


def _idx(proj, name):
    return next(f.index for f in proj.hbc.functions() if f.name == name)


def _commit_insertion(proj, func_idx, keys):
    """Drives the editor, then replicates `cmd_edit`: assemble + apply_insertion."""
    st = _editor(proj, func_idx)
    drive_instructions(st, keys + CX + "y")
    assert st.commit is True
    assert st.structural_change is True
    fidx, asm, frame, warnings, _edall = assemble_committed_function(st)
    warnings = list(warnings) + list(proj.apply_insertion(fidx, asm, frame))
    return st, warnings


# --- no-op insertion ------------------------------------------------


def test_insert_noop_grows_function_and_reparses():
    proj = load_project("loop_sum_g0.hbc")
    idx = _idx(proj, "loopSum")
    n0 = len(proj.hbc.disasm(idx))
    sz0 = proj.hbc.functions()[idx].bytecodeSizeInBytes

    # line 2 -> insert below a Mov r0,r0
    st, w = _commit_insertion(
        proj, idx, DOWN * 2 + "o" + "Mov" + ENTER + ENTER + "r0" + ENTER + RIGHT + ENTER + "r0" + ENTER)

    assert w == []
    ins = proj.hbc.disasm(idx)
    assert len(ins) == n0 + 1
    assert proj.hbc.functions()[idx].bytecodeSizeInBytes == sz0 + 3   # Mov = 1+Reg8+Reg8
    assert proj.is_patched is True
    assert any(x.mnemonic == "Mov" and x.operands[0].u32 == 0 for x in ins)


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_insert_noop_keeps_behaviour(tmp_path):
    proj = load_project("loop_sum_g0.hbc")
    idx = _idx(proj, "loopSum")
    r0 = run_hermes(proj.hbc_path)

    _commit_insertion(proj, idx, DOWN * 2 + "o" + "Mov" + ENTER + ENTER + "r0" + ENTER + RIGHT + ENTER + "r0" + ENTER)

    out = tmp_path / "loop_sum_patched.hbc"
    proj.write_patched(str(out))
    r1 = run_hermes(out)
    assert r1.returncode == r0.returncode
    assert r1.stdout == r0.stdout, f"output changed: {r0.stdout!r} -> {r1.stdout!r}"


# --- insertion before a backward jump --------------------------------


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_insert_before_backward_jump_preserves_target(tmp_path):
    proj = load_project("loop_sum_g0.hbc")
    idx = _idx(proj, "loopSum")
    ins = proj.hbc.disasm(idx)
    # find the backward jump (negative displacement)
    jpos = next(k for k, x in enumerate(ins)
                if x.operands and x.mnemonic.startswith("J") and x.operands[0].i32 < 0)
    r0 = run_hermes(proj.hbc_path)

    # navigate to jpos, insert ABOVE a Mov r0,r0
    st, _ = _commit_insertion(
        proj, idx, DOWN * jpos + "O" + "Mov" + ENTER + ENTER + "r0" + ENTER + RIGHT + ENTER + "r0" + ENTER)

    out = tmp_path / "lb.hbc"
    proj.write_patched(str(out))
    r1 = run_hermes(out)
    assert r1.returncode == 0
    assert r1.stdout == r0.stdout          # the loop still computes correctly


# --- exception handler relocation --------------------------------


def test_insert_into_eh_function_relocates_handlers():
    proj = load_project("try_catch_g0.hbc")
    idx = _idx(proj, "risky")
    eh0 = [(e.start, e.end, e.target) for e in proj.hbc.functions()[idx].exceptionHandlers]
    assert eh0

    # insert Mov r0,r0 at line 1 (after the first LoadParam, before the try)
    st, _ = _commit_insertion(
        proj, idx, DOWN + "O" + "Mov" + ENTER + ENTER + "r0" + ENTER + RIGHT + ENTER + "r0" + ENTER)

    eh1 = [(e.start, e.end, e.target) for e in proj.hbc.functions()[idx].exceptionHandlers]
    assert len(eh1) == len(eh0)
    for (s0, e0, t0), (s1, e1, t1) in zip(eh0, eh1):
        assert (s1, e1, t1) == (s0 + 3, e0 + 3, t0 + 3)


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_insert_into_eh_function_keeps_behaviour(tmp_path):
    proj = load_project("try_catch_g0.hbc")
    idx = _idx(proj, "risky")
    r0 = run_hermes(proj.hbc_path)
    _commit_insertion(
        proj, idx, DOWN + "O" + "Mov" + ENTER + ENTER + "r0" + ENTER + RIGHT + ENTER + "r0" + ENTER)
    out = tmp_path / "tc.hbc"
    proj.write_patched(str(out))
    r1 = run_hermes(out)
    assert r1.returncode == r0.returncode
    assert r1.stdout == r0.stdout


# --- inserted jump with target-pick -----------------------------


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_insert_jump_with_target_pick(tmp_path):
    proj = load_project("loop_sum_g0.hbc")
    idx = _idx(proj, "loopSum")
    # O at the start, choose Jmp, Enter on the Addr -> insert_target, DOWN*3, Enter
    st, _ = _commit_insertion(
        proj, idx, "O" + "Jmp" + ENTER + ENTER + DOWN * 3 + ENTER)
    ins = proj.hbc.disasm(idx)
    assert ins[0].mnemonic == "Jmp"
    # the file must be valid and loadable
    out = tmp_path / "j.hbc"
    proj.write_patched(str(out))
    assert run_hermes(out).returncode == 0


# --- undo -------------------------------------------------------


def test_undo_removes_inserted_line():
    proj = load_project("loop_sum_g0.hbc")
    idx = _idx(proj, "loopSum")
    st = _editor(proj, idx)
    n0 = len(st.nav_indices)
    CZ = "\x1a"
    drive_instructions(st, DOWN * 2 + "o" + "Mov" + ENTER + ENTER + "r0" + ENTER + RIGHT + ENTER + "r0" + ENTER + CZ + CZ)
    # after 2 undos (edit slot1, edit slot0)... the insertion is one more snapshot
    drive_instructions(st, CZ)
    assert len(st.nav_indices) == n0
    assert not any(st.lines[i].inserted for i in st.nav_indices)


# --- rejections --------------------------------------------------


def test_assemble_rejects_incomplete_inserted_line():
    proj = load_project("loop_sum_g0.hbc")
    idx = _idx(proj, "loopSum")
    st = _editor(proj, idx)
    drive_instructions(st, DOWN * 2 + "o" + "Mov" + ENTER)   # inserted but slots not filled in
    assert st.structural_change is True
    with pytest.raises(ValueError, match="incomplete|target"):
        assemble_committed_function(st)


def test_assemble_rejects_framesize_bump_on_eh_function():
    proj = load_project("try_catch_g0.hbc")
    idx = _idx(proj, "risky")
    f = proj.hbc.functions()[idx]
    st = _editor(proj, idx)
    fresh = f.frameSize
    # insert Mov r{frameSize}, r0 -> would require a new register
    drive_instructions(st, "O" + "Mov" + ENTER + ENTER + f"r{fresh}" + ENTER + RIGHT + ENTER + "r0" + ENTER)
    with pytest.raises(ValueError, match="exception handler"):
        assemble_committed_function(st)
