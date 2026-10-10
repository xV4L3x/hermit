"""Decoder/encoder for the SerializedLiteral (SLP) format of the array/object
literal buffers (`array buffer`, `object key buffer`, `object value buffer`),
read by NewArrayWithBuffer[Long] / NewObjectWithBuffer[Long].

Typed RLE runs (verified against hermes-dec and `hermes -dump-bytecode`):

    tag byte = [ext:1][tag:3][len:4]
    ext == 0  ->  len = tag_byte & 0x0F                      (run <= 15)
    ext == 1  ->  len = ((tag_byte & 0x0F) << 8) | next_byte  (run <= 4095, tag on 2 bytes)

    tag: 0 Null, 1 True, 2 False, 3 Number, 4 LongString,
         5 ShortString, 6 ByteString, 7 Integer
    Each element follows as little-endian value bytes:
        Null/True/False -> 0 bytes
        ByteString      -> 1 byte  (string index, uint8)
        ShortString     -> 2 bytes (string index, uint16)
        Integer         -> 4 bytes (int32, SIGNED)
        LongString      -> 4 bytes (string index, uint32)
        Number          -> 8 bytes (double)

Example: `var a=[10,20,30]` (`-g0`) gives the array buffer
`73 0a000000 14000000 1e000000` (tag Integer, run 3, three int32). A value that
doesn't fit int32 (e.g. 3000000000) is promoted by Hermes to the Number tag, a
different run with different bytes: that is why the same-size path rejects a
value that would change the tag (see `encode_slp_element`).
"""

from __future__ import annotations

import struct
from collections import namedtuple
from enum import IntEnum


class SLPTag(IntEnum):
    NULL = 0
    TRUE = 1
    FALSE = 2
    NUMBER = 3
    LONG_STRING = 4
    SHORT_STRING = 5
    BYTE_STRING = 6
    INTEGER = 7


_STRING_TAGS = (SLPTag.LONG_STRING, SLPTag.SHORT_STRING, SLPTag.BYTE_STRING)
_ZERO_WIDTH_TAGS = (SLPTag.NULL, SLPTag.TRUE, SLPTag.FALSE)

_VALUE_WIDTH = {
    SLPTag.NULL: 0, SLPTag.TRUE: 0, SLPTag.FALSE: 0,
    SLPTag.NUMBER: 8, SLPTag.LONG_STRING: 4, SLPTag.SHORT_STRING: 2,
    SLPTag.BYTE_STRING: 1, SLPTag.INTEGER: 4,
}

# A decoded element. `byte_off`/`byte_len` are ABSOLUTE within the buffer
# passed to `decode_slp`.
SLPValue = namedtuple("SLPValue", "tag value byte_off byte_len")


class SLPError(Exception):
    pass


def decode_slp(data: bytes, start: int, count: int) -> list[SLPValue]:
    """Decode `count` elements of `data` (a whole buffer) starting at `start`.
    Raises `SLPError` if the buffer ends first (truncated, or a wrong offset)."""
    out: list[SLPValue] = []
    pos = start
    n = len(data)
    while len(out) < count:
        if pos >= n:
            raise SLPError(
                f"SLP buffer truncated at offset {pos:#x} "
                f"(expected {count} elements, read {len(out)})")
        tag_byte = data[pos]
        ext = tag_byte >> 7
        tag = SLPTag((tag_byte >> 4) & 0x7)
        if ext:
            if pos + 1 >= n:
                raise SLPError(f"extended SLP tag truncated at offset {pos:#x}")
            run_len = ((tag_byte & 0x0F) << 8) | data[pos + 1]
            pos += 2
        else:
            run_len = tag_byte & 0x0F
            pos += 1

        width = _VALUE_WIDTH[tag]
        for _ in range(run_len):
            if len(out) >= count:
                break
            byte_off = pos
            if pos + width > n:
                raise SLPError(f"SLP value truncated at offset {pos:#x}")
            raw = data[pos:pos + width]
            if tag == SLPTag.NUMBER:
                value = struct.unpack("<d", raw)[0]
            elif width:
                value = int.from_bytes(raw, "little")
                if tag == SLPTag.INTEGER:
                    value = struct.unpack("<i", raw)[0]   # signed
            else:
                value = None
            out.append(SLPValue(tag, value, byte_off, width))
            pos += width
    return out


def _encode_text(tag: SLPTag, s: str, string_count: int, *, same_size: bool) -> bytes:
    """Value bytes for `tag` from the (stripped, non-empty) text `s`.
    `same_size` only changes the Integer error text: it explains why a decimal
    point or a large value would change the tag."""
    if tag == SLPTag.INTEGER:
        try:
            n = int(s, 0)
        except ValueError:
            raise SLPError(
                "not an integer (decimal or 0x…) - a value with a decimal "
                "point would change the tag to Number" if same_size
                else "not an integer (decimal or 0x…)")
        if not (-(2**31) <= n <= 2**31 - 1):
            raise SLPError(
                "Integer out of range (int32: -2147483648..2147483647) - a "
                "larger value would promote the tag to Number" if same_size
                else "Integer out of range (int32: -2147483648..2147483647)")
        return struct.pack("<i", n)

    if tag == SLPTag.NUMBER:
        try:
            return struct.pack("<d", float(s))
        except ValueError:
            raise SLPError("not a number")

    if tag in _STRING_TAGS:
        try:
            idx = int(s, 0)
        except ValueError:
            raise SLPError("string index is not an integer (decimal or 0x…)")
        if not (0 <= idx < string_count):
            raise SLPError(f"string index out of range (0–{string_count - 1})")
        width = _VALUE_WIDTH[tag]
        if idx >= (1 << (width * 8)):
            raise SLPError(
                f"index {idx} doesn't fit in {tag.name} ({width * 8} bits)")
        return idx.to_bytes(width, "little")

    raise SLPError(f"unknown SLP tag: {tag!r}")


