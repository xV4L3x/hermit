"""overflowed functions + real bundles (non-monotonic layout,
shared bytecode).

`Module` now handles `ridemovi.hbc` (37k functions, 11 overflow, shared bodies).
The editor shows a banner when the bytecode is shared and asks for confirmation
("apply to all?") before saving.

`ridemovi.hbc` and `overflow_func.hbc` are gitignored -> skip if absent.
"""


import pytest

import _libhbc
from cli.editor import EditorState, assemble_committed_function
from cli.project import HermitProject
from testutil import CORPUS, CX, DOWN, ENTER, RIGHT, drive_instructions


def _open(name):
    p = CORPUS / name
    if not p.exists():
        pytest.skip(f"missing corpus: {name} (gitignored)")
    return HermitProject.from_hbc(str(p))


def _editor(proj, func_idx):
    f = proj.hbc.functions()[func_idx]
    return EditorState(f, proj.hbc.disasm(func_idx),
                       strings=proj.hbc.strings(),
                       opcodes=_libhbc.opcode_table(proj.hbc.version()),
                       version=proj.hbc.version())


def _first_reg_operand_edit(ins):
    """(row, tok, new_value) for a same-size edit of a Reg8 operand."""
    T = _libhbc.OperandType
    for k, x in enumerate(ins):
        for j, o in enumerate(x.operands):
            if o.type == T.Reg8:
                nv = 0 if o.u32 != 0 else 1
                return k, j + 1, nv
    return None


def _shared_plain_func(funcs):
    """First function with shared bytecode, non-overflow, no EH."""
    return next((f for f in funcs
                 if f.shared and not f.overflowed and not f.hasExceptionHandler), None)


# --- shared bytecode: banner + prompt --------------------------


def test_shared_function_state_flags():
    proj = _open("ridemovi.hbc")
    sf = _shared_plain_func(proj.hbc.functions())
    if sf is None:
        pytest.skip("no non-overflow/non-EH shared function")
    st = _editor(proj, sf.index)
    assert st.shared is True
    assert st.share_count >= 2
    assert st.edit_all is False


def test_shared_ctrlx_enters_share_ask_not_exit_ask():
    proj = _open("ridemovi.hbc")
    sf = _shared_plain_func(proj.hbc.functions())
    if sf is None:
        pytest.skip("no shared function")
    ed = _first_reg_operand_edit(proj.hbc.disasm(sf.index))
    if ed is None:
        pytest.skip("no Reg8 operand to modify")
    row, tok, nv = ed

    st = _editor(proj, sf.index)
    drive_instructions(st, DOWN * row + RIGHT * tok + ENTER + f"r{nv}" + ENTER + CX)
    assert st.mode == "share_ask"
    assert st.commit is False


def test_shared_prompt_no_aborts_commit():
    proj = _open("ridemovi.hbc")
    sf = _shared_plain_func(proj.hbc.functions())
    if sf is None:
        pytest.skip("no shared function")
    ed = _first_reg_operand_edit(proj.hbc.disasm(sf.index))
    if ed is None:
        pytest.skip("no Reg8 operand")
    row, tok, nv = ed

    st = _editor(proj, sf.index)
    drive_instructions(st, DOWN * row + RIGHT * tok + ENTER + f"r{nv}" + ENTER + CX + "n")
    assert st.edit_all is False
    assert st.mode == "nav"
    assert "shared" in st.edit_err


def test_shared_edit_all_reparses_and_updates_whole_group():
    proj = _open("ridemovi.hbc")
    funcs = proj.hbc.functions()
    sf = _shared_plain_func(funcs)
    if sf is None:
        pytest.skip("no shared function")
    group = [f.index for f in funcs if f.offset == sf.offset]
    ed = _first_reg_operand_edit(proj.hbc.disasm(sf.index))
    if ed is None:
        pytest.skip("no Reg8 operand")
    row, tok, nv = ed

    st = _editor(proj, sf.index)
    drive_instructions(st, DOWN * row + RIGHT * tok + ENTER + f"r{nv}" + ENTER + CX + "y" + "y")
    assert st.commit is True and st.edit_all is True

    fidx, asm, frame, warnings, edit_all = assemble_committed_function(st)
    assert edit_all is True
    warnings = list(warnings) + list(
        proj.apply_insertion(fidx, asm, frame, edit_all=edit_all))
    assert any("shared the same bytecode" in w for w in warnings)

    # every function in the group has the new operand...
    for gi in group[:4]:
        assert proj.hbc.disasm(gi)[row].operands[tok - 1].u32 == nv
    # ...a function outside the group is intact (same-size edit: N=0, no shift)
    other = next(f.index for f in funcs
                 if f.offset != sf.offset and not f.overflowed)
    h0 = _libhbc.HBCFile.open(proj.hbc_path)
    assert ([x.mnemonic for x in h0.disasm(other)]
            == [x.mnemonic for x in proj.hbc.disasm(other)])


