"""String editor (`edit string:<idx|value|new>`).

A line editor over one string. The new value is validated against the entry's
encoding (ASCII on 8-bit entries, BMP on UTF-16 ones). Its length is free: the
REPL picks the commit path, an in-place overwrite for an unshared same-length
value, otherwise a relocation (`Module.set_string_value`) that re-points the
entry. `edit string:new` and Ctrl+N switch to "new string" mode, committed with
`Module.add_string`; `edit identifier:new` opens the same mode with kind
Identifier (`Module.add_identifier`). An existing Identifier always goes through
the relocation, which rewrites its hash: editing it renames the property at
every site that references it.
"""

from __future__ import annotations

from prompt_toolkit.filters import Condition
from prompt_toolkit.formatted_text import ANSI
from prompt_toolkit.key_binding import KeyBindings

import _libhbc
from cli import display
from cli.project import IDENTIFIER_OPCODES
from cli.editor.common import (
    CURSOR_STYLE, DIRTY_MARK_STYLE, ERR_STYLE, HINT_STYLE, TextBufferMixin,
    apply_discard_cancel_bar, bind_abort, bind_exit_prompt, bind_text_input, input_bar,
    run_screen, view_height,
)


class _StrEditError(Exception):
    pass


def _encode_for_entry(value: str, entry, *, same_length: bool) -> bytes:
    """`value` encoded for `entry`: ASCII on an 8-bit entry, UTF-16LE (BMP
    only) on a UTF-16 one. With `same_length` the byte length must also match
    the entry's."""
    if value == "":
        raise _StrEditError(
            "empty value - the same-length edit does not delete the string "
            "(length change needs relocation)" if same_length
            else "empty value - the relocation path does not delete the string")

    if getattr(entry, "is_utf16", False):
        for ch in value:
            cp = ord(ch)
            if 0xD800 <= cp <= 0xDFFF:
                raise _StrEditError(
                    "isolated surrogate not allowed (BMP only on same-length edits)"
                    if same_length else "isolated surrogate not allowed (BMP only)")
            if cp > 0xFFFF:
                raise _StrEditError(
                    f"code point U+{cp:04X} outside the BMP (non-BMP / UTF-16 4B needs relocation)"
                    if same_length
                    else f"code point U+{cp:04X} outside the BMP (non-BMP not yet supported)")
        enc = value.encode("utf-16-le")
    else:
        try:
            enc = value.encode("ascii")
        except UnicodeEncodeError:
            raise _StrEditError(
                "non-ASCII character on an ASCII (8-bit) entry - the entry would have to "
                "become UTF-16 (not supported)")

    if same_length and len(enc) != entry.byte_length:
        raise _StrEditError(
            f"{len(enc)} bytes ≠ {entry.byte_length} original bytes "
            f"(length change needs relocation)")
    return enc


def encode_string_value(value: str, entry) -> bytes:
    """Same-length edit: `value` encoded for `entry` (`_libhbc.StringEntry`, or
    anything with `.is_utf16` / `.byte_length`), with the original byte length."""
    return _encode_for_entry(value, entry, same_length=True)


def encode_string_new_value(value: str, entry) -> bytes:
    """Relocation path: same encoding as `encode_string_value`, any length."""
    return _encode_for_entry(value, entry, same_length=False)


def encode_string_commit(state: "StringEditState") -> dict:
    """`{absolute_storage_offset: bytes}` for the edited string, `{}` if
    unchanged. Re-validates encoding and length. An Identifier is refused: the
    in-place overwrite would leave its hash stale (the relocation rewrites it)."""
    if state.value == state.original:
        return {}
    if state.kind == _libhbc.StringKind.Identifier:
        raise _StrEditError(
            "Identifier string: the in-place overwrite leaves its hash stale - "
            "use the relocation path")
    return {state.entry.storage_offset: encode_string_value(state.value, state.entry)}


def encode_string_relocation_commit(state: "StringEditState") -> bytes | None:
    """Relocation path: the encoded bytes, or None if the value is unchanged.
    Choosing between patch and relocation is up to `cli/repl.py._edit_string`,
    which knows `is_shared` and always relocates an Identifier."""
    if state.value == state.original:
        return None
    return encode_string_new_value(state.value, state.entry)