def encode_slp_element(tag: SLPTag, raw: str, *, string_count: int = 0) -> bytes:
    """Encode ONE element keeping the same `tag`, the only same-size case.
    `raw` is the text typed by the user (decimal or `0x…`); `string_count` is
    `len(strings())`, for the range check on string tags.

    Raises `SLPError` for: Null/True/False (toggling changes the tag byte, a
    relocation), a value not valid for the tag (a decimal point on Integer
    would change the tag to Number), Integer out of int32 range, string index
    out of range or too wide for the tag.
    """
    s = raw.strip()
    if not s:
        raise SLPError("empty value")

    if tag in _ZERO_WIDTH_TAGS:
        raise SLPError(
            f"element {tag.name} - Null/True/False toggle needs a relocation "
            f"(would change the tag byte, not just the value)")

    return _encode_text(tag, s, string_count, same_size=True)


def encode_slp_value(tag: SLPTag, raw: str, *, string_count: int = 0) -> bytes:
    """Like `encode_slp_element`, but for a tag the caller chose explicitly
    (e.g. from the editor's picker), so it may differ from the original:
    Null/True/False give `b""` (the tag itself is the value, `raw` is ignored).

    Raises `SLPError` for a value not valid for the tag, Integer out of int32
    range, string index out of range or too wide for the tag.
    """
    if tag in _ZERO_WIDTH_TAGS:
        return b""

    s = raw.strip()
    if not s:
        raise SLPError("empty value")

    return _encode_text(tag, s, string_count, same_size=False)


def reencode_value_bytes(tag: SLPTag, value) -> bytes:
    """Value bytes (no tag byte) of an already decoded element, to re-append an
    element the edit didn't touch. Exact: doubles round-trip through IEEE 754."""
    if tag in _ZERO_WIDTH_TAGS:
        return b""
    if tag == SLPTag.NUMBER:
        return struct.pack("<d", value)
    if tag == SLPTag.INTEGER:
        return struct.pack("<i", value)
    width = _VALUE_WIDTH[tag]
    return int(value).to_bytes(width, "little")


def encode_slp_sequence(items: "list[tuple[SLPTag, bytes]]") -> bytes:
    """Greedy RLE of a whole element sequence, the inverse of `decode_slp`
    (runs up to 4095 with the ext bit, else 15). It needn't match Hermes's
    original bytes: the regenerated site goes to a new region of the buffer."""
    out = bytearray()
    i = 0
    n = len(items)
    while i < n:
        tag = items[i][0]
        j = i
        while j < n and items[j][0] == tag and (j - i) < 4095:
            j += 1
        run_len = j - i
        if run_len <= 15:
            out.append((tag.value << 4) | run_len)
        else:
            out.append(0x80 | (tag.value << 4) | (run_len >> 8))
            out.append(run_len & 0xFF)
        for k in range(i, j):
            out.extend(items[k][1])
        i = j
    return bytes(out)


def rebuild_site_bytes(elements: "list[SLPValue]", edit_index: int,
                       new_tag: SLPTag, new_value_bytes: bytes) -> bytes:
    """COMPLETE payload of a site: `elements` re-encoded, with only
    `elements[edit_index]` replaced by `(new_tag, new_value_bytes)`."""
    items: list[tuple[SLPTag, bytes]] = []
    for i, item in enumerate(elements):
        if i == edit_index:
            items.append((new_tag, new_value_bytes))
        else:
            items.append((item.tag, reencode_value_bytes(item.tag, item.value)))
    return encode_slp_sequence(items)


def decode_value_bytes(tag: SLPTag, raw: bytes):
    """Decode one element's value bytes (no tag byte)."""
    if tag in _ZERO_WIDTH_TAGS:
        return None
    if tag == SLPTag.NUMBER:
        return struct.unpack("<d", raw)[0]
    if tag == SLPTag.INTEGER:
        return struct.unpack("<i", raw)[0]
    return int.from_bytes(raw, "little")


_TYPE_NAMES = {
    SLPTag.NULL: "Null", SLPTag.TRUE: "True", SLPTag.FALSE: "False",
    SLPTag.NUMBER: "Number", SLPTag.LONG_STRING: "LongString",
    SLPTag.SHORT_STRING: "ShortString", SLPTag.BYTE_STRING: "ByteString",
    SLPTag.INTEGER: "Integer",
}


def slp_type_name(tag: SLPTag) -> str:
    return _TYPE_NAMES.get(tag, f"tag{int(tag)}")


def slp_render(item: SLPValue, strings: list) -> str:
    """Human-readable form of an element, for the UI."""
    if item.tag == SLPTag.NULL:
        return "null"
    if item.tag == SLPTag.TRUE:
        return "true"
    if item.tag == SLPTag.FALSE:
        return "false"
    if item.tag == SLPTag.NUMBER:
        return repr(item.value)
    if item.tag == SLPTag.INTEGER:
        return str(item.value)
    if item.tag in _STRING_TAGS:
        if 0 <= item.value < len(strings):
            s = str(strings[item.value])
            if len(s) > 24:
                s = s[:21] + "..."
            return repr(s)
        return f"#{item.value}"
    return str(item.value)
