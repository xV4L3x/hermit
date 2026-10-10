"""Array/object literal editor (`edit array:<N>`).

Edits the elements of one NewArrayWithBuffer / NewObjectWithBuffer site. An
element keeps its SLP tag by default (a same-size splice); Ctrl+T changes the
tag, including Null/True/False toggles (a relocation). `a` adds an element (or
key-value pair) and `d` removes the last one. Object keys are read-only: only
the last pair can be added or removed.
"""

from __future__ import annotations

from prompt_toolkit.filters import Condition
from prompt_toolkit.formatted_text import ANSI
from prompt_toolkit.key_binding import KeyBindings

from cli import display
from cli.editor.common import (
    CURSOR_STYLE, ERR_STYLE, HINT_STYLE, TextBufferMixin, apply_discard_cancel_bar,
    bind_abort, bind_exit_prompt, bind_text_input, input_bar, run_screen, view_height,
)
from cli.literals import (
    SLPError, SLPTag, SLPValue, decode_value_bytes, encode_slp_value, slp_render,
    slp_type_name,
)

# The 8 SLP tags in the order of the type picker.
_SLP_TAG_ORDER = (
    SLPTag.NULL, SLPTag.TRUE, SLPTag.FALSE, SLPTag.NUMBER,
    SLPTag.INTEGER, SLPTag.SHORT_STRING, SLPTag.BYTE_STRING, SLPTag.LONG_STRING,
)
_STRING_TAGS = (SLPTag.SHORT_STRING, SLPTag.BYTE_STRING, SLPTag.LONG_STRING)


