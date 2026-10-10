"""Interactive bytecode editor (`edit func:<N>`).

Token-granularity cursor over a function's instructions. Edits happen in an
in-memory buffer: an operand takes a new value validated against its physical
type, a mnemonic can become any opcode with an identical operand signature
(same size). Ctrl+Z / Ctrl+R undo and redo, `R` restores the current line.
Ctrl+X exits, asking Y/N when the buffer changed.

On Y, a same-size edit is re-encoded by `encode_committed_lines` and applied
through `HermitProject.apply_patch`. Inserting (`o`/`O`) or deleting (Ctrl+K)
instructions changes the layout, so `assemble_committed_function` builds the
AsmInstr list for `HermitProject.apply_insertion` (`Module`: jump relocation,
branch relaxation, EH relocation). The `patch` command writes the new `.hbc`.
"""

from __future__ import annotations

from types import SimpleNamespace

from prompt_toolkit.filters import Condition
from prompt_toolkit.key_binding import KeyBindings

import _libhbc
from cli.editor.common import (
    CURSOR_STYLE, DIRTY_MARK_STYLE, EMPTY_STYLE, ERR_STYLE, GUTTER_STYLE, HINT_STYLE,
    INCOMPLETE_MARK_STYLE, INFO_STYLE, MNEMONIC_STYLE, SEL_STYLE,
    bind_abort, bind_exit_prompt, fmt_num, run_screen, strip_kind_suffix, view_height,
)

_T = _libhbc.OperandType

_OP_STYLE = {
    _T.Reg8: "fg:ansicyan", _T.Reg32: "fg:ansicyan",
    _T.StringID: "fg:ansigreen",
    _T.Addr8: "fg:ansimagenta", _T.Addr32: "fg:ansimagenta",
    _T.Imm32: "fg:ansiyellow", _T.Double: "fg:ansiyellow",
}
_OP_KIND_NAME = {
    _T.Reg8: "Reg8", _T.Reg32: "Reg32", _T.UInt8: "UInt8", _T.UInt16: "UInt16",
    _T.UInt32: "UInt32", _T.Imm32: "Imm32", _T.Double: "Double",
    _T.Addr8: "Addr8", _T.Addr32: "Addr32", _T.StringID: "StringID",
}

# allowed ranges per physical type (min, max) inclusive; None = no integer constraint
_INT_RANGE = {
    _T.Reg8: (0, 0xFF), _T.UInt8: (0, 0xFF),
    _T.UInt16: (0, 0xFFFF),
    _T.Reg32: (0, 0xFFFFFFFF), _T.UInt32: (0, 0xFFFFFFFF),
    _T.Imm32: (-(2**31), 2**31 - 1), _T.Addr32: (-(2**31), 2**31 - 1),
    _T.Addr8: (-128, 127),
}


class _Slot:
    __slots__ = ("type", "value", "is_str_ref")

    def __init__(self, type_, value, is_str_ref):
        self.type = type_          # _libhbc.OperandType - PHYSICAL type
        self.value = value         # int (Reg/UInt/Imm/Addr/string index) | float (Double)
        self.is_str_ref = is_str_ref

    def clone(self):
        return _Slot(self.type, self.value, self.is_str_ref)

    def key(self):
        return (self.type, self.value, self.is_str_ref)


class _Line:
    __slots__ = ("nav", "offset", "mnemonic", "opcode", "slots", "dirty", "orig",
                 "note", "inserted", "jump_target")

    def __init__(self, nav, *, offset=None, mnemonic="", opcode=0,
                 slots=(), note="", inserted=False):
        self.nav = nav
        self.offset = offset           # func-relative byte offset; None if inserted
        self.mnemonic = mnemonic
        self.opcode = opcode
        self.slots = list(slots)
        self.dirty = False
        self.note = note
        self.inserted = inserted       # True = line added on insert (offset still unknown)
        self.jump_target = None        # for an INSERTED jump: nav-line index of the target
        self.orig = self._snapshot()

    def _snapshot(self):
        return (self.mnemonic, self.opcode, tuple(s.key() for s in self.slots))

    def matches_orig(self) -> bool:
        return self._snapshot() == self.orig

    def restore_orig(self):
        mn, op, slotkeys = self.orig
        self.mnemonic = mn
        self.opcode = op
        self.slots = [_Slot(t, v, r) for (t, v, r) in slotkeys]
        self.dirty = False

    def clone(self):
        c = _Line(self.nav, offset=self.offset, mnemonic=self.mnemonic,
                  opcode=self.opcode, slots=[s.clone() for s in self.slots],
                  note=self.note, inserted=self.inserted)
        c.dirty = self.dirty
        c.orig = self.orig
        c.jump_target = self.jump_target
        return c

    @property
    def n_tokens(self) -> int:
        return 1 + len(self.slots) if self.nav else 0

    @property
    def complete(self) -> bool:
        """True if every slot has a value (none cleared by an opcode change)."""
        return all(s.value is not None for s in self.slots)


def _op_value(op) -> object:
    t = op.type
    if t in (_T.Imm32, _T.Addr8, _T.Addr32):
        return op.i32
    if t == _T.Double:
        return op.f64
    return op.u32  # Reg*/UInt*/StringID(index)


