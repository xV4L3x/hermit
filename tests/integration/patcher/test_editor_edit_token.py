"""editing a token in the buffer (no disk write).

Headless: drives the real prompt_toolkit Application with a pipe input.
Every sequence ends with Ctrl+C (`\\x03`), which always exits, so the tests
never hang even when unsaved changes remain.
"""


import pytest

import _libhbc
from cli.editor import EditorState, run_editor
from testutil import CORPUS, CR, CZ, DOWN, ENTER, RIGHT, drive_instructions


def _state(name, pred=None):
    path = CORPUS / name
    if not path.exists():
        pytest.skip(f"missing corpus: {name}")
    hbc = _libhbc.HBCFile.open(str(path))
    funcs = hbc.functions()
    idx = next((f.index for f in funcs if pred(f)), 0) if pred else 0
    f = funcs[idx]
    return EditorState(
        f, hbc.disasm(f.index),
        strings=hbc.strings(),
        opcodes=_libhbc.opcode_table(hbc.version()),
    )


def _max_line0(st):
    """control_flow / max - line 0 = 'LoadParam r0, 1' -> slot0 Reg8, slot1 UInt8."""
    return st.lines[st.nav_indices[0]]


# --- operands ---------------------------------------------------------------


def test_edit_operand_reg_ok():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    drive_instructions(st, RIGHT + ENTER + "r3" + ENTER)
    assert st.mode == "nav"
    assert _max_line0(st).slots[0].value == 3
    assert _max_line0(st).dirty is True
    assert st.dirty_count == 1


def test_edit_operand_hex_accepted():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    drive_instructions(st, RIGHT + ENTER + "0x2" + ENTER)
    assert _max_line0(st).slots[0].value == 2


def test_edit_operand_out_of_range_rejected():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    drive_instructions(st, RIGHT + ENTER + "999" + ENTER)
    assert st.mode == "edit_op"          # still in edit: rejected
    assert st.edit_err
    assert _max_line0(st).slots[0].value == 0
    assert st.dirty_count == 0


def test_edit_operand_non_integer_rejected():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    drive_instructions(st, RIGHT + ENTER + "abc" + ENTER)
    assert st.mode == "edit_op"
    assert _max_line0(st).slots[0].value == 0


def test_edit_string_id_by_index():
    st = _state("hello.hbc")  # func 0 'global' has PutById ... "greet" (StringID)
    # find the first line with a string-ref slot
    tgt = next(
        (i for i in st.nav_indices
         if any(s.is_str_ref for s in st.lines[i].slots)),
        None,
    )
    assert tgt is not None
    line = st.lines[tgt]
    slot_pos = next(j for j, s in enumerate(line.slots) if s.is_str_ref)
    n_strings = len(st.strings)
    # navigate: down to the line, then right to the slot
    ci = st.nav_indices.index(tgt)
    keys = DOWN * ci + RIGHT * (slot_pos + 1) + ENTER + "0" + ENTER
    drive_instructions(st, keys)
    assert line.slots[slot_pos].value == 0
    assert line.dirty is True

    # index out of range -> rejected
    st2 = _state("hello.hbc")
    line2 = st2.lines[tgt]
    drive_instructions(st2, DOWN * ci + RIGHT * (slot_pos + 1) + ENTER + str(n_strings + 5) + ENTER)
    assert st2.mode == "edit_op"
    assert line2.slots[slot_pos].value == line2.orig[2][slot_pos][1]


# --- mnemonic ----------------------------------------------------------


def test_edit_mnemonic_same_signature_swaps_and_empties_slots():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    # line 2 = 'JGreater +6, r0, r1'  signature {Addr8,Reg8,Reg8}
    l2 = st.lines[st.nav_indices[2]]
    assert l2.mnemonic == "JGreater"
    drive_instructions(st, DOWN + DOWN + ENTER + "JNotGreater" + ENTER)
    assert st.mode == "nav"
    assert l2.mnemonic == "JNotGreater"
    assert l2.opcode == st.op_by_mn["JNotGreater"].opcode
    # opcode change -> all slots cleared even though the signature is identical
    assert all(s.value is None for s in l2.slots)
    assert l2.complete is False
    assert l2.dirty is True
    # the types stay (signature unchanged)
    assert [str(s.type) for s in l2.slots] == [str(t) for t in st.op_by_mn["JNotGreater"].operands]