class ArrayEditState(TextBufferMixin):
    """State of the array/object editor. `values` / `keys` are `SLPValue`s with
    `byte_off` ABSOLUTE in the file (decoded by the caller).

    An element can keep its tag (`patches`: same-size splices) or change it
    via the picker. `changed_indices` is EVERY index touched; `orig_values` is
    the never-mutated pre-session snapshot, used by `cli.repl._edit_array` to
    choose between `apply_array_patch` and a relocation, which re-appends the
    WHOLE site from `values` (the current state, not just the last edit).

    A session is EITHER element edits OR a single add/remove (`pending_add` /
    `pending_remove`), never both. The key of a new pair must reference an
    existing string."""

    def __init__(self, site_idx, site, values, keys, strings, funcs, *,
                 context_bytes=b"", context_base=0,
                 is_shared=False, shared_with=()):
        self.site_idx = site_idx
        self.site = site
        self.values = list(values)
        self.orig_values = list(values)   # PRE-session snapshot, never mutated
        self.keys = list(keys) if keys is not None else None
        self.strings = list(strings)
        self.funcs = list(funcs)

        self.sel = 0                  # index in self.values
        self.mode = "nav"             # nav | edit | type_pick | add_type_pick | add_edit | exit_ask
        self.buf = ""
        self.cur = 0
        self.commit = False
        self.edit_err = ""
        self.patches: dict = {}       # {absolute byte_off: bytes} - only same-tag/same-width edits
        self.changed_indices: set = set()  # EVERY index touched in the session
        self.edit_tag = None          # tag currently being edited (None = element's original tag)
        self.type_pick_sel = 0

        # Add/remove ONLY at the end, never combined with changed_indices.
        self.add_stage = None          # None | "key" | "value"
        self.add_key_tag = None
        self.add_key_bytes = None
        self.pending_add = False       # True after an add confirmed in the session
        self.pending_remove = False    # True after 'd' in the session

        self.is_shared = bool(is_shared)
        self.shared_with = list(shared_with)

        self.top = 0
        self._max_top = 0

        self.context_bytes = bytes(context_bytes)
        self.context_base = int(context_base)

    @property
    def dirty_count(self) -> int:
        return len(self.changed_indices) + (1 if (self.pending_add or self.pending_remove) else 0)

    @property
    def is_resize(self) -> bool:
        """True for an add/remove session (decides the dispatch in `cli.repl._edit_array`)."""
        return self.pending_add or self.pending_remove

    def cur_value(self):
        return self.values[self.sel] if self.values else None

    def move(self, delta: int):
        if self.values:
            self.sel = max(0, min(len(self.values) - 1, self.sel + delta))

    def ensure_visible(self, view_height: int, header_rows: int):
        row = header_rows + self.sel
        if row < self.top:
            self.top = row
        elif row >= self.top + view_height:
            self.top = row - view_height + 1
        self.top = max(0, min(self.top, self._max_top))

    def begin_edit(self):
        v = self.cur_value()
        if v is None:
            return
        if self.is_resize:
            self.edit_err = "session already has an add/remove - only one edit type per commit"
            return
        # Empty buffer: the element is a scalar, retyped as a whole.
        self.buf = ""
        self.cur = 0
        self.mode = "edit"
        self.edit_tag = None   # None = current element's tag (same-tag fast path)
        self.edit_err = ""

    def cancel_edit(self):
        self.mode = "nav"
        self.buf = ""
        self.edit_tag = None
        self.edit_err = ""

    def begin_add(self):
        """Key `a`: opens the type picker for the new element, first for the KEY
        (string tags only) in an object, then for the VALUE. Refused if the
        session already has element edits or an add/remove."""
        if self.changed_indices:
            self.edit_err = "session already has element edits - only one edit type per commit"
            return
        if self.is_resize:
            self.edit_err = "an add/remove is already queued in this session"
            return
        self.add_stage = "key" if self.keys is not None else "value"
        self.add_key_tag = None
        self.add_key_bytes = None
        self.buf = ""
        self.cur = 0
        default_tag = SLPTag.SHORT_STRING if self.add_stage == "key" else SLPTag.INTEGER
        self.type_pick_sel = _SLP_TAG_ORDER.index(default_tag)
        self.mode = "add_type_pick"
        self.edit_err = ""

    def add_type_pick_confirm(self):
        """Enter in the add-mode picker: sets `edit_tag` and moves to `add_edit`.
        A key must be a string tag (it references an existing string)."""
        tag = _SLP_TAG_ORDER[self.type_pick_sel]
        if self.add_stage == "key" and tag not in _STRING_TAGS:
            self.edit_err = "the key must be a reference to an existing string (string:N)"
            return
        self.edit_tag = tag
        self.buf = ""
        self.cur = 0
        self.mode = "add_edit"
        self.edit_err = ""

    def add_type_pick_cancel(self):
        """Esc in the add-mode picker: cancels the whole add."""
        self.mode = "nav"
        self.add_stage = None
        self.edit_tag = None
        self.edit_err = ""

    def cancel_add_edit(self):
        """Esc in add_edit: back to the picker, the add isn't cancelled."""
        self.mode = "add_type_pick"
        self.edit_err = ""

    def commit_add_field(self):
        """Enter in add_edit: validates `buf` with the chosen tag. A key moves on
        to the VALUE's picker; a value completes the add, appending the element
        (and key) to `values` / `keys` for the preview and setting `pending_add`."""
        try:
            enc = encode_slp_value(self.edit_tag, self.buf, string_count=len(self.strings))
        except SLPError as ex:
            self.edit_err = str(ex)
            return

        if self.add_stage == "key":
            self.add_key_tag = self.edit_tag
            self.add_key_bytes = enc
            self.add_stage = "value"
            self.edit_tag = None
            self.buf = ""
            self.cur = 0
            self.type_pick_sel = _SLP_TAG_ORDER.index(SLPTag.INTEGER)
            self.mode = "add_type_pick"
            self.edit_err = ""
            return

        new_val = SLPValue(self.edit_tag, decode_value_bytes(self.edit_tag, enc), -1, len(enc))
        self.values.append(new_val)
        if self.keys is not None:
            new_key = SLPValue(self.add_key_tag, decode_value_bytes(self.add_key_tag, self.add_key_bytes),
                               -1, len(self.add_key_bytes))
            self.keys.append(new_key)
        self.sel = len(self.values) - 1
        self.pending_add = True
        self.add_stage = None
        self.edit_tag = None
        self.mode = "nav"
        self.edit_err = ""

    def request_remove_last(self):
        """Key `d`: removes the LAST element (or key-value pair) from the preview.
        Refused if empty, or if the session already has other edits."""
        if not self.values:
            self.edit_err = "no elements to remove"
            return
        if self.changed_indices:
            self.edit_err = "session already has element edits - only one edit type per commit"
            return
        if self.pending_add or self.pending_remove:
            self.edit_err = "an add/remove is already queued in this session"
            return
        self.values.pop()
        if self.keys is not None:
            self.keys.pop()
        self.pending_remove = True
        self.sel = max(0, min(self.sel, len(self.values) - 1)) if self.values else 0

    def open_type_pick(self):
        """Ctrl+T in edit mode: the tag picker, preselected on the current tag."""
        v = self.cur_value()
        if v is None:
            return
        cur = self.edit_tag if self.edit_tag is not None else v.tag
        self.type_pick_sel = _SLP_TAG_ORDER.index(cur) if cur in _SLP_TAG_ORDER else 0
        self.mode = "type_pick"

    def type_pick_move(self, delta: int):
        self.type_pick_sel = max(0, min(len(_SLP_TAG_ORDER) - 1, self.type_pick_sel + delta))

    def type_pick_confirm(self):
        """Enter in the picker: `edit_tag` = the selection, back to `edit` with an
        empty buffer (Null/True/False commit with one more Enter, no text)."""
        self.edit_tag = _SLP_TAG_ORDER[self.type_pick_sel]
        self.buf = ""
        self.cur = 0
        self.mode = "edit"
        self.edit_err = ""

    def type_pick_cancel(self):
        self.mode = "edit"

    def commit_element(self):
        """Enter in edit: validates `buf` with the current tag (`edit_tag`, else the
        element's) via `encode_slp_value`. On success it updates the preview,
        marks the index in `changed_indices` and, only if tag and width match
        `orig_values`, records `patches[byte_off] = enc` (same-size splice).
        On error it stays in `edit` with `edit_err`."""
        v = self.cur_value()
        if v is None:
            self.cancel_edit()
            return
        tag = self.edit_tag if self.edit_tag is not None else v.tag
        try:
            enc = encode_slp_value(tag, self.buf, string_count=len(self.strings))
        except SLPError as ex:
            self.edit_err = str(ex)
            return
        orig = self.orig_values[self.sel]
        self.values[self.sel] = v._replace(tag=tag, value=decode_value_bytes(tag, enc),
                                           byte_len=len(enc))
        self.changed_indices.add(self.sel)
        if tag == orig.tag and len(enc) == orig.byte_len:
            self.patches[orig.byte_off] = enc
        else:
            self.patches.pop(orig.byte_off, None)
        self.edit_tag = None
        self.mode = "nav"
        self.edit_err = ""

    def request_exit(self, event):
        if not self.changed_indices and not self.is_resize:
            self.commit = False
            event.app.exit()
            return
        self.mode = "exit_ask"
        event.app.invalidate()

    @property
    def needs_relocation(self) -> bool:
        """True if any edited element changed tag or width, or the site's buffer
        is shared (`is_shared`: always detach). False means every edit stays on
        the same-size path (`apply_array_patch`)."""
        if self.is_shared:
            return True
        return len(self.patches) != len(self.changed_indices)