def _build_lines(func, instrs, op_by_mn) -> list[_Line]:
    lines: list[_Line] = []
    for ins in instrs:
        desc = op_by_mn.get(ins.mnemonic)
        slots: list[_Slot] = []
        if desc is not None and len(desc.operands) == len(ins.operands):
            mask = desc.string_ref_mask
            for i, (ptype, op) in enumerate(zip(desc.operands, ins.operands)):
                slots.append(_Slot(ptype, _op_value(op), bool(mask & (1 << i))))
        else:
            # unknown or misaligned opcode: slots from the physical types seen by the disasm
            for op in ins.operands:
                slots.append(_Slot(op.type, _op_value(op), op.type == _T.StringID))
        lines.append(_Line(True, offset=ins.offset, mnemonic=ins.mnemonic,
                           opcode=ins.opcode, slots=slots))

    if not lines:
        lines.append(_Line(False, note="  (function with no bytecode)"))

    if func.hasExceptionHandler and func.exceptionHandlers:
        lines.append(_Line(False, note=""))
        lines.append(_Line(False, note="  Exception Handlers:"))
        for i, eh in enumerate(func.exceptionHandlers):
            lines.append(_Line(
                False,
                note=f"    [{i}]  try {eh.start:#06x}..{eh.end:#06x}"
                     f"  ->  catch @ {eh.target:#06x}",
            ))
    return lines


def _clone_lines(lines):
    return [ln.clone() for ln in lines]


