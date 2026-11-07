"""One-line preview of the constant an instruction loads from a table, for the
disassembly view: the array/object literal of New{Array,Object}WithBuffer[Long],
or the value of LoadConstBigInt[LongIndex].

    ["loca", "lho", "st"] [array][4]
    {kx: "vx", ky: "vy", kz: "vz"} [object][0, 0]
    123456789012345678901234567890n [bigint][10]

The id inside the second bracket is the byte offset in the literal buffer(s):
the operand of the instruction, so two instructions with the same id build their
literal from the same buffer segment. An object shows the key buffer offset, then
the value buffer offset.

The id of a BigInt is its index in the BigInt Table.

A literal preview is cut at `MAX_BODY` characters: elements that do not fit are
replaced by `… +N`. A BigInt of more than `MAX_DIGITS` digits shows its first digits
and `… +N digits` (no trailing `n`, the value is not complete). Everything here is
display only; decoding is `cli.literals.decode_slp` and `HBCFile.bigints()`.
"""

from __future__ import annotations

import re

from cli.literals import SLPError, SLPTag, decode_slp

MAX_BODY = 40      # characters of the preview, brackets and ellipsis included
MAX_STRING = 12    # characters of one string element before it is cut
MAX_DIGITS = 24    # digits of a BigInt before it is cut

_BIGINT = ("LoadConstBigInt", "LoadConstBigIntLongIndex")
_ARRAY = ("NewArrayWithBuffer", "NewArrayWithBufferLong")
_OBJECT = ("NewObjectWithBuffer", "NewObjectWithBufferLong")
_STRING_TAGS = (SLPTag.LONG_STRING, SLPTag.SHORT_STRING, SLPTag.BYTE_STRING)
_IDENT = re.compile(r"^[A-Za-z_$][A-Za-z0-9_$]*$")

_GREEN, _YELLOW, _DIM, _RESET = "\x1b[32m", "\x1b[33m", "\x1b[2m", "\x1b[0m"


def is_literal_instruction(instr) -> bool:
    return instr.mnemonic in _ARRAY or instr.mnemonic in _OBJECT or instr.mnemonic in _BIGINT


def _escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def _number(x: float) -> str:
    return str(int(x)) if float(x).is_integer() and abs(x) < 1e15 else repr(x)


class LiteralPreview:
    """Builds previews for one opened HBC file. Buffers and the string table are
    read on first use, so a disassembly without literals costs nothing."""

    def __init__(self, hbc):
        self._hbc = hbc
        self._bufs = None
        self._strings = None

    def _load(self):
        if self._bufs is None:
            self._bufs = (self._hbc.array_buffer(),
                          self._hbc.obj_key_buffer(),
                          self._hbc.obj_value_buffer())
            self._strings = self._hbc.strings()

    # (plain, ansi) of one decoded element
    def _element(self, e) -> tuple[str, str]:
        if e.tag in _STRING_TAGS:
            s = self._strings[e.value] if e.value < len(self._strings) else f"<str{e.value}>"
            s = _escape(s)
            if len(s) > MAX_STRING:
                s = s[:MAX_STRING - 1] + "…"
            plain = f'"{s}"'
            return plain, f"{_GREEN}{plain}{_RESET}"
        if e.tag == SLPTag.NULL:   plain = "null"
        elif e.tag == SLPTag.TRUE:  plain = "true"
        elif e.tag == SLPTag.FALSE: plain = "false"
        elif e.tag == SLPTag.NUMBER: plain = _number(e.value)
        else:                        plain = str(e.value)          # Integer
        return plain, f"{_YELLOW}{plain}{_RESET}"

    def _key(self, e) -> tuple[str, str]:
        """An object key: a bare name when it is a valid identifier."""
        if e.tag in _STRING_TAGS and e.value < len(self._strings):
            s = self._strings[e.value]
            if _IDENT.fullmatch(s) and len(s) <= MAX_STRING:
                return s, s
        return self._element(e)

    @staticmethod
    def _join(items: list[tuple[str, str]], open_: str, close: str) -> str:
        """`open_ item, item, … +N close` within MAX_BODY characters (always at
        least one item)."""
        used = len(open_) + len(close)
        shown: list[str] = []
        for i, (plain, ansi) in enumerate(items):
            rest = len(items) - i - 1
            tail = len(f", … +{rest}") if rest else 0
            sep = 2 if shown else 0
            if shown and used + sep + len(plain) + tail > MAX_BODY:
                break
            shown.append(ansi)
            used += sep + len(plain)
        hidden = len(items) - len(shown)
        body = ", ".join(shown)
        if hidden:
            body += f", … +{hidden}"
        return open_ + body + close

    def _render_bigint(self, instr) -> str | None:
        ops = instr.operands
        if len(ops) < 2:
            return None
        idx = ops[1].u32
        bigints = self._hbc.bigints()
        tag = f"{_DIM}[bigint][{idx}]{_RESET}"
        if idx >= len(bigints) or not bigints[idx].ok:
            return f"{_DIM}<unreadable bigint>{_RESET} {tag}"
        dec = bigints[idx].decimal
        sign, digits = ("-", dec[1:]) if dec.startswith("-") else ("", dec)
        if len(digits) > MAX_DIGITS:
            body = f"{sign}{digits[:MAX_DIGITS]}… +{len(digits) - MAX_DIGITS} digits"
        else:
            body = f"{dec}n"
        return f"{_YELLOW}{body}{_RESET} {tag}"

    def render(self, instr) -> str | None:
        """ANSI text `<preview> [array][id]` / `<preview> [object][keyId, valueId]` /
        `<value>n [bigint][id]`, or None if `instr` loads no such constant."""
        if not is_literal_instruction(instr):
            return None
        if instr.mnemonic in _BIGINT:
            return self._render_bigint(instr)
        ops = instr.operands
        is_obj = instr.mnemonic in _OBJECT
        if len(ops) < (5 if is_obj else 4):
            return None
        count = ops[2].u32
        self._load()
        arr, keys, vals = self._bufs
        try:
            if is_obj:
                ks = decode_slp(keys, ops[3].u32, count)
                vs = decode_slp(vals, ops[4].u32, count)
                items = []
                for k, v in zip(ks, vs):
                    kp, ka = self._key(k)
                    vp, va = self._element(v)
                    items.append((f"{kp}: {vp}", f"{ka}: {va}"))
                body = self._join(items, "{", "}") if items else "{}"
                tag = f"[object][{ops[3].u32}, {ops[4].u32}]"
            else:
                es = decode_slp(arr, ops[3].u32, count)
                items = [self._element(e) for e in es]
                body = self._join(items, "[", "]") if items else "[]"
                tag = f"[array][{ops[3].u32}]"
        except SLPError:
            body = f"{_DIM}<unreadable literal>{_RESET}"
            tag = f"[object][{ops[3].u32}, {ops[4].u32}]" if is_obj else f"[array][{ops[3].u32}]"
        return f"{body} {_DIM}{tag}{_RESET}"