def run_array_editor(site_idx=0, site=None, values=(), keys=None, strings=(),
                     funcs=(), *, context_bytes=b"", context_base=0,
                     is_shared=False, shared_with=(),
                     _input=None, _output=None, _state=None) -> "ArrayEditState":
    """Full-screen editor for a literal site; returns the state on exit. A buffer
    shared with other sites (`is_shared` / `shared_with`) shows a banner and the
    commit detaches it via relocation. `_input/_output/_state` are for headless tests."""
    state = _state if _state is not None else ArrayEditState(
        site_idx, site, values, keys, strings, funcs,
        context_bytes=context_bytes, context_base=context_base,
        is_shared=is_shared, shared_with=shared_with)

    _HEADER_ROWS = 3   # fixed rows before the element list in _body_rows()

    def _title():
        site = state.site
        fname = (state.funcs[site.function_index].name
                 if 0 <= site.function_index < len(state.funcs) else "?")
        kind = "object" if site.is_object else "array"
        shared_txt = f"   ⚠ shared ×{len(state.shared_with) + 1}" if state.is_shared else ""
        return [(
            "bold",
            f" hermit edit array - site [{state.site_idx}] func[{site.function_index}] "
            f"{fname}  @ {site.instruction_offset:#06x}   {site.mnemonic}   "
            f"{kind}, {len(state.values)} elem"
            f"{'  *' if state.dirty_count else ''}{shared_txt} ",
        )]

    def _body_rows():
        site = state.site
        kind = "object" if site.is_object else "array"
        rows = [
            f"\x1b[2mFunction:\x1b[0m func[{site.function_index}]   "
            f"\x1b[2mOpcode:\x1b[0m {site.mnemonic}",
            f"\x1b[2mType:\x1b[0m {kind}   \x1b[2mElements:\x1b[0m {len(state.values)}",
            "",
        ]
        new_idx = len(state.values) - 1 if state.pending_add else -1
        for i, v in enumerate(state.values):
            vstr = slp_render(v, state.strings)
            text = vstr
            if state.keys is not None:
                kstr = slp_render(state.keys[i], state.strings)
                text = f"{kstr}: {vstr}"
            postxt = "NEW" if i == new_idx else f"@ {v.byte_off:#x}"
            tagtxt = f"\x1b[2m{slp_type_name(v.tag)} {postxt}\x1b[0m"
            if i == state.sel:
                rows.append(f"\x1b[7m  [{i}] {text}\x1b[0m   {tagtxt}")
            else:
                rows.append(f"  [{i}] {text}   {tagtxt}")
        if state.pending_remove:
            rows.append("\x1b[2m  [removed] last element removed - Ctrl+X to apply\x1b[0m")
        if state.keys is not None:
            rows.append("")
            rows.append("\x1b[2m(keys are read-only - only `a`/`d` touch the last pair)\x1b[0m")
        if state.is_shared:
            others = ", ".join(f"array:{j}" for j in state.shared_with)
            rows.append("")
            rows.append(
                f"\x1b[33m⚠ buffer shared with {others}\x1b[0m - editing "
                f"this site \x1b[1mDETACHES\x1b[0m it from the group (detach): "
                f"the other sites remain intact on the old bytes.")
        v = state.cur_value()
        if v is not None and v.byte_off >= 0:
            rows.append("")
            rows.append("\x1b[2mByte view (selected element highlighted):\x1b[0m")
            rows += display.hex_view_rows(
                state.context_bytes, state.context_base, (v.byte_off, v.byte_off + v.byte_len))
        return rows

    def _body():
        vh = view_height()
        rows = _body_rows()
        state._max_top = max(0, len(rows) - vh)
        state.ensure_visible(vh, _HEADER_ROWS)
        return ANSI("\n".join(rows[state.top:state.top + vh]))

    def _status():
        v = state.cur_value()
        if v is None:
            return [(CURSOR_STYLE, " (no element) ")]
        d = state.dirty_count
        return [(
            CURSOR_STYLE,
            f" element {state.sel + 1}/{len(state.values)}   {slp_type_name(v.tag)}"
            f"{f'   {d} modified' if d else ''} ",
        )]

    def _bar1():
        if state.mode == "exit_ask":
            if state.pending_add:
                msg = " new element queued - apply to the in-memory image? "
            elif state.pending_remove:
                msg = " last element removed - apply to the in-memory image? "
            else:
                d = state.dirty_count
                msg = (f" {d} element{'s' if d != 1 else ''} modified - "
                      f"apply to the in-memory image? ")
            return apply_discard_cancel_bar(msg)
        if state.mode in ("type_pick", "add_type_pick"):
            # (style, text) tuples, not ANSI escapes: those only work inside ANSI(...).
            allowed = _STRING_TAGS \
                if (state.mode == "add_type_pick" and state.add_stage == "key") else _SLP_TAG_ORDER
            label = " new type: " if state.mode == "type_pick" else (
                " KEY type: " if state.add_stage == "key" else " VALUE type: ")
            result = [(HINT_STYLE, label)]
            for i, t in enumerate(_SLP_TAG_ORDER):
                if i == state.type_pick_sel:
                    style = CURSOR_STYLE
                elif t not in allowed:
                    style = HINT_STYLE
                else:
                    style = ""
                result.append((style, slp_type_name(t) + " "))
            return result
        if state.mode in ("edit", "add_edit"):
            if state.mode == "edit":
                v = state.cur_value()
                old = slp_render(v, state.strings) if v else ""
                tag = state.edit_tag if state.edit_tag is not None else (v.tag if v else None)
                label = f" new {slp_type_name(tag)} (was {old}): "
            else:
                which = "KEY" if state.add_stage == "key" else "VALUE"
                label = f" new {which} ({slp_type_name(state.edit_tag)}): "
            return input_bar(label, state.buf, state.cur)
        return [(HINT_STYLE,
                 " ↑↓ element   Enter edit   a add   d remove last   "
                 "PgUp/PgDn scrolls   ^X save/exit ")]

    def _bar2():
        if state.edit_err:
            return [(ERR_STYLE, "  " + state.edit_err)]
        if state.mode == "exit_ask":
            return [(HINT_STYLE,
                     "  Y re-encodes and applies to the image (integrity check) · "
                     "the .hbc is written by `patch`")]
        if state.mode in ("type_pick", "add_type_pick"):
            return [(HINT_STYLE, "  <-> choose type - Enter confirms · Esc cancels")]
        if state.mode == "edit":
            return [(HINT_STYLE,
                     "  Enter confirms · Esc cancels · ^T change type (incl. Null/True/False)")]
        if state.mode == "add_edit":
            return [(HINT_STYLE, "  Enter confirms · Esc returns to type")]
        if state.keys is not None:
            return [(HINT_STYLE, "  keys are read-only - only values are editable")]
        return [("", "")]

    kb = KeyBindings()
    in_nav = Condition(lambda: state.mode == "nav")
    in_edit = Condition(lambda: state.mode == "edit")
    in_type_pick = Condition(lambda: state.mode == "type_pick")
    in_ask = Condition(lambda: state.mode == "exit_ask")
    # add_type_pick / add_edit reuse the type_pick / edit navigation; only
    # Enter and Esc dispatch to different methods.
    in_add_type_pick = Condition(lambda: state.mode == "add_type_pick")
    in_add_edit = Condition(lambda: state.mode == "add_edit")
    in_text_entry = Condition(lambda: state.mode in ("edit", "add_edit"))
    in_any_type_pick = Condition(lambda: state.mode in ("type_pick", "add_type_pick"))

    bind_abort(kb, state)

    @kb.add("q", filter=in_nav)
    @kb.add("c-x", filter=in_nav)
    def _q(event):
        state.request_exit(event)

    @kb.add("up", filter=in_nav)
    def _up(event):
        state.move(-1); event.app.invalidate()

    @kb.add("down", filter=in_nav)
    def _down(event):
        state.move(+1); event.app.invalidate()

    @kb.add("pageup", filter=in_nav)
    def _pgup(event):
        state.move(-view_height()); event.app.invalidate()

    @kb.add("pagedown", filter=in_nav)
    def _pgdn(event):
        state.move(+view_height()); event.app.invalidate()

    @kb.add("enter", filter=in_nav)
    def _open_edit(event):
        state.begin_edit(); event.app.invalidate()

    @kb.add("a", filter=in_nav)
    def _add(event):
        state.begin_add(); event.app.invalidate()

    @kb.add("d", filter=in_nav)
    def _remove_last(event):
        state.request_remove_last(); event.app.invalidate()

    @kb.add("escape", filter=in_edit)
    def _cancel(event):
        state.cancel_edit(); event.app.invalidate()

    @kb.add("enter", filter=in_edit)
    def _commit(event):
        state.commit_element(); event.app.invalidate()

    @kb.add("escape", filter=in_add_edit)
    def _cancel_add_edit(event):
        state.cancel_add_edit(); event.app.invalidate()

    @kb.add("enter", filter=in_add_edit)
    def _commit_add_field(event):
        state.commit_add_field(); event.app.invalidate()

    bind_text_input(kb, state, in_text_entry)

    @kb.add("c-t", filter=in_edit)
    def _open_type_pick(event):
        state.open_type_pick(); event.app.invalidate()

    @kb.add("left", filter=in_any_type_pick)
    def _tp_left(event):
        state.type_pick_move(-1); event.app.invalidate()

    @kb.add("right", filter=in_any_type_pick)
    def _tp_right(event):
        state.type_pick_move(+1); event.app.invalidate()

    @kb.add("enter", filter=in_type_pick)
    def _tp_confirm(event):
        state.type_pick_confirm(); event.app.invalidate()

    @kb.add("escape", filter=in_type_pick)
    def _tp_cancel(event):
        state.type_pick_cancel(); event.app.invalidate()

    @kb.add("enter", filter=in_add_type_pick)
    def _atp_confirm(event):
        state.add_type_pick_confirm(); event.app.invalidate()

    @kb.add("escape", filter=in_add_type_pick)
    def _atp_cancel(event):
        state.add_type_pick_cancel(); event.app.invalidate()

    bind_exit_prompt(kb, state, in_ask, cancel_mode="nav")

    run_screen(_title, _body, _status, _bar1, _bar2, kb, _input=_input, _output=_output)
    return state