class EditorState:
    def __init__(self, func, instrs, *, strings=None, opcodes=None, version=None):
        self.func = func
        self.version = version      # HBC version (for the encoder at commit time)
        self.commit = False         # outcome of the exit prompt: True = apply
        self.strings = list(strings) if strings is not None else []
        self.op_list = list(opcodes) if opcodes is not None else []
        self.op_by_mn = {d.mnemonic: d for d in self.op_list}
        self.by_sig: dict[tuple, list[str]] = {}
        for d in self.op_list:
            self.by_sig.setdefault(tuple(d.operands), []).append(d.mnemonic)
        for v in self.by_sig.values():
            v.sort()

        self.lines = _build_lines(func, instrs, self.op_by_mn)
        self.nav_indices = [i for i, ln in enumerate(self.lines) if ln.nav]
        self._orig_nav_count = len(self.nav_indices)   # to detect insert/delete
        self._orig_offsets = [self.lines[i].offset for i in self.nav_indices]  # snapshot
        self._insert_above = False
        # Shared bytecode - editing the bytes changes every function in the group.
        self.shared = bool(getattr(func, "shared", False))
        self.share_count = int(getattr(func, "shareCount", 1))
        self.edit_all = False   # answer to "apply to all?" (share_ask)
        self.ci = 0
        self.tok = 0
        self.top = 0
        mn_lens = [len(ln.mnemonic) for ln in self.lines if ln.nav]
        self.mn_width = min(20, max(mn_lens)) if mn_lens else 1

        self.mode = "nav"   # nav | edit_op | edit_mn | exit_ask | insert_pick | insert_target | share_ask
        self.edit_buf = ""
        self.edit_err = ""
        self.cand_all: list[str] = []
        self.cand_sel = 0

        self.undo: list = []
        self.redo: list = []
        self.quit_armed = False

    # --- current line / token ----------------------------------------------

    @property
    def cur_line_idx(self) -> int:
        return self.nav_indices[self.ci] if self.nav_indices else 0

    @property
    def cur_line(self):
        return self.lines[self.cur_line_idx] if self.nav_indices else None

    @property
    def cur_slot(self):
        ln = self.cur_line
        if ln and 1 <= self.tok <= len(ln.slots):
            return ln.slots[self.tok - 1]
        return None

    @property
    def dirty_count(self) -> int:
        return sum(1 for i in self.nav_indices if self.lines[i].dirty)

    # --- movement ----------------------------------------------------------

    def move_token(self, delta: int) -> None:
        ln = self.cur_line
        if ln:
            self.tok = max(0, min(ln.n_tokens - 1, self.tok + delta))

    def move_line(self, delta: int) -> None:
        if not self.nav_indices:
            return
        self.ci = max(0, min(len(self.nav_indices) - 1, self.ci + delta))
        self.tok = 0

    def move_line_to(self, ci: int) -> None:
        if not self.nav_indices:
            return
        self.ci = max(0, min(len(self.nav_indices) - 1, ci))
        self.tok = 0

    def ensure_visible(self, view_height: int) -> None:
        idx = self.cur_line_idx
        if idx < self.top:
            self.top = idx
        elif idx >= self.top + view_height:
            self.top = idx - view_height + 1
        self.top = max(0, min(self.top, max(0, len(self.lines) - view_height)))

    # --- undo / redo ---------------------------------------------------

    def _snapshot(self):
        return (_clone_lines(self.lines), self.ci, self.tok, self.top)

    def _restore(self, snap):
        lines, ci, tok, top = snap
        # IN-PLACE restore: keeps the identity of the list and of the _Line objects
        # (on a same-size edit the structure never changes - same count and same nav lines).
        if len(lines) == len(self.lines):
            for dst, src in zip(self.lines, lines):
                dst.nav, dst.offset = src.nav, src.offset
                dst.mnemonic, dst.opcode = src.mnemonic, src.opcode
                dst.slots = [s.clone() for s in src.slots]
                dst.dirty, dst.note, dst.orig = src.dirty, src.note, src.orig
                dst.inserted, dst.jump_target = src.inserted, src.jump_target
        else:
            self.lines = _clone_lines(lines)
        self.nav_indices = [i for i, ln in enumerate(self.lines) if ln.nav]
        self.ci = max(0, min(len(self.nav_indices) - 1, ci)) if self.nav_indices else 0
        self.tok = tok
        self.top = top
        ln = self.cur_line
        if ln:
            self.tok = max(0, min(ln.n_tokens - 1, self.tok))

    def push_undo(self):
        self.undo.append(self._snapshot())
        if len(self.undo) > 200:
            self.undo.pop(0)
        self.redo.clear()

    def undo_action(self):
        if not self.undo:
            return
        self.redo.append(self._snapshot())
        self._restore(self.undo.pop())
        self.quit_armed = False

    def redo_action(self):
        if not self.redo:
            return
        self.undo.append(self._snapshot())
        self._restore(self.redo.pop())
        self.quit_armed = False

    # --- edit mode -----------------------------------------------------

    def begin_edit(self):
        ln = self.cur_line
        if ln is None or not ln.slots and self.tok > 0:
            return
        # INSERTED jump, cursor on the Addr operand -> target selection (not typing a number)
        if ln.inserted and self.tok == 1 and self.is_jump_line(ln):
            self.begin_insert_target(ln)
            return
        self.edit_buf = ""
        self.edit_err = ""
        if self.tok == 0:
            if not self.op_by_mn:
                return
            self.cand_all = list(self.by_sig.get(tuple(s.type for s in ln.slots), []))
            if ln.mnemonic not in self.cand_all:
                self.cand_all.append(ln.mnemonic)
                self.cand_all.sort()
            try:
                self.cand_sel = self.cand_all.index(ln.mnemonic)
            except ValueError:
                self.cand_sel = 0
            self.mode = "edit_mn"
        else:
            self.mode = "edit_op"

    def cancel_edit(self):
        self.mode = "nav"
        self.edit_buf = ""
        self.edit_err = ""

    def filtered_candidates(self) -> list[str]:
        q = self.edit_buf.strip().lower()
        if not q:
            return self.cand_all
        pref = [m for m in self.cand_all if m.lower().startswith(q)]
        sub = [m for m in self.cand_all if q in m.lower() and m not in pref]
        return pref + sub

    def edit_type_char(self, ch: str):
        self.edit_buf += ch
        self.edit_err = ""
        if self.mode == "edit_mn":
            self.cand_sel = 0

    def edit_backspace(self):
        self.edit_buf = self.edit_buf[:-1]
        self.edit_err = ""
        if self.mode == "edit_mn":
            self.cand_sel = 0

    def edit_cycle(self, delta: int):
        cands = self.filtered_candidates()
        if cands:
            self.cand_sel = (self.cand_sel + delta) % len(cands)

    def commit_edit(self):
        if self.mode == "insert_pick":
            self.commit_insert()
            return
        if self.mode == "insert_target":
            self.set_insert_target()
            return
        ln = self.cur_line
        if ln is None:
            self.cancel_edit()
            return
        if self.mode == "edit_mn":
            self._commit_mnemonic(ln)
        elif self.mode == "edit_op":
            self._commit_operand(ln)

    def _commit_mnemonic(self, ln):
        cands = self.filtered_candidates()
        target = None
        buf = self.edit_buf.strip()
        if cands:
            target = cands[min(self.cand_sel, len(cands) - 1)]
        elif buf in self.op_by_mn and buf in self.cand_all:
            target = buf
        if target is None:
            self.edit_err = f"no opcode compatible with {buf!r}"
            return
        if target == ln.mnemonic:
            self.cancel_edit()
            return
        self.push_undo()
        ln.mnemonic = target
        ln.opcode = self.op_by_mn[target].opcode
        # opcode change -> operands need reconfirming: clear all slots
        # (the signature is identical, so the TYPES stay; only the values reset)
        for s in ln.slots:
            s.value = None
        ln.dirty = True
        self.tok = 1 if ln.slots else 0   # move the cursor to the first slot to fill in
        self.quit_armed = False
        self.cancel_edit()

    def _commit_operand(self, ln):
        slot = ln.slots[self.tok - 1]
        try:
            newval = _parse_operand(self.edit_buf, slot, self.strings)
        except _EditError as e:
            self.edit_err = str(e)
            return
        self.push_undo()
        slot.value = newval
        ln.dirty = not ln.matches_orig()
        self.quit_armed = False
        self.cancel_edit()

    def revert_line(self):
        ln = self.cur_line
        if ln is None or ln.matches_orig():
            return
        self.push_undo()
        ln.restore_orig()
        self.quit_armed = False

    # --- line insertion / deletion -----------------------------

    @property
    def structural_change(self) -> bool:
        """True if the instruction count changed (insert/delete) or there are
        inserted lines - requires the structural serialization path
        (`Module`), not the same-size patch."""
        return (len(self.nav_indices) != self._orig_nav_count
                or any(self.lines[i].inserted for i in self.nav_indices))

    def delete_line(self):
        """Delete the current instruction from the buffer. No check here -
        if the deletion breaks a jump or a handler, the error surfaces at commit."""
        if not self.nav_indices or len(self.nav_indices) < 2:
            self.edit_err = "you can't delete the function's last instruction"
            return
        self.push_undo()
        del self.lines[self.cur_line_idx]
        self.nav_indices = [i for i, ln in enumerate(self.lines) if ln.nav]
        self.ci = min(self.ci, len(self.nav_indices) - 1)
        self.tok = 0
        self.quit_armed = False

    def begin_insert(self, above: bool):
        if not self.op_by_mn:
            return
        self._insert_above = above
        self.edit_buf = ""
        self.edit_err = ""
        self.cand_all = sorted(self.op_by_mn)
        self.cand_sel = 0
        self.mode = "insert_pick"

    def commit_insert(self):
        cands = self.filtered_candidates()
        buf = self.edit_buf.strip()
        target = cands[min(self.cand_sel, len(cands) - 1)] if cands else (
            buf if buf in self.op_by_mn else None)
        if target is None:
            self.edit_err = f"unknown opcode: {buf!r}"
            return
        desc = self.op_by_mn[target]
        if target == "SwitchImm":
            self.edit_err = "SwitchImm cannot be inserted (attached jump table)"
            return
        if target.startswith("SaveGenerator"):
            self.edit_err = "generator opcodes are not supported"
            return

        self.push_undo()
        mask = desc.string_ref_mask
        slots = [_Slot(t, None, bool(mask & (1 << i))) for i, t in enumerate(desc.operands)]
        ln = _Line(True, offset=None, mnemonic=target, opcode=desc.opcode,
                   slots=slots, inserted=True)
        ln.dirty = True

        at = self.cur_line_idx if self._insert_above else self.cur_line_idx + 1
        self.lines.insert(at, ln)
        self.nav_indices = [i for i, x in enumerate(self.lines) if x.nav]
        self.ci = self.nav_indices.index(at)
        self.tok = 1 if slots else 0
        self.quit_armed = False
        self.cancel_edit()

    def is_jump_line(self, ln) -> bool:
        d = self.op_by_mn.get(ln.mnemonic)
        return bool(d) and d.jump_kind in ("Unconditional", "Conditional")

    def begin_insert_target(self, ln):
        """Target-selection mode for an INSERTED jump: the cursor moves
        freely, Enter sets the current line as the destination."""
        self._target_line = ln
        self.mode = "insert_target"
        self.edit_err = ""

    def set_insert_target(self):
        ln = getattr(self, "_target_line", None)
        if ln is None or not ln.slots:
            self.cancel_edit()
            return
        self.push_undo()
        ln.jump_target = self.ci        # index in nav_indices of the destination line
        ln.slots[0].value = 0           # placeholder: the assembler computes the displacement
        self._target_line = None
        self.cancel_edit()