def test_shared_edit_all_false_is_refused_by_repl_path():
    # cmd_edit must not call apply_insertion if shared and edit_all is False.
    proj = _open("ridemovi.hbc")
    sf = _shared_plain_func(proj.hbc.functions())
    if sf is None:
        pytest.skip("no shared function")
    ed = _first_reg_operand_edit(proj.hbc.disasm(sf.index))
    if ed is None:
        pytest.skip("no Reg8 operand")
    row, tok, nv = ed

    st = _editor(proj, sf.index)
    drive_instructions(st, DOWN * row + RIGHT * tok + ENTER + f"r{nv}" + ENTER + CX + "n")
    # the user said N: no commit, the project stays unpatched
    assert st.commit is False
    assert proj.is_patched is False


# --- overflowed functions ---------------------------------------


def test_overflow_function_framesize_not_capped_at_127():
    proj = _open("overflow_func.hbc")
    ov = next((f for f in proj.hbc.functions() if f.overflowed), None)
    if ov is None:
        pytest.skip("no overflow function in overflow_func.hbc")
    ed = _first_reg_operand_edit(proj.hbc.disasm(ov.index))
    if ed is None:
        pytest.skip("no Reg8 operand")
    row, tok, nv = ed

    st = _editor(proj, ov.index)
    drive_instructions(st, DOWN * row + RIGHT * tok + ENTER + f"r{nv}" + ENTER + CX + "y")
    assert st.commit is True

    # the overflow function's frameSize can exceed 127: it must not trigger
    # the "> 127" rejection (which only applies to non-overflow functions).
    fidx, asm, frame, warnings, _edall = assemble_committed_function(st)
    proj.apply_insertion(fidx, asm, frame)
    g = proj.hbc.functions()[ov.index]
    assert g.overflowed is True
    assert proj.hbc.disasm(ov.index)[row].operands[tok - 1].u32 == nv


def test_overflow_function_insert_grows_and_reparses():
    proj = _open("overflow_func.hbc")
    ov = next((f for f in proj.hbc.functions() if f.overflowed), None)
    if ov is None:
        pytest.skip("no overflow function")
    sz0 = ov.bytecodeSizeInBytes
    n0 = len(proj.hbc.disasm(ov.index))

    st = _editor(proj, ov.index)
    drive_instructions(st, DOWN + "o" + "Mov" + ENTER + ENTER + "r0" + ENTER
               + RIGHT + ENTER + "r0" + ENTER + CX + "y")
    fidx, asm, frame, warnings, _edall = assemble_committed_function(st)
    proj.apply_insertion(fidx, asm, frame)

    g = proj.hbc.functions()[ov.index]
    assert g.overflowed is True
    assert g.bytecodeSizeInBytes == sz0 + 3           # Mov = 1 + Reg8 + Reg8
    assert len(proj.hbc.disasm(ov.index)) == n0 + 1
    assert all(x.mnemonic != "???" for x in proj.hbc.disasm(ov.index))


# --- regression: "compiler-shape" files untouched ----------------


def test_plain_corpus_no_share_prompt_on_clean_exit():
    proj = _open("control_flow_g0.hbc")
    idx = next(f.index for f in proj.hbc.functions() if f.name == "max")
    f = proj.hbc.functions()[idx]
    assert f.shared is False and f.overflowed is False
    st = _editor(proj, idx)
    drive_instructions(st, CX)                    # no changes -> direct exit
    assert st.mode == "nav"
    assert st.edit_all is False
    assert st.commit is False