def encode_new_string(value: str) -> bytes:
    """Encoder for `add_string`: kind String, ASCII only, non-empty, any length."""
    if value == "":
        raise _StrEditError("empty string - nothing to add")
    try:
        return value.encode("ascii")
    except UnicodeEncodeError:
        raise _StrEditError(
            "non-ASCII character - new strings are ASCII only (UTF-16 encoding of new "
            "strings not supported)")


def encode_new_identifier(value: str, strings=(), kinds=()) -> bytes:
    """Encoder for `add_identifier`: the `encode_new_string` rules, and no
    Identifier with the same text may exist already (reuse that one instead)."""
    enc = encode_new_string(value)
    for i, (s, k) in enumerate(zip(strings, kinds)):
        if k == _libhbc.StringKind.Identifier and str(s) == value:
            raise _StrEditError(f"identifier already exists: string:{i} - use that index")
    return enc


class StringEditState(TextBufferMixin):
    """State of the string editor. Opens in edit mode.

    `is_add`: "new string" mode, with no entry and an empty buffer. Ctrl+N
    switches to it from an existing string (Esc comes back). `add_kind` is the
    kind of the string to add: `Identifier` for `edit identifier:new`, which
    also refuses a text that already exists as an Identifier.
    `is_shared` / `shared_with`: the entry shares storage with other entries
    (their indices). It still opens, with a banner: editing detaches it from
    the group (the REPL commits through `apply_string_relocation`)."""

    def __init__(self, str_idx, strings, kinds, entries, xrefs, funcs,
                 *, context_bytes=b"", context_base=0, is_add=False,
                 is_shared=False, shared_with=(), add_kind=None):
        self.str_idx = str_idx
        self.add_kind = add_kind if add_kind is not None else _libhbc.StringKind.String
        self._all_strings = list(strings)
        self._all_kinds = list(kinds)
        self.is_add = bool(is_add)
        self.is_shared = bool(is_shared)
        self.shared_with = list(shared_with)
        if self.is_add:
            self.original = ""
            self.entry = None
            self.kind = self.add_kind
            self.xrefs = []
            self.funcs = []
            self.context_bytes = b""
            self.context_base = 0
            self.highlight = None
        else:
            self.original = str(strings[str_idx]) if str_idx < len(strings) else ""
            self.entry = entries[str_idx]
            self.kind = kinds[str_idx] if str_idx < len(kinds) else None
            self.xrefs = list(xrefs)
            self.funcs = list(funcs)
            self.context_bytes = bytes(context_bytes)
            self.context_base = int(context_base)
            off = self.entry.storage_offset
            self.highlight = (off, off + self.entry.byte_length)

        self.value = self.original          # last confirmed value (commit_buf)
        self.buf = self.value               # text line being edited
        self.cur = len(self.buf)
        self.mode = "edit"                  # edit | exit_ask | site_pick
        self.local_site = None              # XRefSite chosen with O: rename that site only
        self.pick_sel = 0
        self.commit = False
        self.edit_err = ""

        self.top = 0
        self._max_top = 0

        self._prev = None                   # snapshot to return from add-mode (Ctrl+N -> Esc)

    def switch_to_add(self):
        """Switches to "new string": saves the current state for `Esc`."""
        if self.is_add:
            return
        self._prev = (self.original, self.value, self.buf, self.cur, self.entry,
                      self.kind, self.xrefs, self.funcs, self.context_bytes,
                      self.context_base, self.highlight, self.is_shared,
                      self.shared_with)
        self.is_add = True
        self.original = ""
        self.value = ""
        self.buf = ""
        self.cur = 0
        self.entry = None
        self.kind = _libhbc.StringKind.String
        self.xrefs = []
        self.funcs = []
        self.context_bytes = b""
        self.context_base = 0
        self.highlight = None
        self.is_shared = False
        self.shared_with = []
        self.edit_err = ""

    def _switch_back(self) -> bool:
        if not self._prev:
            return False
        (self.original, self.value, self.buf, self.cur, self.entry, self.kind,
         self.xrefs, self.funcs, self.context_bytes, self.context_base,
         self.highlight, self.is_shared, self.shared_with) = self._prev
        self._prev = None
        self.is_add = False
        self.edit_err = ""
        return True

    @property
    def adds_identifier(self) -> bool:
        return self.is_add and self.kind == _libhbc.StringKind.Identifier

    @property
    def is_identifier(self) -> bool:
        """Editing an existing Identifier: a rename at every referencing site."""
        return not self.is_add and self.kind == _libhbc.StringKind.Identifier

    @property
    def site_count(self) -> int:
        return sum(len(x.sites) for x in self.xrefs)

    @property
    def operand_sites(self) -> list:
        """Instruction sites that use the entry as a name (GetById, PutById, ...):
        the ones a one-site rename can re-point. Literal keys are renamed only
        globally."""
        return [st for x in self.xrefs for st in x.sites
                if st.kind == _libhbc.XRefKind.Operand and st.mnemonic in IDENTIFIER_OPCODES]

    @property
    def value_sites(self) -> list:
        """Sites that use the entry as a string VALUE: LoadConstString, array
        elements, object values. Hermes stores each text once, so a property name
        and a string literal with the same text share the Identifier entry, and a
        global rename changes these values too."""
        return [st for x in self.xrefs for st in x.sites
                if st.kind in (_libhbc.XRefKind.ArrayElement, _libhbc.XRefKind.ObjectValue)
                or (st.kind == _libhbc.XRefKind.Operand
                    and st.mnemonic not in IDENTIFIER_OPCODES)]

    def start_site_pick(self) -> bool:
        if not self.operand_sites:
            self.edit_err = "no instruction site to rename alone - only the global rename"
            return False
        self.mode = "site_pick"
        self.pick_sel = 0
        self.edit_err = ""
        return True

    def move_pick(self, delta: int):
        n = len(self.operand_sites)
        if n:
            self.pick_sel = max(0, min(n - 1, self.pick_sel + delta))

    def merges_with(self):
        """Index of another Identifier whose text equals the current value (the
        renamed name and that one become the same property), else None."""
        if not self.is_identifier:
            return None
        for i, (s, k) in enumerate(zip(self._all_strings, self._all_kinds)):
            if i != self.str_idx and k == _libhbc.StringKind.Identifier and str(s) == self.value:
                return i
        return None

    def _encode_new(self, value: str) -> bytes:
        if self.adds_identifier:
            return encode_new_identifier(value, self._all_strings, self._all_kinds)
        return encode_new_string(value)

    @property
    def modified(self) -> bool:
        return self.value != self.original

    @property
    def byte_length(self) -> int:
        if self.entry is None:
            return len(self.value.encode("ascii", errors="replace"))
        return self.entry.byte_length

    @property
    def char_length(self) -> int:
        if self.entry is None:
            return len(self.value)
        return self.entry.char_length

    @property
    def is_utf16(self) -> bool:
        return bool(self.entry.is_utf16) if self.entry is not None else False

    def commit_buf(self) -> bool:
        """Validates `buf` (`encode_new_string` in add mode, else
        `encode_string_new_value`). On success `value = buf`; on error sets `edit_err`."""
        try:
            if self.is_add:
                self._encode_new(self.buf)
            else:
                encode_string_new_value(self.buf, self.entry)
        except _StrEditError as ex:
            self.edit_err = str(ex)
            return False
        self.value = self.buf
        self.edit_err = ""
        return True

    def reset_buf(self):
        """Esc: leaves add mode if it was entered with Ctrl+N, else restores the original."""
        if self.is_add and self._switch_back():
            return
        self.buf = self.original
        self.cur = len(self.buf)
        self.value = self.original
        self.edit_err = ""

    def scroll(self, delta: int):
        self.top = max(0, min(self._max_top, self.top + delta))

    def buf_byte_len(self):
        try:
            if self.is_utf16:
                return len(self.buf.encode("utf-16-le"))
            return len(self.buf.encode("ascii"))
        except UnicodeEncodeError:
            return None

    def buf_ok(self) -> bool:
        try:
            if self.is_add:
                self._encode_new(self.buf)
            else:
                encode_string_new_value(self.buf, self.entry)
            return True
        except _StrEditError:
            return False

    def request_exit(self, event):
        # An unchanged buffer always exits, even if `original` can't be
        # re-encoded (e.g. a UTF-16 entry holding an isolated surrogate): nothing to save.
        if self.buf == self.original:
            self.value = self.original
            self.commit = False
            event.app.exit()
            return
        if not self.commit_buf():
            event.app.invalidate()
            return
        if self.value == self.original:
            self.commit = False
            event.app.exit()
            return
        self.mode = "exit_ask"
        event.app.invalidate()