class _EditError(Exception):
    pass


def _parse_operand(raw: str, slot: _Slot, strings: list) -> object:
    s = raw.strip()
    if not s:
        raise _EditError("empty value")
    t = slot.type

    if t == _T.Double:
        try:
            return float(s)
        except ValueError:
            raise _EditError("not a number")

    if t in (_T.Reg8, _T.Reg32) and s[:1] in ("r", "R"):
        s = s[1:]
    try:
        n = int(s, 0)
    except ValueError:
        raise _EditError("not an integer (use decimal or 0x…)")

    if slot.is_str_ref:
        if not strings:
            raise _EditError("string table not available")
        if not (0 <= n < len(strings)):
            raise _EditError(f"string index out of range (0–{len(strings) - 1})")
        width_max = {_T.UInt8: 0xFF, _T.UInt16: 0xFFFF}.get(t, 0xFFFFFFFF)
        if n > width_max:
            raise _EditError(f"index doesn't fit in {_OP_KIND_NAME.get(t, t)} (same-size edit doesn't widen)")
        return n

    rng = _INT_RANGE.get(t)
    if rng and not (rng[0] <= n <= rng[1]):
        raise _EditError(f"out of range for {_OP_KIND_NAME.get(t, t)} ({rng[0]}–{rng[1]})")
    return n


# Commit buffer -> bytes

def encode_committed_lines(state: "EditorState") -> dict[int, bytes]:
    """Re-encodes the modified lines, keyed by ABSOLUTE file offset
    (`func.offset + line.offset`). Each result must have exactly the length of
    the original instruction.

    Raises `ValueError` for an unknown opcode, `SwitchImm` (its jump table
    isn't relocated), empty slots, or an encoded length different from the original."""
    if state.version is None:
        raise ValueError("HBC version unknown to the editor - cannot encode")

    base = state.func.offset
    out: dict[int, bytes] = {}
    for i in state.nav_indices:
        ln = state.lines[i]
        if not ln.dirty:
            continue
        if not ln.complete:
            raise ValueError(
                f"line @{ln.offset:#06x} incomplete - fill it in or press R to restore it")

        desc = state.op_by_mn.get(ln.mnemonic)
        if desc is None:
            raise ValueError(f"line @{ln.offset:#06x}: unknown opcode {ln.mnemonic!r}")
        if desc.jump_kind == "Switch":
            raise ValueError(
                f"line @{ln.offset:#06x}: editing SwitchImm is not supported "
                f"(the attached jump table is not relocated)")

        orig_types = [t for (t, _v, _r) in ln.orig[2]]
        expected = 1 + sum(_libhbc.operand_size(t) for t in orig_types)

        enc = _libhbc.encode_instruction(
            state.version, ln.opcode, [s.value for s in ln.slots])

        if len(enc) != expected:
            raise ValueError(
                f"line @{ln.offset:#06x}: encoded {len(enc)} B ≠ expected {expected} B "
                f"- the edit is not same-size")

        out[base + ln.offset] = enc
    return out