def test_refill_emptied_slots_completes_line():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    l2 = st.lines[st.nav_indices[2]]
    # swap -> cursor already on the first slot (tok=1); fill the 3 slots
    keys = (DOWN + DOWN + ENTER + "JNotGreater" + ENTER
            + ENTER + "+6" + ENTER
            + RIGHT + ENTER + "r0" + ENTER
            + RIGHT + ENTER + "r1" + ENTER)
    drive_instructions(st, keys)
    assert [s.value for s in l2.slots] == [6, 0, 1]
    assert l2.complete is True
    assert l2.dirty is True


def test_undo_after_swap_restores_mnemonic_and_values():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    l2 = st.lines[st.nav_indices[2]]
    drive_instructions(st, DOWN + DOWN + ENTER + "JNotGreater" + ENTER + CZ)
    assert l2.mnemonic == "JGreater"
    assert [s.value for s in l2.slots] == [6, 0, 1]
    assert l2.complete is True
    assert st.dirty_count == 0


def test_edit_mnemonic_incompatible_signature_rejected():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    l2 = st.lines[st.nav_indices[2]]
    drive_instructions(st, DOWN + DOWN + ENTER + "LoadConstInt" + ENTER)  # signature {Reg8,Imm32}
    assert st.mode == "edit_mn"
    assert st.edit_err
    assert l2.mnemonic == "JGreater"


def test_edit_mnemonic_unknown_rejected():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    l2 = st.lines[st.nav_indices[2]]
    drive_instructions(st, DOWN + DOWN + ENTER + "Bogus" + ENTER)
    assert st.mode == "edit_mn"
    assert l2.mnemonic == "JGreater"


def test_mnemonic_candidates_are_same_signature():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    st.ci, st.tok = 2, 0
    st.begin_edit()
    sig = tuple(s.type for s in st.cur_line.slots)
    for mn in st.cand_all:
        assert tuple(st.op_by_mn[mn].operands) == sig


# --- undo / redo / revert ---------------------------------------------


def test_undo_then_redo():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    drive_instructions(st, RIGHT + ENTER + "r3" + ENTER + CZ)
    assert _max_line0(st).slots[0].value == 0
    assert st.dirty_count == 0
    drive_instructions(st, CR)
    assert _max_line0(st).slots[0].value == 3
    assert st.dirty_count == 1


def test_revert_line_restores_all_edits():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    # two edits on the same line 0: slot0 r0->r3, slot1 1->5
    drive_instructions(st, RIGHT + ENTER + "r3" + ENTER + RIGHT + ENTER + "5" + ENTER + "R")
    assert [s.value for s in _max_line0(st).slots] == [0, 1]
    assert _max_line0(st).dirty is False
    assert st.dirty_count == 0


# --- quit guard (nano-style Y/N prompt on q / Ctrl+X) ------------


def test_quit_prompts_when_dirty():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    drive_instructions(st, RIGHT + ENTER + "r3" + ENTER + "q")  # q with changes -> prompt, doesn't exit
    assert st.mode == "exit_ask"
    assert st.commit is False


def test_clean_quit_exits_directly():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    drive_instructions(st, "q")
    assert st.mode == "nav"
    assert st.commit is False


# --- no disk writes -------------------------------------------------


def test_editor_has_no_disk_io():
    st = _state("control_flow.hbc", lambda f: f.name == "max")
    drive_instructions(st, RIGHT + ENTER + "r3" + ENTER)
    assert not hasattr(st, "save")
    assert not hasattr(run_editor, "serialize")
