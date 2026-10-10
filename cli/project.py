"""HermitProject - in-memory HBC analysis session."""

import hashlib
import os
from typing import Optional

import _libhbc
from cli.literals import decode_slp

_FOOTER = 20  # SHA-1 footer, the last 20 bytes of the file


def _entry_key(e) -> tuple:
    return (e.storage_offset, e.byte_length, e.char_length,
            e.is_utf16, e.is_overflow, e.kind)


def _site_id(s) -> tuple:
    return (s.function_index, s.instruction_offset, s.mnemonic, s.is_object)


def _site_full(s) -> tuple:
    return _site_id(s) + (s.count, s.buffer_idx, s.key_buffer_idx)


def _decode_site(buf: bytes, start: int, count: int) -> list:
    return decode_slp(buf, start, count) if count else []


def _tag_values(elems) -> list:
    return [(e.tag, e.value) for e in elems]


def _check_index(idx: int, n: int, what: str) -> None:
    if not (0 <= idx < n):
        raise ValueError(f"{what} {idx} out of range (0–{n - 1})")


def _parse_image(image, what: str) -> "_libhbc.HBCFile":
    try:
        return _libhbc.HBCFile.from_bytes(bytes(image))
    except Exception as exc:
        raise ValueError(f"invalid image with {what}: {exc}") from exc


def _check_file_length(h, size: int) -> None:
    if h.file_length != size:
        raise ValueError(f"file_length header {h.file_length} ≠ {size}")


def _check_fields_unchanged(h0, h1, fields, what: str = "") -> None:
    for field in fields:
        if getattr(h1, field) != getattr(h0, field):
            raise ValueError(
                f"header.{field} changed{what} "
                f"({getattr(h0, field)} -> {getattr(h1, field)})")


def _check_other_sites_unchanged(sites0: list, sites1: list, skip: int, what: str) -> None:
    for j, s in enumerate(sites1):
        if j != skip and _site_full(s) != _site_full(sites0[j]):
            raise ValueError(f"array:{j} changed unexpectedly after the {what}")