# Structural commit -> Module (insert / delete)

_REG_TYPES = (_T.Reg8, _T.Reg32)


def assemble_committed_function(state: "EditorState"):
    """The buffer (inserts and deletes included) as the AsmInstr list for
    `Module.set_function_body`: `(func_idx, asm_instrs, frame_size, warnings, edit_all)`,
    where `warnings` are non-blocking.

    Raises `ValueError` for incomplete lines, unknown opcodes, SwitchImm, an
    inserted jump with no target, the target of a deleted jump, a deleted
    exception handler boundary, or a frameSize bump on a function with an
    exception handler."""
    if state.version is None:
        raise ValueError("HBC version unknown to the editor")

    nav = [state.lines[i] for i in state.nav_indices]
    warnings: list[str] = []

    # ORIGINAL byte offset -> index in nav, to resolve existing jump targets.
    byte_to_idx = {ln.offset: k for k, ln in enumerate(nav) if ln.offset is not None}

    # A deleted instruction must not be an exception handler boundary.
    if state.func.hasExceptionHandler:
        surviving = set(byte_to_idx)
        deleted = {o for o in state._orig_offsets if o is not None} - surviving
        if deleted:
            eh_bounds: set[int] = set()
            for e in state.func.exceptionHandlers:
                eh_bounds |= {e.start, e.end, e.target}
            bad = sorted(deleted & eh_bounds)
            if bad:
                raise ValueError(
                    "deleted an instruction that is an exception handler boundary "
                    f"(offset {[hex(b) for b in bad]}) - move the try/catch first")

    max_reg = -1
    asm = []
    for k, ln in enumerate(nav):
        if not ln.complete:
            raise ValueError(f"line {k}: incomplete instruction - fill it in or cancel")
        desc = state.op_by_mn.get(ln.mnemonic)
        if desc is None:
            raise ValueError(f"line {k}: unknown opcode {ln.mnemonic!r}")
        if desc.jump_kind == "Switch":
            raise ValueError(f"line {k}: SwitchImm not supported")

        operands = [float(s.value) for s in ln.slots]

        jt, ap = -1, -1
        if desc.jump_kind in ("Unconditional", "Conditional"):
            ap = 0
            if ln.inserted:
                if ln.jump_target is None:
                    raise ValueError(f"line {k}: inserted jump has no target (press Enter on the Addr operand)")
                jt = int(ln.jump_target)
            else:
                orig_addr = ln.orig[2][0][1]        # original displacement
                tgt_byte = ln.offset + int(orig_addr)
                if tgt_byte not in byte_to_idx:
                    raise ValueError(
                        f"line {k}: the jump target ({tgt_byte:#x}) was "
                        f"deleted or is no longer aligned to an instruction")
                jt = byte_to_idx[tgt_byte]

        for s in ln.slots:
            if s.type in _REG_TYPES and s.value is not None:
                max_reg = max(max_reg, int(s.value))

        asm.append((ln.opcode, operands, jt, ap,
                    -1 if ln.offset is None else int(ln.offset)))

    frame_size = max(state.func.frameSize, max_reg + 1)
    if frame_size > 127 and not getattr(state.func, "overflowed", False):
        raise ValueError(f"frameSize {frame_size} > 127 (7-bit field) - "
                         f"the function would need to become overflowed (not supported)")
    if frame_size > state.func.frameSize and state.func.hasExceptionHandler:
        raise ValueError(
            "the inserted instruction would use a new register, but the function has "
            "an exception handler - use only registers < {} (frameSize bump is not "
            "safe with EH)".format(state.func.frameSize))

    # soft warning: the last instruction is not a terminator
    if nav:
        last = state.op_by_mn.get(nav[-1].mnemonic)
        if last is not None and last.jump_kind not in ("Return", "Throw"):
            warnings.append(
                f"the last instruction ({nav[-1].mnemonic}) is not Ret/Throw - "
                f"the function might execute past the end of the bytecode")

    return state.func.index, asm, frame_size, warnings, bool(getattr(state, "edit_all", False))


# Rendering

def _slot_text(slot: _Slot, strings: list) -> str:
    t, v = slot.type, slot.value
    if v is None:
        return f"⟨{_OP_KIND_NAME.get(t, '?')}⟩"   # ⟨Reg8⟩ - slot to fill in
    if slot.is_str_ref:
        if 0 <= v < len(strings):
            sv = strip_kind_suffix(str(strings[v]))
            sv = sv.replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
            if len(sv) > 30:
                sv = sv[:27] + "..."
            return f'"{sv}"'
        return f"#{v}"
    if t in (_T.Reg8, _T.Reg32):
        return f"r{v}"
    if t == _T.Double:
        return fmt_num(v)
    if t in (_T.Addr8, _T.Addr32):
        return f"{'+' if v >= 0 else ''}{v}"
    return str(v)


def _tok_style(slot: _Slot | None, is_mn: bool) -> str:
    if is_mn:
        return MNEMONIC_STYLE
    return _OP_STYLE.get(slot.type, "") if slot else ""