def run_string_editor(str_idx=0, strings=(), kinds=(), entries=(), xrefs=(),
                      funcs=(), *, context_bytes=b"", context_base=0, add=False,
                      is_shared=False, shared_with=(), add_kind=None,
                      _input=None, _output=None, _state=None) -> "StringEditState":
    """Full-screen string editor; returns the state on exit. `add=True` opens
    "new string" mode, of kind `add_kind` (String if None).
    `_input/_output/_state` are for headless tests."""
    state = _state if _state is not None else StringEditState(
        str_idx, strings, kinds, entries, xrefs, funcs,
        context_bytes=context_bytes, context_base=context_base, is_add=add,
        is_shared=is_shared, shared_with=shared_with, add_kind=add_kind)

    def _what():
        return "identifier" if state.adds_identifier else "string"

    def _title():
        if state.is_add:
            kind = "Identifier" if state.adds_identifier else "String"
            return [("bold",
                     f" hermit - new {_what()}   {state.char_length} char / "
                     f"{state.byte_length} B   kind {kind}   8-bit ASCII ")]
        o = display.ellipsize(display.escape_ws(state.original), 40)
        return [(
            "bold",
            f' hermit edit string - [{state.str_idx}] "{o}"   '
            f"{state.char_length} char / {state.byte_length} B   "
            f"UTF16={1 if state.is_utf16 else 0}"
            f"{'  overflow' if state.entry.is_overflow else ''}"
            f"{f'  ⚠ shared ×{len(state.shared_with) + 1}' if state.is_shared else ''}"
            f"{'  *' if state.modified else ''} ",
        )]

    def _body_rows():
        if state.adds_identifier:
            return [
                "\x1b[2mNew identifier (property, global or variable name) to add to "
                "the string table.\x1b[0m",
                "",
                "\x1b[2mKind:\x1b[0m      Identifier  \x1b[2m(its hash is appended to the "
                "Identifier Hash Table)\x1b[0m",
                f"\x1b[2mLength:\x1b[0m    {state.char_length} chars · "
                f"{state.byte_length} bytes  \x1b[2m(free)\x1b[0m",
                "\x1b[2mEncoding:\x1b[0m  8-bit ASCII  \x1b[2m(non-ASCII not yet supported)\x1b[0m",
                "",
                "\x1b[2mAppended to the end of the string storage: no existing index\x1b[0m",
                "\x1b[2mchanges. Use it later as the name of a GetById / PutById /\x1b[0m",
                "\x1b[2mTryGetById / DelById / ... with `edit func:`.\x1b[0m",
            ]
        if state.is_add:
            return [
                "\x1b[2mNew string to add to the string table.\x1b[0m",
                "",
                "\x1b[2mKind:\x1b[0m      String  \x1b[2m(for a property or global name: `edit identifier:new`)\x1b[0m",
                f"\x1b[2mLength:\x1b[0m    {state.char_length} chars · "
                f"{state.byte_length} bytes  \x1b[2m(free)\x1b[0m",
                "\x1b[2mEncoding:\x1b[0m  8-bit ASCII  \x1b[2m(non-ASCII not yet supported)\x1b[0m",
                "",
                "\x1b[2mAppended to the end of the string storage: no existing index\x1b[0m",
                "\x1b[2mchanges. Use it later in a LoadConstString with `edit func:`.\x1b[0m",
            ]
        e = state.entry
        kind_name = ("Identifier" if state.kind == _libhbc.StringKind.Identifier
                     else "String")
        rows = []
        if state.is_identifier:
            n = state.site_count
            rows.append(
                f"\x1b[33m⚠ Identifier: renaming it renames the property at all {n} "
                f"site{'' if n == 1 else 's'} below\x1b[0m \x1b[2m(its hash is rewritten)\x1b[0m")
            nv = len(state.value_sites)
            if nv:
                rows.append(
                    f"\x1b[33m⚠ {nv} of them use the same text as a string value "
                    f"(LoadConstString / array element / object value): the global "
                    f"rename changes {'it' if nv == 1 else 'them'} too\x1b[0m")
            rows.append("")
        rows += [
            f"\x1b[2mKind:\x1b[0m      {kind_name}",
            f"\x1b[2mLength:\x1b[0m    {state.char_length} chars · "
            f"{state.byte_length} bytes  \x1b[2m(length is free - a value of "
            f"different length re-points the entry instead of overwriting it)\x1b[0m",
            f"\x1b[2mEncoding:\x1b[0m  "
            f"{'UTF-16LE' if state.is_utf16 else '8-bit ASCII'}"
            f"{'  overflow' if e.is_overflow else ''}",
        ]
        if state.is_shared:
            others = ", ".join(f"string:{j}" for j in state.shared_with)
            rows.append(
                f"\x1b[33m⚠ storage shared with {others}\x1b[0m - "
                f"this edit \x1b[1mDETACHES\x1b[0m it from the group (detach): "
                f"the others remain intact on the old bytes.")
        rows.append("")
        rows.append(f"\x1b[2mByte view (storage @ {e.storage_offset:#x}):\x1b[0m")
        rows += display.hex_view_rows(state.context_bytes, state.context_base,
                                      state.highlight)
        rows.append("")
        n = sum(len(x.sites) for x in state.xrefs)
        rows.append(f"\x1b[2mReferences ({n} sit{'e' if n == 1 else 'es'}):\x1b[0m")
        xr = display.xrefs_detail_to_ansi_rows(state.xrefs, state.funcs)
        rows += xr if xr else ["  \x1b[2m(no references)\x1b[0m"]
        return rows

    def _pick_rows():
        sites = state.operand_sites
        n_lit = state.site_count - len(sites)
        rows = [
            f"\x1b[1mRename one site only\x1b[0m: the chosen instruction will name "
            f"\x1b[32m\"{display.escape_ws(state.value)}\"\x1b[0m, every other site keeps "
            f"\x1b[32m\"{display.escape_ws(state.original)}\"\x1b[0m.",
            "\x1b[2mThe new name is added as an Identifier (or the existing one is reused), "
            "then the operand is re-pointed; the opcode is widened if the index needs it.\x1b[0m",
        ]
        if n_lit:
            rows.append(f"\x1b[2m{n_lit} other site{'' if n_lit == 1 else 's'} (literal keys, "
                        f"string values): global rename only, not listed.\x1b[0m")
        rows.append("")
        for i, st in enumerate(sites):
            fname = display._func_name(state.funcs, st.function_index)
            mark = "\x1b[7m>" if i == state.pick_sel else " "
            rows.append(f"{mark} [{st.function_index}] {fname}  @ {st.instruction_offset:#06x}  "
                        f"({st.mnemonic})\x1b[0m")
        return rows

    def _body():
        vh = view_height()
        if state.mode == "site_pick":
            rows = _pick_rows()
            sel_row = 4 + state.pick_sel
            if sel_row < state.top:
                state.top = sel_row
            elif sel_row >= state.top + vh:
                state.top = sel_row - vh + 1
            state._max_top = max(0, len(rows) - vh)
            state.top = max(0, min(state.top, state._max_top))
            return ANSI("\n".join(rows[state.top:state.top + vh]))
        rows = _body_rows()
        state._max_top = max(0, len(rows) - vh)
        state.top = max(0, min(state.top, state._max_top))
        return ANSI("\n".join(rows[state.top:state.top + vh]))

    def _status():
        if state.is_add:
            kind = "Identifier" if state.adds_identifier else "String"
            return [(CURSOR_STYLE,
                     f" new {_what()} (kind {kind})   {state.byte_length} B   "
                     f"{'ready' if state.buf_ok() else 'incomplete'} ")]
        return [(
            CURSOR_STYLE,
            f" off {state.entry.storage_offset:#x}   {state.byte_length} B original   "
            f"{'MODIFIED' if state.modified else 'unchanged'} ",
        )]

    def _bar1():
        if state.mode == "site_pick":
            return [(HINT_STYLE, " pick the site to rename:  "),
                    ("bold", " ↑↓ "), (HINT_STYLE, "move  "),
                    ("bold", " Enter "), (HINT_STYLE, "rename this site only  "),
                    ("bold", " Esc "), (HINT_STYLE, "back ")]
        if state.mode == "exit_ask" and state.is_identifier:
            n = state.site_count
            return [
                (DIRTY_MARK_STYLE, f" rename the identifier at all {n} site{'' if n == 1 else 's'}? "),
                ("bold", " Y "), (HINT_STYLE, "all sites  "),
                ("bold", " O "), (HINT_STYLE, "one site  "),
                ("bold", " N "), (HINT_STYLE, "discard  "),
                ("bold", " Esc "), (HINT_STYLE, "cancel "),
            ]
        if state.mode == "exit_ask":
            if state.is_add:
                prompt = f" new {_what()} - add it to the string table? "
            elif state.is_identifier:
                n = state.site_count
                prompt = f" rename the identifier at all {n} site{'' if n == 1 else 's'}? "
            else:
                prompt = " string modified - apply to the in-memory image? "
            return apply_discard_cancel_bar(prompt)
        n = state.buf_byte_len()
        ok = state.buf_ok()   # no length constraint, only the encoding
        if state.is_add:
            tag = f"   [{'?' if n is None else n} B {'OK' if ok else 'error'}]"
        else:
            tag = f"   [{'?' if n is None else n}/{state.byte_length} B {'OK' if ok else 'error'}]"
        return input_bar(" new value: ", state.buf, state.cur) + [
            (HINT_STYLE if ok else ERR_STYLE, tag)]

    def _bar2():
        if state.edit_err:
            return [(ERR_STYLE, "  " + state.edit_err)]
        if state.mode == "site_pick":
            return [(HINT_STYLE, "  two commits: add the identifier, then re-point the "
                                 "operand · shared bytecode is refused (bonus B12)")]
        if state.mode == "exit_ask":
            if state.is_add:
                return [(HINT_STYLE,
                         f"  Y adds the {_what()} and rebuilds the file in memory · "
                         "the .hbc is written by `patch`")]
            if state.is_identifier:
                other = state.merges_with()
                if other is not None:
                    return [(ERR_STYLE,
                             f"  ⚠ string:{other} has the same name: the two become the same "
                             f"property · Y renames anyway")]
                return [(HINT_STYLE,
                         "  Y renames at every site and rewrites the hash · "
                         "the .hbc is written by `patch`")]
            return [(HINT_STYLE,
                     "  Y re-encodes and applies to the image (integrity check) · "
                     "the .hbc is written by `patch`")]
        if state.is_add:
            return [(HINT_STYLE,
                     "  <-> move · Home/End · BS/Del · Esc cancels · "
                     "^X add/exit   (ASCII only)")]
        hint = "BMP only (UTF-16LE)" if state.is_utf16 else "ASCII only (8-bit entry)"
        return [(HINT_STYLE,
                 f"  <-> move · Home/End · BS/Del · Esc restores · ^N new · "
                 f"PgUp/PgDn scrolls · ^X save/exit   ({hint})")]

    kb = KeyBindings()
    in_edit = Condition(lambda: state.mode == "edit")
    in_ask = Condition(lambda: state.mode == "exit_ask")
    not_add = Condition(lambda: state.mode == "edit" and not state.is_add)

    bind_abort(kb, state)

    @kb.add("c-n", filter=not_add)
    def _new(event):
        state.switch_to_add()
        event.app.invalidate()

    @kb.add("c-x", filter=in_edit)
    def _cx(event):
        state.request_exit(event)

    @kb.add("escape", filter=in_edit)
    def _esc(event):
        state.reset_buf(); event.app.invalidate()

    bind_text_input(kb, state, in_edit)

    @kb.add("pageup", filter=in_edit)
    def _pgup(event):
        state.scroll(-view_height()); event.app.invalidate()

    @kb.add("pagedown", filter=in_edit)
    def _pgdn(event):
        state.scroll(+view_height()); event.app.invalidate()

    bind_exit_prompt(kb, state, in_ask, cancel_mode="edit")

    ask_identifier = Condition(lambda: state.mode == "exit_ask" and state.is_identifier)
    in_pick = Condition(lambda: state.mode == "site_pick")

    @kb.add("o", filter=ask_identifier)
    @kb.add("O", filter=ask_identifier)
    def _one_site(event):
        state.start_site_pick()
        event.app.invalidate()

    @kb.add("up", filter=in_pick)
    def _pick_up(event):
        state.move_pick(-1); event.app.invalidate()

    @kb.add("down", filter=in_pick)
    def _pick_down(event):
        state.move_pick(+1); event.app.invalidate()

    @kb.add("enter", filter=in_pick)
    def _pick(event):
        state.local_site = state.operand_sites[state.pick_sel]
        state.commit = True
        event.app.exit()

    @kb.add("escape", filter=in_pick)
    def _pick_back(event):
        state.mode = "exit_ask"
        event.app.invalidate()

    run_screen(_title, _body, _status, _bar1, _bar2, kb, _input=_input, _output=_output)
    return state