class HermitProject:
    """HBC analysis session, opened from a .hbc file, entirely in memory."""

    def __init__(self, hbc: _libhbc.HBCFile, hbc_path: str) -> None:
        self.hbc          = hbc
        self._hbc_path    = hbc_path

        # Mutable file image. None = identical to the file on disk. On the
        # first patch it becomes the authoritative bytearray and `self.hbc` is
        # re-parsed from it, so disasm/strings/functions see the patch. Not
        # persisted: the `patch` command writes it to a new .hbc.
        self._image: Optional[bytearray] = None
        self._patched = False

    @classmethod
    def from_hbc(cls, hbc_path: str) -> "HermitProject":
        hbc_path = os.path.abspath(hbc_path)
        return cls(_libhbc.HBCFile.open(hbc_path), hbc_path)

    # ------------------------------------------------------------------
    # State
    # ------------------------------------------------------------------

    @property
    def image_bytes(self) -> bytes:
        """The file's current bytes: the patched image if there is one, else the file on disk."""
        if self._image is not None:
            return bytes(self._image)
        with open(self._hbc_path, "rb") as fh:
            return fh.read()

    @property
    def is_patched(self) -> bool:
        return self._patched

    @property
    def hbc_path(self) -> str:
        return self._hbc_path

    def write_patched(self, out_path: str) -> int:
        """Writes the current image (a faithful copy if unpatched). Returns the bytes written."""
        data = self.image_bytes
        with open(out_path, "wb") as fh:
            fh.write(data)
        return len(data)

    def _commit(self, image, new_hbc: "_libhbc.HBCFile") -> None:
        """The image becomes authoritative."""
        self._image = bytearray(image)
        self.hbc = new_hbc
        self._patched = True

    # ------------------------------------------------------------------
    # Shared machinery
    # ------------------------------------------------------------------

    @staticmethod
    def _splice(base: bytearray, offset: int, blob: bytes) -> None:
        base[offset:offset + len(blob)] = blob

    @staticmethod
    def _refresh_footer(base: bytearray) -> None:
        # SHA-1 of every byte except the footer. The Hermes runtime doesn't
        # verify it (hermes-dec does).
        base[-_FOOTER:] = hashlib.sha1(bytes(base[:-_FOOTER])).digest()

    def _structural(self, what: str, op) -> "tuple[bytes, _libhbc.HBCFile, _libhbc.Module]":
        """Runs `op(module)` on a Module of the current file, serializes and
        re-parses. Errors of `op` become ValueError."""
        m = _libhbc.Module.from_hbcfile(self.hbc)
        try:
            result = op(m)
        except Exception as exc:
            raise ValueError(str(exc)) from exc
        image = m.serialize()
        return image, _parse_image(image, what), result

    def _check_disasm_sample(self, new_hbc, h0, what: str, extra=()) -> None:
        """Structural edits move the bytecode as blocks: instead of
        re-disassembling every function (impractical on real bundles) compare a
        sample: first, last, middle, the global entry, plus `extra`."""
        n = len(self.hbc.functions())
        sample = sorted({0, n - 1, h0.global_code_index, n // 2, *extra} & set(range(n)))
        for i in sample:
            old_ins, new_ins = self.hbc.disasm(i), new_hbc.disasm(i)
            if len(new_ins) != len(old_ins) or any(x.mnemonic == "???" for x in new_ins):
                raise ValueError(
                    f"integrity: disasm func {i} broken after {what} "
                    f"({len(old_ins)} -> {len(new_ins)} instr)")
            if [x.mnemonic for x in new_ins] != [x.mnemonic for x in old_ins]:
                raise ValueError(f"integrity: disasm func {i} changed after {what}")

    def _check_string_state(self, new_hbc, str_idx: int, expected_value: str,
                            strings0: list, kinds0: list, what: str) -> list:
        """Strings after an edit of `str_idx`: it has the expected value, no
        other string and no kind changed. Returns the new strings."""
        strings1 = list(new_hbc.strings())
        if strings1[str_idx] != expected_value:
            raise ValueError(
                f"re-parse: string[{str_idx}] = {strings1[str_idx]!r} "
                f"≠ expected {expected_value!r}")
        for j in range(len(strings0)):
            if j != str_idx and strings1[j] != strings0[j]:
                raise ValueError(f"the {what} altered string[{j}]")
        if list(new_hbc.string_kinds()) != kinds0:
            raise ValueError(f"string kind table changed by the {what}")
        return strings1

    # ------------------------------------------------------------------
    # Same-size patches: spliced into a COPY of the image, checked, committed
    # ------------------------------------------------------------------

    def apply_patch(self, func_idx: int, patches: dict[int, bytes]) -> None:
        """Applies re-encoded bytes (`{absolute_offset: bytes}`) of a same-size
        edit to the image. With no relocation there is no section field to
        recompute. Sequence: splice on a copy, SHA-1 footer, re-parse, re-disasm
        the function: instruction starts must be identical to before.
        On any failure the current image is intact and `ValueError` is raised."""
        if not patches:
            return

        base = bytearray(self.image_bytes)
        limit = len(base) - _FOOTER

        for abs_off, blob in patches.items():
            if abs_off < 0 or abs_off + len(blob) > limit:
                raise ValueError(f"patch out of range at offset {abs_off:#x}")
            self._splice(base, abs_off, blob)
        self._refresh_footer(base)

        old_starts = None
        funcs0 = self.hbc.functions()
        if func_idx < len(funcs0):
            fi0 = funcs0[func_idx]
            old_starts = {fi0.offset + ins.offset for ins in self.hbc.disasm(func_idx)}

        new_hbc = _parse_image(base, "patch")
        _check_file_length(new_hbc.header(), len(base))

        if old_starts is not None:
            fi1 = new_hbc.functions()[func_idx]
            new_starts = {fi1.offset + ins.offset for ins in new_hbc.disasm(func_idx)}
            if new_starts != old_starts:
                raise ValueError(
                    "the patch moved the instruction boundaries (edit not same-size?)")

        self._commit(base, new_hbc)

    def apply_string_patch(self, str_idx: int, new_bytes: bytes, expected_value: str) -> None:
        """Same-length splice of ONE string into the string storage. Besides
        the `apply_patch` sequence: the string table must not move and every
        other string must stay byte-identical. Bytecode isn't touched, so no
        instruction check."""
        entries0 = list(self.hbc.string_entries())
        kinds0   = list(self.hbc.string_kinds())
        strings0 = list(self.hbc.strings())

        _check_index(str_idx, len(entries0), "string index")
        e = entries0[str_idx]
        if kinds0[str_idx] == _libhbc.StringKind.Identifier:
            raise ValueError(
                "Identifier string: overwriting it desyncs the Identifier Hash Table")
        if len(new_bytes) != e.byte_length:
            raise ValueError(
                f"length {len(new_bytes)} B ≠ {e.byte_length} B original "
                f"(length change needs relocation)")

        off = e.storage_offset
        base = bytearray(self.image_bytes)
        if off < 0 or off + len(new_bytes) > len(base) - _FOOTER:
            raise ValueError(f"string storage out of range at offset {off:#x}")

        # Authoritative re-scan for suffix sharing: [off, off+len) must not
        # intersect any other entry (half-open; empty entries don't count).
        a0, a1 = off, off + len(new_bytes)
        for j, other in enumerate(entries0):
            if j == str_idx or other.byte_length == 0:
                continue
            b0 = other.storage_offset
            b1 = b0 + other.byte_length
            if a0 < b1 and b0 < a1:
                raise ValueError(
                    f"suffix-sharing with string:{j} {strings0[j]!r} - "
                    f"bytes [{max(a0, b0):#x},{min(a1, b1):#x}) shared")

        self._splice(base, off, new_bytes)
        self._refresh_footer(base)

        new_hbc = _parse_image(base, "patched string")
        h0, h1 = self.hbc.header(), new_hbc.header()
        _check_file_length(h1, len(base))
        _check_fields_unchanged(h0, h1, ("string_count", "string_storage_size",
                                         "overflow_string_count", "string_kind_count"))
        self._check_string_state(new_hbc, str_idx, expected_value, strings0, kinds0, "patch")
        if [_entry_key(x) for x in new_hbc.string_entries()] != [_entry_key(x) for x in entries0]:
            raise ValueError("string table moved (entries don't match)")

        self._commit(base, new_hbc)

    def apply_array_patch(
        self, buffer_name: str, abs_off: int, new_bytes: bytes, site_idx: int
    ) -> None:
        """Same-length splice of ONE element of the `array buffer`
        (`buffer_name="array"`) or the `object value buffer` (`"objvalue"`).
        Same mechanics as `apply_string_patch`, plus an authoritative overlap
        re-scan of the sites: Hermes deduplicates literal buffers, so two sites
        can read the same bytes."""
        if buffer_name not in ("array", "objvalue"):
            raise ValueError(f"unknown buffer_name: {buffer_name!r}")

        sites = list(self.hbc.literal_sites())
        _check_index(site_idx, len(sites), "site index")

        h0 = self.hbc.header()
        if buffer_name == "array":
            buf_off, buf_size = self.hbc.array_buffer_offset(), h0.array_buffer_size
        else:
            buf_off, buf_size = self.hbc.obj_value_buffer_offset(), h0.obj_value_buffer_size

        base = bytearray(self.image_bytes)
        a0, a1 = abs_off, abs_off + len(new_bytes)
        if a0 < buf_off or a1 > buf_off + buf_size or a1 > len(base) - _FOOTER:
            raise ValueError(
                f"splice outside the {buffer_name!r} buffer at offset {abs_off:#x}")

        # `abs_off` must be EXACTLY the start of an element of the target site
        # and `new_bytes` must have its original width (local decode of the
        # site; the overlap scan below looks at the OTHER sites).
        site = sites[site_idx]
        target_buf = self.hbc.array_buffer() if buffer_name == "array" else self.hbc.obj_value_buffer()
        site_elems = _decode_site(target_buf, site.buffer_idx, site.count)
        target = next((e for e in site_elems if e.byte_off + buf_off == abs_off), None)
        if target is None:
            raise ValueError(
                f"offset {abs_off:#x} doesn't correspond to an element of array:{site_idx}")
        if len(new_bytes) != target.byte_len:
            raise ValueError(
                f"length {len(new_bytes)} B ≠ {target.byte_len} B original "
                f"(element {target.tag.name}) - type change needs relocation")

        for j, s in enumerate(sites):
            if j == site_idx:
                continue
            rng = self._site_byte_range(s, buffer_name)
            if rng is None:
                continue
            b0, b1 = rng
            if a0 < b1 and b0 < a1:
                raise ValueError(
                    f"buffer shared with array:{j} (func {s.function_index}) - "
                    f"bytes [{max(a0, b0):#x},{min(a1, b1):#x}) shared. "
                    f"Detach needs relocation")

        self._splice(base, a0, new_bytes)
        self._refresh_footer(base)

        new_hbc = _parse_image(base, "patched array")
        h1 = new_hbc.header()
        _check_file_length(h1, len(base))
        _check_fields_unchanged(h0, h1, ("array_buffer_size", "obj_key_buffer_size",
                                         "obj_value_buffer_size"))
        if [_site_full(s) for s in new_hbc.literal_sites()] != [_site_full(s) for s in sites]:
            raise ValueError("literal_sites() changed after the edit")

        self._commit(base, new_hbc)

    def _site_byte_range(self, site, buffer_name: str):
        """ABSOLUTE `[start, end)` range of `site` in the `buffer_name` buffer
        ("array" or "objvalue"), or None if the site doesn't use it (an array
        site never uses "objvalue" and vice versa; keys are never patched)."""
        if buffer_name == "array":
            if site.is_object:
                return None
            buf, buf_off = self.hbc.array_buffer(), self.hbc.array_buffer_offset()
        elif buffer_name == "objvalue":
            if not site.is_object:
                return None
            buf, buf_off = self.hbc.obj_value_buffer(), self.hbc.obj_value_buffer_offset()
        else:
            raise ValueError(f"unknown buffer_name: {buffer_name!r}")

        local_start = site.buffer_idx
        if site.count == 0:
            return (buf_off + local_start, buf_off + local_start)
        values = decode_slp(buf, local_start, site.count)
        return (buf_off + local_start, buf_off + values[-1].byte_off + values[-1].byte_len)

    def sites_sharing_buffer(self, site_idx: int) -> list[int]:
        """Indices of the OTHER literal sites sharing at least one buffer byte
        with `site_idx`, for the editor's "shared ×N" banner (whole-site
        granularity; `apply_array_patch` re-scans per element)."""
        sites = list(self.hbc.literal_sites())
        _check_index(site_idx, len(sites), "site index")
        site = sites[site_idx]
        buffer_name = "objvalue" if site.is_object else "array"
        rng = self._site_byte_range(site, buffer_name)
        if rng is None or rng[0] == rng[1]:
            return []
        a0, a1 = rng
        out = []
        for j, s in enumerate(sites):
            if j == site_idx:
                continue
            r = self._site_byte_range(s, buffer_name)
            if r is None:
                continue
            b0, b1 = r
            if a0 < b1 and b0 < a1:
                out.append(j)
        return out

    # ------------------------------------------------------------------
    # Structural edits, through Module (the image is rebuilt)
    # ------------------------------------------------------------------

    def apply_string_relocation(self, str_idx: int, new_bytes: bytes, expected_value: str) -> None:
        """Changes the CONTENT (and so the length) of a string: the new bytes go
        to the end of the string storage and only that entry is repointed.
        The append never touches existing bytes, so a shared entry is detached
        implicitly and no overlap scan is needed (unlike `apply_string_patch`).
        `new_bytes` comes from `encode_string_new_value`."""
        entries0 = list(self.hbc.string_entries())
        kinds0   = list(self.hbc.string_kinds())
        strings0 = list(self.hbc.strings())
        h0 = self.hbc.header()

        _check_index(str_idx, len(entries0), "string index")
        if kinds0[str_idx] == _libhbc.StringKind.Identifier:
            raise ValueError(
                "Identifier string: overwriting it desyncs the Identifier Hash Table")

        new_image, new_hbc, _ = self._structural(
            "relocated string", lambda m: m.set_string_value(str_idx, new_bytes))

        h1 = new_hbc.header()
        _check_file_length(h1, len(new_image))
        _check_fields_unchanged(h0, h1, ("string_count", "string_kind_count",
                                         "overflow_string_count"))
        if h1.string_storage_size != h0.string_storage_size + len(new_bytes):
            raise ValueError(
                f"string_storage_size {h1.string_storage_size} ≠ "
                f"{h0.string_storage_size + len(new_bytes)}")

        self._check_string_state(new_hbc, str_idx, expected_value, strings0, kinds0, "relocation")

        entries1 = list(new_hbc.string_entries())
        for j in range(len(entries0)):
            if j != str_idx and _entry_key(entries1[j]) != _entry_key(entries0[j]):
                raise ValueError(
                    f"string_entries[{j}] changed - only string:{str_idx} should move")

        self._check_disasm_sample(new_hbc, h0, "set_string_value")
        self._commit(new_image, new_hbc)

    def apply_add_string(self, value: str) -> int:
        """Appends a new `String`-kind string (the kind table, string table and
        storage grow, every later section is re-laid-out). Returns its index."""
        h0 = self.hbc.header()
        strings0 = list(self.hbc.strings())
        kinds0   = list(self.hbc.string_kinds())

        new_image, new_hbc, idx = self._structural("add_string", lambda m: m.add_string(value))

        enc = value.encode("ascii")
        h1 = new_hbc.header()
        _check_file_length(h1, len(new_image))
        if h1.string_count != h0.string_count + 1:
            raise ValueError(f"string_count {h1.string_count} ≠ {h0.string_count + 1}")
        if h1.string_storage_size != h0.string_storage_size + len(enc):
            raise ValueError(
                f"string_storage_size {h1.string_storage_size} ≠ "
                f"{h0.string_storage_size + len(enc)}")
        if h1.string_kind_count not in (h0.string_kind_count, h0.string_kind_count + 1):
            raise ValueError(
                f"string_kind_count {h1.string_kind_count} outside "
                f"[{h0.string_kind_count}, {h0.string_kind_count + 1}]")
        exp_ovf = h0.overflow_string_count + (1 if len(enc) >= 255 else 0)
        if h1.overflow_string_count != exp_ovf:
            raise ValueError(
                f"overflow_string_count {h1.overflow_string_count} ≠ {exp_ovf}")

        strings1 = list(new_hbc.strings())
        if idx != h0.string_count or strings1[idx] != value:
            raise ValueError(
                f"re-parse: string[{idx}] = {strings1[idx]!r} ≠ expected {value!r}")
        if strings1[:idx] != strings0:
            raise ValueError("add_string altered a pre-existing string")
        kinds1 = list(new_hbc.string_kinds())
        if kinds1[:idx] != kinds0 or kinds1[idx] != _libhbc.StringKind.String:
            raise ValueError("add_string altered the string kind table")

        self._check_disasm_sample(new_hbc, h0, "add_string")
        self._commit(new_image, new_hbc)
        return idx

    def apply_array_relocation(
        self, func_idx: int, instr_offset: int, is_object: bool,
        new_buffer_bytes: bytes, site_idx: int, changed_indices,
    ) -> None:
        """Changes the VALUE and/or TAG of elements of a literal site, including
        what the same-size path rejects (Null/True/False toggles, type changes,
        shared buffers). `new_buffer_bytes` is the COMPLETE payload of the
        site, re-serialized by `cli.literals.encode_slp_sequence` from the
        editor's current state, and is appended to the buffer by
        `Module.set_literal_value`: nothing is overwritten, so a shared buffer
        is detached implicitly. `changed_indices` (the elements touched in the
        session) is only used to check that all the others are unchanged."""
        changed_indices = set(changed_indices)
        sites0 = list(self.hbc.literal_sites())
        _check_index(site_idx, len(sites0), "site index")
        site0 = sites0[site_idx]
        self._check_site_matches(site0, site_idx, func_idx, instr_offset, is_object)
        if not changed_indices:
            raise ValueError("changed_indices empty - no element to relocate")
        for i in changed_indices:
            if not (0 <= i < site0.count):
                raise ValueError(f"changed_indices contains {i}, out of range (0–{site0.count - 1})")

        h0 = self.hbc.header()
        old_buf = self.hbc.obj_value_buffer() if is_object else self.hbc.array_buffer()
        old_elems = _decode_site(old_buf, site0.buffer_idx, site0.count)

        new_image, new_hbc, _ = self._structural(
            "relocated literal",
            lambda m: m.set_literal_value(func_idx, instr_offset, is_object, new_buffer_bytes))

        h1 = new_hbc.header()
        _check_file_length(h1, len(new_image))

        target_field = "obj_value_buffer_size" if is_object else "array_buffer_size"
        other_fields = [f for f in ("array_buffer_size", "obj_key_buffer_size", "obj_value_buffer_size")
                        if f != target_field]
        exp_target = getattr(h0, target_field) + len(new_buffer_bytes)
        if getattr(h1, target_field) != exp_target:
            raise ValueError(f"{target_field} {getattr(h1, target_field)} ≠ {exp_target}")
        for f in other_fields:
            if getattr(h1, f) != getattr(h0, f):
                raise ValueError(f"{f} changed unexpectedly ({getattr(h0, f)} -> {getattr(h1, f)})")

        new_sites = list(new_hbc.literal_sites())
        if len(new_sites) != len(sites0):
            raise ValueError(
                f"literal_sites() count changed ({len(sites0)} -> {len(new_sites)})")

        site1 = new_sites[site_idx]
        if (_site_id(site1), site1.count, site1.key_buffer_idx) != \
           (_site_id(site0), site0.count, site0.key_buffer_idx):
            raise ValueError(f"site array:{site_idx} changed beyond bufferIdx")
        if site1.buffer_idx != getattr(h0, target_field):
            raise ValueError(
                f"expected bufferIdx {getattr(h0, target_field)}, found {site1.buffer_idx}")
        _check_other_sites_unchanged(sites0, new_sites, site_idx, "relocation")

        new_buf = new_hbc.obj_value_buffer() if is_object else new_hbc.array_buffer()
        new_elems = _decode_site(new_buf, site1.buffer_idx, site1.count)
        for i in range(site0.count):
            if i not in changed_indices and \
               (new_elems[i].tag, new_elems[i].value) != (old_elems[i].tag, old_elems[i].value):
                raise ValueError(f"element {i} of site array:{site_idx} changed (expected unchanged)")

        self._check_disasm_sample(new_hbc, h0, "set_literal_value", extra=(func_idx,))
        self._commit(new_image, new_hbc)

    def apply_array_resize(
        self, func_idx: int, instr_offset: int, is_object: bool, is_add: bool,
        new_buffer_bytes: bytes, new_key_buffer_bytes: bytes, site_idx: int,
    ) -> None:
        """Adds (`is_add`) or removes the LAST element (array) or key-value pair
        (object) of a literal site. The buffers are the COMPLETE payloads with
        count ± 1 elements (`cli.literals.encode_slp_sequence`). Like
        `apply_array_relocation`, but it also rewrites `count`/`sizeHint`
        (`Module.resize_literal`) and, for objects, appends to the key buffer
        (only to add/remove the last pair, never to rename a key)."""
        sites0 = list(self.hbc.literal_sites())
        _check_index(site_idx, len(sites0), "site index")
        site0 = sites0[site_idx]
        self._check_site_matches(site0, site_idx, func_idx, instr_offset, is_object)
        if not is_add and site0.count == 0:
            raise ValueError(f"array:{site_idx} has no elements to remove")

        expected_count = site0.count + (1 if is_add else -1)

        h0 = self.hbc.header()
        old_val_buf = self.hbc.obj_value_buffer() if is_object else self.hbc.array_buffer()
        old_elems = _decode_site(old_val_buf, site0.buffer_idx, site0.count)
        old_keys = (_decode_site(self.hbc.obj_key_buffer(), site0.key_buffer_idx, site0.count)
                    if is_object else None)

        new_image, new_hbc, _ = self._structural(
            "resized literal",
            lambda m: m.resize_literal(func_idx, instr_offset, is_object, is_add,
                                       new_buffer_bytes, new_key_buffer_bytes))

        h1 = new_hbc.header()
        _check_file_length(h1, len(new_image))

        val_field = "obj_value_buffer_size" if is_object else "array_buffer_size"
        exp_val = getattr(h0, val_field) + len(new_buffer_bytes)
        if getattr(h1, val_field) != exp_val:
            raise ValueError(f"{val_field} {getattr(h1, val_field)} ≠ {exp_val}")
        if is_object:
            exp_key = h0.obj_key_buffer_size + len(new_key_buffer_bytes)
            if h1.obj_key_buffer_size != exp_key:
                raise ValueError(f"obj_key_buffer_size {h1.obj_key_buffer_size} ≠ {exp_key}")
            if h1.array_buffer_size != h0.array_buffer_size:
                raise ValueError("array_buffer_size changed unexpectedly")
        else:
            for f in ("obj_key_buffer_size", "obj_value_buffer_size"):
                if getattr(h1, f) != getattr(h0, f):
                    raise ValueError(f"{f} changed unexpectedly")

        new_sites = list(new_hbc.literal_sites())
        if len(new_sites) != len(sites0):
            raise ValueError(
                f"literal_sites() count changed ({len(sites0)} -> {len(new_sites)})")

        site1 = new_sites[site_idx]
        if _site_id(site1) != _site_id(site0):
            raise ValueError(f"site array:{site_idx} changed beyond count/bufferIdx")
        if site1.count != expected_count:
            raise ValueError(f"count {site1.count} ≠ expected {expected_count}")
        if site1.buffer_idx != getattr(h0, val_field):
            raise ValueError(
                f"expected bufferIdx {getattr(h0, val_field)}, found {site1.buffer_idx}")
        if is_object and site1.key_buffer_idx != h0.obj_key_buffer_size:
            raise ValueError(
                f"expected keyBufferIdx {h0.obj_key_buffer_size}, found {site1.key_buffer_idx}")

        # sizeHint == count must still hold (LiteralSite doesn't expose sizeHint,
        # so re-disassemble the instruction).
        instr = next((x for x in new_hbc.disasm(func_idx) if x.offset == instr_offset), None)
        if instr is None or len(instr.operands) < 3:
            raise ValueError("integrity: site instruction not found after the edit")
        if instr.operands[1].u32 != expected_count or instr.operands[2].u32 != expected_count:
            raise ValueError(
                f"instruction sizeHint/count inconsistent with {expected_count} "
                f"after resize_literal")

        _check_other_sites_unchanged(sites0, new_sites, site_idx, "resize")

        new_val_buf = new_hbc.obj_value_buffer() if is_object else new_hbc.array_buffer()
        new_elems = _decode_site(new_val_buf, site1.buffer_idx, site1.count)
        if is_add:
            if _tag_values(new_elems[:-1]) != _tag_values(old_elems):
                raise ValueError("existing elements changed after the add")
        elif _tag_values(new_elems) != _tag_values(old_elems[:-1]):
            raise ValueError("remaining elements changed after the remove")

        if is_object:
            new_keys = _decode_site(new_hbc.obj_key_buffer(), site1.key_buffer_idx, site1.count)
            if is_add:
                if _tag_values(new_keys[:-1]) != _tag_values(old_keys):
                    raise ValueError("existing keys changed after the add")
            elif _tag_values(new_keys) != _tag_values(old_keys[:-1]):
                raise ValueError("remaining keys changed after the remove")

        self._check_disasm_sample(new_hbc, h0, "resize_literal", extra=(func_idx,))
        self._commit(new_image, new_hbc)

    @staticmethod
    def _check_site_matches(site0, site_idx: int, func_idx: int, instr_offset: int,
                            is_object: bool) -> None:
        if (site0.function_index, site0.instruction_offset, site0.is_object) != (
            func_idx, instr_offset, is_object
        ):
            raise ValueError(
                f"site array:{site_idx} does not match the given "
                "func_idx/instr_offset/is_object")

    def apply_insertion(self, func_idx: int, asm_instrs, frame_size: int,
                        edit_all: bool = False) -> list[str]:
        """Replaces a function's body with `asm_instrs`, a list of
        `(opcode, operands, jump_target_idx, addr_operand_pos, orig_byte_offset)`
        from `cli.editor.assemble_committed_function`. With `edit_all`, a
        function with shared bytecode applies to its whole group. `Module` does
        the jump relocation, branch relaxation, EH relocation, overflow header
        rewrite, layout and footer. Returns `Module`'s warnings; `ValueError` if
        the output doesn't re-parse or fails the integrity check."""
        m = _libhbc.Module.from_hbcfile(self.hbc)
        m.set_function_body(func_idx, list(asm_instrs), frame_size, edit_all)
        new_image = m.serialize()
        new_hbc = _parse_image(new_image, "structural edit")

        ins = new_hbc.disasm(func_idx)
        if len(ins) != len(asm_instrs):
            raise ValueError(
                f"integrity: {len(ins)} re-disassembled instructions ≠ {len(asm_instrs)} expected")
        if any(x.mnemonic == "???" for x in ins):
            raise ValueError("integrity: unknown opcode in the re-serialized body")

        self._commit(new_image, new_hbc)
        return list(m.warnings())

    def apply_add_function(self, asm_instrs, frame_size: int) -> int:
        """Adds a NEW function at the end of the function table. `asm_instrs`
        has the shape of `apply_insertion`. `Module::add_function` fixes the
        metadata: paramCount 1, functionName 0 (cosmetic), strict, callable and
        constructible, no exception handler or debug info. Returns the new index."""
        h0 = self.hbc.header()
        n0 = len(self.hbc.functions())

        new_image, new_hbc, idx = self._structural(
            "add_function", lambda m: m.add_function(list(asm_instrs), frame_size))

        h1 = new_hbc.header()
        _check_file_length(h1, len(new_image))
        if h1.function_count != h0.function_count + 1:
            raise ValueError(
                f"function_count {h1.function_count} ≠ {h0.function_count + 1}")
        if h1.global_code_index != h0.global_code_index:
            raise ValueError(
                f"global_code_index {h1.global_code_index} ≠ {h0.global_code_index}")
        if idx != n0:
            raise ValueError(f"add_function: index {idx} ≠ expected {n0}")

        ins = new_hbc.disasm(idx)
        if len(ins) != len(asm_instrs):
            raise ValueError(
                f"integrity: {len(ins)} re-disassembled instructions ≠ {len(asm_instrs)} expected")
        if any(x.mnemonic == "???" for x in ins):
            raise ValueError("integrity: unknown opcode in the new function's body")

        self._check_disasm_sample(new_hbc, h0, "add_function")
        self._commit(new_image, new_hbc)
        return idx