def _line_fragments(state: "EditorState", line: _Line, is_cursor: bool):
    if not line.nav:
        return [(INFO_STYLE, line.note)]

    incomplete = not line.complete
    if incomplete:
        mark, mark_style = "!", INCOMPLETE_MARK_STYLE
    elif line.inserted:
        mark, mark_style = "+", DIRTY_MARK_STYLE
    elif line.dirty:
        mark, mark_style = "*", DIRTY_MARK_STYLE
    else:
        mark, mark_style = " ", GUTTER_STYLE
    off_txt = "??????" if line.offset is None else f"{line.offset:#06x}"
    frags = [(mark_style, mark), (GUTTER_STYLE, f"{off_txt}  ")]

    n_tokens = 1 + len(line.slots)
    for i in range(n_tokens):
        if i == 1:
            pad = max(1, state.mn_width - len(line.mnemonic)) + 1
            frags.append(("", " " * pad))
        elif i >= 2:
            frags.append(("", ", "))
        if i == 0:
            text, base = line.mnemonic, MNEMONIC_STYLE
        else:
            slot = line.slots[i - 1]
            text = _slot_text(slot, state.strings)
            base = EMPTY_STYLE if slot.value is None else _tok_style(slot, False)
        style = CURSOR_STYLE if (is_cursor and i == state.tok) else base
        frags.append((style, text))
    return frags


# edit func:new builds a NEW function in this same editor, from a stand-in
# FunctionInfo and two seed instructions. Its fixed metadata (paramCount 1,
# functionName 0, strict, callable and constructible) is set in C++
# (`Module::add_function`).

def new_function_stub():
    """`FunctionInfo`-like stand-in for `edit func:new`: the function isn't in
    `hbc.functions()` yet. `index=-1` is a sentinel, never used as an index
    (the REPL calls `apply_add_function`)."""
    return SimpleNamespace(
        index=-1, name="<new function>", frameSize=1,
        hasExceptionHandler=False, exceptionHandlers=[],
        shared=False, shareCount=1, overflowed=False,
    )


def new_function_seed_instrs(opcodes):
    """Seed body `LoadConstUndefined r0` / `Ret r0` (returns `undefined`),
    duck-typed as `hbc.Instruction`/`Operand` with the fields `_build_lines` reads."""
    op_by_mn = {d.mnemonic: d for d in opcodes}
    lcu = op_by_mn["LoadConstUndefined"]
    ret = op_by_mn["Ret"]
    reg0 = SimpleNamespace(type=_T.Reg8, u32=0, i32=0, f64=0.0)
    return [
        SimpleNamespace(offset=0, mnemonic="LoadConstUndefined", opcode=lcu.opcode, operands=[reg0]),
        SimpleNamespace(offset=2, mnemonic="Ret",                opcode=ret.opcode, operands=[reg0]),
    ]


def run_editor(func, instrs, *, strings=None, opcodes=None, version=None,
               _input=None, _output=None, _state=None) -> "EditorState":
    """Full-screen editor for a function; returns the state on exit.
    `strings` resolve and validate StringID operands, `opcodes` is
    `_libhbc.opcode_table(version)`, `version` is needed to encode at commit.
    `_input/_output/_state` are for headless tests."""
    state = _state if _state is not None else EditorState(
        func, instrs, strings=strings, opcodes=opcodes, version=version)

    def _title():
        d = state.dirty_count
        inc = sum(1 for i in state.nav_indices if not state.lines[i].complete)
        ins = sum(1 for i in state.nav_indices if state.lines[i].inserted)
        deleted = state._orig_nav_count - (len(state.nav_indices) - ins)
        dtxt = f"   {d} modified" if d else ""
        itxt = f"   {inc} incomplete" if inc else ""
        ntxt = f"   +{ins} inserted" if ins else ""
        xtxt = f"   -{deleted} deleted" if deleted > 0 else ""
        stxt = f"   ⚠ shared ×{state.share_count}" if state.shared else ""
        otxt = "   overflow" if getattr(func, "overflowed", False) else ""
        return [(
            "bold" if not inc else ERR_STYLE,
            f" hermit edit - func[{func.index}] {func.name}   "
            f"{len(state.nav_indices)} instr   {func.frameSize} regs{otxt}{stxt}   "
            f"EDIT{dtxt}{ntxt}{xtxt}{itxt} ",
        )]

    def _body():
        vh = view_height()
        state.ensure_visible(vh)
        out = []
        for off, line in enumerate(state.lines[state.top: state.top + vh]):
            real = state.top + off
            is_cursor = line.nav and real == state.cur_line_idx
            out.extend(_line_fragments(state, line, is_cursor))
            out.append(("", "\n"))
        if out and out[-1] == ("", "\n"):
            out.pop()
        return out

    def _status():
        ln = state.cur_line
        if ln is None:
            return [(CURSOR_STYLE, " (no instruction) ")]
        if state.tok == 0:
            kind, raw = "mnemonic", f"opcode {ln.opcode:#04x}"
        else:
            slot = ln.slots[state.tok - 1]
            kind = _OP_KIND_NAME.get(slot.type, "?")
            if ln.inserted and state.tok == 1 and state.is_jump_line(ln):
                if ln.jump_target is None:
                    raw = "⟨target not chosen - Enter, then ↑↓ and Enter on the destination⟩"
                else:
                    dst = state.lines[state.nav_indices[ln.jump_target]]
                    raw = f"-> line {ln.jump_target} ({dst.mnemonic})"
            elif slot.value is None:
                raw = "⟨empty - Enter to fill in⟩"
            elif slot.is_str_ref:
                sv = state.strings[slot.value] if 0 <= slot.value < len(state.strings) else "?"
                raw = f"#{slot.value}  {strip_kind_suffix(str(sv))!r}"
            elif slot.type in (_T.Addr8, _T.Addr32):
                tgt = "?" if ln.offset is None else f"{ln.offset + slot.value:#06x}"
                raw = f"{slot.value:+d}  -> {tgt}"
            else:
                raw = fmt_num(slot.value) if slot.type == _T.Double else str(slot.value)
        dtag = "  [inserted]" if ln.inserted else ("  [dirty]" if ln.dirty else "")
        off_txt = "??????" if ln.offset is None else f"{ln.offset:#06x}"
        return [(
            CURSOR_STYLE,
            f" line {state.ci + 1}/{len(state.nav_indices)}   off {off_txt}   "
            f"token {state.tok}: {kind} = {raw}{dtag} ",
        )]

    def _bar1():
        if state.mode == "insert_pick":
            return [(HINT_STYLE, f" insert {'above' if state._insert_above else 'below'} "),
                    ("", state.edit_buf), ("bold", "▁"),
                    (HINT_STYLE, "   Tab cycles · Enter · Esc")]
        if state.mode == "insert_target":
            return [(DIRTY_MARK_STYLE, " choose the jump destination "),
                    (HINT_STYLE, "  ↑↓ move · Enter confirms · Esc cancels")]
        if state.mode == "share_ask":
            return [(INCOMPLETE_MARK_STYLE,
                     f" ⚠ this code is shared by {state.share_count} functions - "
                     f"apply the change to ALL of them? "),
                    ("bold", " Y "), (HINT_STYLE, "yes   "),
                    ("bold", " N/Esc "), (HINT_STYLE, "cancel (private clone not supported) ")]
        if state.mode == "exit_ask":
            d = state.dirty_count
            ins = sum(1 for i in state.nav_indices if state.lines[i].inserted)
            deleted = state._orig_nav_count - (len(state.nav_indices) - ins)
            parts = []
            if d:       parts.append(f"{d} modified")
            if ins:     parts.append(f"{ins} inserted")
            if deleted > 0: parts.append(f"{deleted} deleted")
            summary = ", ".join(parts) or "changes"
            return [(DIRTY_MARK_STYLE, f" {summary} - apply to the in-memory file? "),
                    ("bold", " Y "), (HINT_STYLE, "apply and exit   "),
                    ("bold", " N "), (HINT_STYLE, "discard and exit   "),
                    ("bold", " Esc "), (HINT_STYLE, "cancel ")]
        if state.mode == "edit_mn":
            ln = state.cur_line
            sig = ",".join(_OP_KIND_NAME.get(s.type, "?") for s in ln.slots) or "-"
            return [(HINT_STYLE, " opcode "), ("", f"{state.edit_buf}"),
                    ("bold", "\u2581"), (HINT_STYLE, f"   [signature {sig}]")]
        if state.mode == "edit_op":
            slot = state.cur_slot
            kind = _OP_KIND_NAME.get(slot.type, "?") if slot else "?"
            old = _slot_text(slot, state.strings) if slot else ""
            return [(HINT_STYLE, f" edit {kind} (was {old}) "),
                    ("", state.edit_buf), ("bold", "\u2581")]
        return [(HINT_STYLE,
                 " <-> token   ↑↓ line   Enter edit   o/O insert   "
                 "^K delete   ^Z/^R undo   R restore   ^X save/exit ")]

    def _bar2():
        if state.edit_err:
            return [(ERR_STYLE, "  " + state.edit_err)]
        if state.mode == "insert_target":
            return [(HINT_STYLE, "  the assembler computes the displacement at commit time")]
        if state.mode == "exit_ask":
            return [(HINT_STYLE,
                     "  Y re-encodes the changes, applies them to the in-memory image "
                     "(with an integrity check) · the .hbc on disk is written by `patch`")]
        if state.mode in ("edit_mn", "insert_pick"):
            cands = state.filtered_candidates()
            if not cands:
                return [(HINT_STYLE, "  (no compatible opcode)")]
            frags = [(HINT_STYLE, "  ")]
            for i, m in enumerate(cands[:10]):
                if i:
                    frags.append((HINT_STYLE, "  "))
                frags.append((SEL_STYLE if i == state.cand_sel else "", m))
            if len(cands) > 10:
                frags.append((HINT_STYLE, f"   +{len(cands) - 10}"))
            frags.append((HINT_STYLE, "   Tab cycles · Enter confirms · Esc cancels"))
            return frags
        if state.mode == "edit_op":
            slot = state.cur_slot
            hint = ""
            if slot:
                if slot.is_str_ref:
                    hint = f"index 0–{max(0, len(state.strings) - 1)}"
                elif slot.type in _INT_RANGE:
                    lo, hi = _INT_RANGE[slot.type]
                    hint = f"{lo}–{hi}"
                elif slot.type == _T.Double:
                    hint = "number"
            return [(HINT_STYLE, f"  {hint}   Enter confirms · Esc cancels")]
        return [("", "")]

    # --- key bindings --------------------------------------------------

    kb = KeyBindings()
    in_nav = Condition(lambda: state.mode == "nav")
    in_type = Condition(lambda: state.mode in ("edit_op", "edit_mn", "insert_pick"))
    in_pick = Condition(lambda: state.mode in ("edit_mn", "insert_pick"))
    in_target = Condition(lambda: state.mode == "insert_target")
    in_editing = Condition(lambda: state.mode in
                           ("edit_op", "edit_mn", "insert_pick", "insert_target"))
    in_exit = Condition(lambda: state.mode == "exit_ask")
    in_share = Condition(lambda: state.mode == "share_ask")

    def _request_exit(event):
        # Clean buffer: exit at once. Otherwise ask Y/N. `structural_change`
        # covers deletions, which don't count in dirty_count.
        if state.dirty_count == 0 and not state.structural_change:
            event.app.exit()
            return
        incomplete = [i for i in state.nav_indices if not state.lines[i].complete]
        if incomplete:
            state.edit_err = (f"{len(incomplete)} incomplete lines (!) - fill them in "
                              f"or press R to restore them before exiting")
            event.app.invalidate()
            return
        state.edit_err = ""
        # Shared bytecode -> ask first whether to apply to all functions.
        if state.shared and not state.edit_all:
            state.mode = "share_ask"
        else:
            state.mode = "exit_ask"
        event.app.invalidate()

    @kb.add("q", filter=in_nav)
    @kb.add("c-x", filter=in_nav)
    def _q(event):
        _request_exit(event)

    @kb.add("y", filter=in_share)
    @kb.add("Y", filter=in_share)
    def _share_yes(event):
        state.edit_all = True
        state.mode = "exit_ask"
        event.app.invalidate()

    @kb.add("n", filter=in_share)
    @kb.add("N", filter=in_share)
    @kb.add("escape", filter=in_share)
    def _share_no(event):
        state.edit_all = False
        state.mode = "nav"
        state.edit_err = ("shared code: without «apply to all» the change "
                          "cannot be saved (private clone not supported)")
        event.app.invalidate()

    bind_exit_prompt(kb, state, in_exit, cancel_mode="nav")
    bind_abort(kb, state)

    @kb.add("left", filter=in_nav)
    def _left(event):
        state.move_token(-1); event.app.invalidate()

    @kb.add("right", filter=in_nav)
    def _right(event):
        state.move_token(+1); event.app.invalidate()

    @kb.add("up", filter=in_nav)
    def _up(event):
        state.move_line(-1); event.app.invalidate()

    @kb.add("down", filter=in_nav)
    def _down(event):
        state.move_line(+1); event.app.invalidate()

    @kb.add("pageup", filter=in_nav)
    def _pgup(event):
        state.move_line(-view_height()); event.app.invalidate()

    @kb.add("pagedown", filter=in_nav)
    def _pgdn(event):
        state.move_line(+view_height()); event.app.invalidate()

    @kb.add("home", filter=in_nav)
    @kb.add("g", filter=in_nav)
    def _home(event):
        state.move_line_to(0); state.top = 0; event.app.invalidate()

    @kb.add("end", filter=in_nav)
    @kb.add("G", filter=in_nav)
    def _end(event):
        state.move_line_to(len(state.nav_indices) - 1); event.app.invalidate()

    @kb.add("enter", filter=in_nav)
    def _open_edit(event):
        state.begin_edit(); event.app.invalidate()

    @kb.add("o", filter=in_nav)
    def _ins_below(event):
        state.begin_insert(above=False); event.app.invalidate()

    @kb.add("O", filter=in_nav)
    def _ins_above(event):
        state.begin_insert(above=True); event.app.invalidate()

    @kb.add("c-k", filter=in_nav)
    def _del_line(event):
        state.delete_line(); event.app.invalidate()

    @kb.add("R", filter=in_nav)
    def _revert(event):
        state.revert_line(); event.app.invalidate()

    @kb.add("c-z", filter=in_nav)
    def _undo(event):
        state.undo_action(); event.app.invalidate()

    @kb.add("c-r", filter=in_nav)
    def _redo(event):
        state.redo_action(); event.app.invalidate()

    # --- edit / insert modes ---
    @kb.add("escape", filter=in_editing)
    def _esc(event):
        state.cancel_edit(); event.app.invalidate()

    @kb.add("enter", filter=in_editing)
    def _commit(event):
        state.commit_edit(); event.app.invalidate()

    @kb.add("backspace", filter=in_type)
    def _bs(event):
        state.edit_backspace(); event.app.invalidate()

    @kb.add("tab", filter=in_pick)
    def _tab(event):
        state.edit_cycle(+1); event.app.invalidate()

    @kb.add("s-tab", filter=in_pick)
    def _stab(event):
        state.edit_cycle(-1); event.app.invalidate()

    @kb.add("<any>", filter=in_type)
    def _typed(event):
        if event.data and event.data.isprintable():
            state.edit_type_char(event.data)
            event.app.invalidate()

    # --- insert_target: free cursor to choose the jump destination ---
    @kb.add("up", filter=in_target)
    def _t_up(event):
        state.move_line(-1); event.app.invalidate()

    @kb.add("down", filter=in_target)
    def _t_down(event):
        state.move_line(+1); event.app.invalidate()

    run_screen(_title, _body, _status, _bar1, _bar2, kb, _input=_input, _output=_output)
    return state