# HBC Bytecode Format — Version 96 (Technical Reference)

> Reference manual for the Hermes Bytecode (HBC) binary format, version 96 —
> the version shipped by every React Native application from RN 0.73 up to
> 0.83. This document describes the
> on-disk layout field by field, cross-referenced against:
>
> 1. hermit's own C++ parser (`core/include/hbc/format.h`,
>    `core/include/hbc/hbc.h`, `core/src/parser/parser.cpp`), which is the
>    implementation this document is meant to describe and that has been
>    exercised against both synthetic samples and real React Native bundles
>    (`ridemovi.hbc`, 37774 functions);
> 2. the Hermes compiler source at the tag
>    `hermes-2023-09-26-RNv0.73.0-ee2922a50fb719bdb378025d95dbd32ad93cd679`
>    (`BYTECODE_VERSION = 96`), in particular
>    `include/hermes/BCGen/HBC/BytecodeFileFormat.h` and
>    `include/hermes/BCGen/HBC/BytecodeList.def`. **Never** the git tag
>    `v0.12.0` (HBC 89, a different and older format) nor the default branch
>    `static_h` (HBC 99, Static Hermes — a related but distinct compiler, see
>    `bytecode-versions.md`).
>
> `BytecodeFileFormat.h` is identical across v94, v95 and v96 — the struct
> layout in this document also describes those two versions. Only the opcode
> *table* differs per version (new/removed opcodes, renumbering); see
> §13 for the v96 table specifically.
>
> Sections marked **NOT YET EXPLORED** are present in the struct definitions
> and are faithfully preserved on round-trip (copied byte-for-byte or
> recomputed from scratch so the file stays valid), but their *content* has
> never been decoded into a data structure, because no sample observed so
> far exercises the non-trivial case. Each such section links to the
> corresponding bonus-phase entry in
> [`../phases/phase-bonus/README.md`](../phases/phase-bonus/README.md) with
> the concrete next step to unblock it.

---

## 1. Conventions

- **Byte order:** little-endian throughout.
- **Integer types:** `uN` = unsigned N-bit integer, `iN` = signed N-bit
  (two's complement), `f64` = IEEE 754 double.
- **Bitfields:** `field[N]` = N bits. On little-endian, C++ bitfields are
  assigned starting from the LSB of the enclosing word — bit 0 of a
  `struct { uint32_t a:5; uint32_t b:3; }` is the lowest bit of byte 0.
- **Offsets:** absolute from the start of the file, unless stated otherwise
  (the exception-handler table and the debug-info sub-sections are relative
  to a function's own bytecode or to the debug blob, see §4.4 and §9).
- **`field × N`:** N consecutive repetitions of `field`.
- **Alignment:** every section is aligned to 4 bytes, *including* the raw
  string/buffer blobs — Hermes pads with zero bytes after an odd-sized blob
  so the next section starts on a 4-byte boundary (`BYTECODE_ALIGNMENT`).
  Only the file header (fixed 128 B) and the file footer (fixed 20 B) are
  exempt, since their size is already a multiple of 4.

**Operand physical types** (bytecode instruction encoding, §12–13):

| Type | Bytes | Meaning |
|---|---|---|
| `Reg8` | 1 | register index |
| `Reg32` | 4 | register index, wide form (`MovLong`) |
| `UInt8` / `UInt16` / `UInt32` | 1 / 2 / 4 | unsigned integer |
| `Imm32` | 4 | signed integer |
| `Double` | 8 | IEEE 754 double |
| `Addr8` / `Addr32` | 1 / 4 | signed jump offset, relative to the start of the *instruction* (not to the next instruction) |

In §13 an operand marked with a trailing `*` is additionally a String Table
index — the physical type above still fixes its on-disk size; the `*` only
means the disassembler resolves it against `strings()` for display.

---

## 2. File layout overview

The file is a flat sequence of sections. Only `debugInfoOffset` is stored as
an absolute offset in the header (§3); every other section's position is
derived by walking the header's counts/sizes in order, starting right after
the fixed-size header. This is exactly what hermit's `HBCFile::section_map()`
does, and it mirrors Hermes's own `visitBytecodeSegmentsInOrder`.

| # | Section | Size | Align |
|---|---|---|---|
| 1 | File Header | 128 B, fixed | 1 |
| 2 | Function Headers | `functionCount` × 16 B (`SmallFuncHeader`) | 4 |
| 3 | String Kind Table | `stringKindCount` × 4 B | 4 |
| 4 | Identifier Hash Table | `identifierCount` × 4 B | 4 |
| 5 | String Table | `stringCount` × 4 B (`SmallStringTableEntry`) | 4 |
| 6 | Overflow String Table | `overflowStringCount` × 8 B | 4 |
| 7 | String Storage | `stringStorageSize` B, raw blob | 4 |
| 8 | Array Buffer | `arrayBufferSize` B | 4 |
| 9 | Object Key Buffer | `objKeyBufferSize` B | 4 |
| 10 | Object Value Buffer | `objValueBufferSize` B | 4 |
| 11 | BigInt Table + Storage | `bigIntCount` × 8 B + `bigIntStorageSize` B | 4 |
| 12 | RegExp Table + Storage | `regExpCount` × 8 B + `regExpStorageSize` B | 4 |
| 13 | CJS Module Table | `cjsModuleCount` × 8 B | 4 |
| 14 | Function Source Table | `functionSourceCount` × 8 B | 4 |
| 15 | Bytecode | from the end of §14 (aligned) up to `debugInfoOffset` | 4 |
| 16 | Debug Info | `fileLength − 20 − debugInfoOffset` | 4 |
| 17 | File Footer | 20 B, fixed | 1 |

A section with count/size 0 is entirely omitted (it contributes no bytes and
does not advance the running offset) — this is why, say, a file with no
BigInt literals has no "BigInt Table" section at all, not an empty one.

**What "Bytecode" (§15) actually contains.** This single span holds, back to
back for every function in the function table: the function's raw
instruction stream (`bytecodeSizeInBytes` bytes, padded to a 4-byte boundary,
plus any `SwitchImm` jump tables appended right after the padding — §4.3),
immediately followed by that function's *function-info section* (§4.4:
overflow `FunctionHeader` if the function overflowed, exception-handler
table, and the three debug-info offset fields). There is no section-level
separator between one function's data and the next's, and no padding
*between* function bodies beyond the 4-byte alignment inherent in the jump
table placement — this was confirmed empirically (`finding_phase12_m4_layout`
during development) and is the reason the patcher must track exact byte
extents per function rather than assuming a fixed stride.

**Shared function bodies.** In real React Native bundles, multiple
`SmallFuncHeader` entries can point at the *same* `(offset,
bytecodeSizeInBytes)` pair inside the Bytecode section — i.e. several
functions in the function table share one physical instruction stream
(`ridemovi.hbc` has 1862 such groups covering 8851 of its 37774 functions).
This is a size optimization implemented in Hermes's bytecode-layout pass
(`lib/BCGen/HBC/BytecodeStream.cpp`, deduplication by a `DedupKey` over the
emitted opcode array) and is documented only in the Hermes source, not in
any format spec. See [`shared-bytecode.md`](shared-bytecode.md) for the full
analysis and its consequences for patching.

---

## 3. File Header

`BytecodeFileHeader`, 128 bytes, always at offset 0.

| Field | Off | B | Type | Notes |
|---|---|---|---|---|
| `magic` | 0x00 | 8 | `u64` | `0x1F1903C103BC1FC6` (bytes on disk: `C6 1F BC 03 C1 03 19 1F`) |
| `version` | 0x08 | 4 | `u32` | 96 for this document |
| `sourceHash` | 0x0C | 20 | `u8[20]` | SHA-1 of the original JS source. **Not verified by the Hermes runtime** — see `doc/security/integrity-ios.md` in the thesis repository for the security implication (no bytecode integrity check at load time) |
| `fileLength` | 0x20 | 4 | `u32` | total file size, footer included |
| `globalCodeIndex` | 0x24 | 4 | `u32` | function-table index of the top-level (module-wrapper) function |
| `functionCount` | 0x28 | 4 | `u32` | |
| `stringKindCount` | 0x2C | 4 | `u32` | number of RLE runs in the String Kind Table (§5.1), not the number of strings |
| `identifierCount` | 0x30 | 4 | `u32` | number of strings used as property/variable names (a subset of all strings, flagged `Identifier` in §5.1) |
| `stringCount` | 0x34 | 4 | `u32` | total string count (`Identifier` + plain `String`) |
| `overflowStringCount` | 0x38 | 4 | `u32` | strings whose length ≥ 255 characters, which don't fit `SmallStringTableEntry.length` (§5.3–5.4) |
| `stringStorageSize` | 0x3C | 4 | `u32` | bytes of the raw string blob (§5.5) |
| `bigIntCount` | 0x40 | 4 | `u32` | **NOT YET EXPLORED**, see §7 |
| `bigIntStorageSize` | 0x44 | 4 | `u32` | ″ |
| `regExpCount` | 0x48 | 4 | `u32` | **partially explored**, see §8 |
| `regExpStorageSize` | 0x4C | 4 | `u32` | ″ |
| `arrayBufferSize` | 0x50 | 4 | `u32` | array-literal serialized buffer (§6) |
| `objKeyBufferSize` | 0x54 | 4 | `u32` | object-literal key buffer (§6) |
| `objValueBufferSize` | 0x58 | 4 | `u32` | object-literal value buffer (§6) |
| `segmentID` | 0x5C | 4 | `u32` | identifies which compiled segment this file is, when the Hermes compiler is invoked in multi-segment CommonJS mode (§9.3). Does not affect this document's layout in any way — it is a plain diagnostic tag. Always 0 in a monolithic (single-file) bundle |
| `cjsModuleCount` | 0x60 | 4 | `u32` | number of entries in the CJS Module Table (§9). Always 0 in Metro-bundled React Native output |
| `functionSourceCount` | 0x64 | 4 | `u32` | entries in the Function Source Table, §4.5 |
| `debugInfoOffset` | 0x68 | 4 | `u32` | **the only absolute section offset stored in the header.** Every other section's position is derived by summing the sizes above, in header-field order (§2) |
| `options` | 0x6C | 1 | `u8` | bitfield, see below |
| `padding` | 0x6D | 19 | `u8[19]` | zero-filled |

### `options` bitfield

| Bit | Name | Notes |
|---|---|---|
| 0 | `staticBuiltins` | compiler flag; not currently interpreted by hermit beyond preserving it |
| 1 | `cjsModulesStaticallyResolved` | selects the on-disk shape of the CJS Module Table (§9.2). Always 0 for Metro-bundled React Native output, and — confirmed in §9.4 — unreachable as 1 from the public Hermes CLI compiler on any input |
| 2 | `hasAsync` | file contains at least one `async` function |
| 3–7 | reserved | always 0 |

---

## 4. Function Table

Every function in the file has an entry in a flat array of
`functionCount` fixed-size headers, immediately following the file header.
Two physical struct shapes exist: the common 16-byte `SmallFuncHeader`, and
a 32-byte `FunctionHeader` used only as an *overflow* record for functions
whose metadata doesn't fit the reduced bit widths of the small form.

### 4.1 `SmallFuncHeader` (16 bytes × `functionCount`)

Three 32-bit bitfield words, followed by four byte-sized fields.

| Bits | Field | Notes |
|---|---|---|
| **Word 0** | | |
| 0–24 | `offset` | absolute file offset of the function's bytecode; if `overflowed`, this instead holds `largeOff[0:15]` (see below) |
| 25–31 | `paramCount` | **declared JS parameter count + 1** (the `+1` is the implicit `this`) — confirmed empirically: a zero-arg function has `paramCount = 1`, and `LoadParam r, 0` always yields `this` |
| **Word 1** | | |
| 0–14 | `bytecodeSizeInBytes` | instruction-stream length in bytes; max 32767 before this field itself overflows |
| 15–31 | `functionName` | String Table index. For an arrow function assigned to a variable (`const f = () => ...`), Hermes names it after the variable, not a generic placeholder — there is no separate "anonymous function" naming convention to rely on at the format level |
| **Word 2** | | |
| 0–24 | `infoOffset` | absolute file offset of the function-info section (§4.4: EH table + debug-info offset triple); if `overflowed`, this instead holds `largeOff[16:31]` |
| 25–31 | `frameSize` | number of registers in the function's stack frame |
| **Byte fields** | | |
| — | `environmentSize` (`u8`) | captured-variable slot count for this function's own closure environment |
| — | `highestReadCacheIndex` (`u8`) | inline-cache metadata for property reads; not currently interpreted beyond preservation |
| — | `highestWriteCacheIndex` (`u8`) | inline-cache metadata for property writes; not currently interpreted beyond preservation |
| — | `flags` (`u8`) | see below |

**Overflow addressing.** When `flags.overflowed = 1`:

```
largeOff = (offset & 0xFFFF) | ((infoOffset & 0xFFFF) << 16)
```

`largeOff` is the absolute file offset of a full 32-byte `FunctionHeader`
(§4.2) holding the function's real, full-width metadata. This happens for
functions whose true values don't fit the narrow bitfields above — the
concrete trigger observed in `ridemovi.hbc` (11 occurrences, interleaved
with normal entries in the info block, not contiguous) is
`environmentSize > 255`, i.e. a closure capturing more than 255 variables.

### `flags` byte (shared by `SmallFuncHeader` and `FunctionHeader`)

| Bit | Name | Notes |
|---|---|---|
| 0–1 | `prohibitInvoke` | a **2-bit enum**, not two independent flags: `0` = plain `Call` is prohibited (the function can only be invoked with `new` — an ES2015 class constructor); `1` = `Construct` is prohibited (`new` is illegal — arrow, async, generator functions); `2` = `None`, both allowed (plain function). The VM's actual check is `prohibitInvoke == isConstructCall`, so a stray value of `3` degenerates to behaving like `None` |
| 2 | `strictMode` | |
| 3 | `hasExceptionHandler` | an EH table is present in this function's function-info section (§4.4) |
| 4 | `hasDebugInfo` | a debug-info offset triple is present in this function's function-info section (§4.4), pointing into §9 |
| 5 | `overflowed` | always `0` inside a `FunctionHeader` itself (it only ever means "look at `largeOff`" when read from a `SmallFuncHeader`) |
| 6–7 | reserved | always 0 |

### 4.2 `FunctionHeader` (overflow form, 32 bytes, variable count)

Not part of the fixed-count function table — instances live inline in the
Bytecode section, at `largeOff` as computed above, one per overflowed
function. `infoOffset` here is a **self-referential sentinel**: Hermes always
sets it equal to `largeOff` itself, and the function-info section actually
starts right after this struct, at `largeOff + 32` (not at the field's face
value, which would just point back at the struct).

| Field | Off | B |
|---|---|---|
| `offset` | 0x00 | 4 |
| `paramCount` | 0x04 | 4 |
| `bytecodeSizeInBytes` | 0x08 | 4 |
| `functionName` | 0x0C | 4 |
| `infoOffset` | 0x10 | 4 | sentinel, see above — real function-info is at `largeOff + 32` |
| `frameSize` | 0x14 | 4 |
| `environmentSize` | 0x18 | 4 |
| `highestReadCacheIndex` | 0x1C | 1 |
| `highestWriteCacheIndex` | 0x1D | 1 |
| `flags` | 0x1E | 1 | as above; `overflowed` reads back as 0 |
| `padding` | 0x1F | 1 | |

### 4.3 `SwitchImm` jump table

A per-function data area, appended right after that function's own
instruction stream, **outside** `bytecodeSizeInBytes` and outside the normal
instruction stream — the disassembler must stop at `bytecodeSizeInBytes` and
treat anything past it (up to the next function, or up to the start of the
function-info section) as jump-table data, present **only** if the function
contains at least one `SwitchImm` instruction.

| Position (from the start of the function's bytecode) | Content |
|---|---|
| `0` | instructions, `bytecodeSizeInBytes` bytes |
| `bytecodeSizeInBytes` | padding (zero bytes), up to the next multiple of 4 |
| `alignTo4(bytecodeSizeInBytes)` | `i32 × N`, all of this function's `SwitchImm` sub-tables, concatenated in instruction order |

Each `SwitchImm` has its own sub-table of `maxValue − minValue + 1` entries,
each a plain `i32` (a relative jump target), with no header and no
separator between sub-tables.

`SwitchImm` operands:

| Operand | Type | Notes |
|---|---|---|
| `discr` | `Reg8` | register holding the value to branch on |
| `tableOffset` | `UInt32` | from the start of the instruction to the start of its sub-table; **not itself 4-aligned** on disk — it is dynamically aligned at runtime |
| `defaultTarget` | `Addr32` | relative to the instruction, taken when `discr` is out of range |
| `minValue` | `UInt32` | |
| `maxValue` | `UInt32` | |

**Target resolution.** If `discr` is an integer in `[minValue, maxValue]`,
the entry read is at
`alignUp(ip + tableOffset, 4) + 4 × (discr − minValue)`, and the jump target
is `ip + entry` (the entry value is signed); the alignment is on the
*absolute* file address. Otherwise the jump target is `ip + defaultTarget`.
`ip` is the address of the `SwitchImm` instruction itself — this matches the
general jump-offset convention of the format (§12): *every* jump target,
including `SwitchImm`'s, is relative to the start of the jumping
instruction, never to the address right after it.

### 4.4 Function-info section

Located at `infoOffset` for a non-overflowed function, or at
`largeOff + 32` for an overflowed one (§4.1–4.2). Despite living physically
inside the "Bytecode" span of §2, it is logically separate metadata, not
instructions.

| Present when | Content | Notes |
|---|---|---|
| `flags.hasExceptionHandler` | `count:u32`, then `count ×` `{start:u32, end:u32, target:u32}` | `start`/`end`/`target` are offsets **relative to the start of this function's own bytecode**, not absolute file offsets. `[start, end)` is the protected range (end exclusive); `target` is the entry point of the matching catch handler |
| `flags.hasDebugInfo` | `debugOffset:u32`, `scopeDescOffset:u32`, `textifiedCalleesOffset:u32` | each offset is relative to the *start* of its own debug-info sub-section (§9.2 / §9.3 / §9.4 respectively), not to the debug blob as a whole and not absolute |

### 4.5 Function Source Table (`functionSourceCount` × 8 B)

Preserves Hermes's per-function `"show source"` / `"hide source"` /
`"sensitive"` pragma directives (the same mechanism as `"use strict"`, but
controlling whether `Function.prototype.toString()` can see the original
source for that function).

| Field | Off | B | Notes |
|---|---|---|---|
| `functionID` | 0x0 | 4 | index into the function table |
| `stringID` | 0x4 | 4 | index into the String Table — an **empty** string here marks "hide source" / "sensitive"; a non-empty string is the literal preserved source text |

---

## 5. String Table

Four parallel structures plus one raw blob, together encoding every string
literal, identifier and property name in the file.

### 5.1 String Kind Table (`stringKindCount` × 4 B)

A run-length encoding over the string table (§5.3): consecutive strings of
the same *kind* (used as a property/variable name, vs. a plain value) are
collapsed into one entry.

| Bits | Field | Notes |
|---|---|---|
| 0–30 | `count` | number of consecutive strings (in String Table order, starting right after the previous run) sharing this kind |
| 31 | `kind` | `0` = `String` (a plain value, e.g. a string literal), `1` = `Identifier` (used as a property name or variable name somewhere in the bytecode) |

### 5.2 Identifier Hash Table (`identifierCount` × 4 B)

One `u32` hash per `Identifier`-kind string (§5.1), in the same relative
order those identifiers appear in the String Table. Hermes computes this
hash once at compile time and the VM uses it to speed up property lookups
without re-hashing the string at runtime.

> **NOTE — hash algorithm not yet reverse-engineered.** hermit's parser
> captures this table verbatim (`idHash_`) for round-trip purposes but does
> not currently recompute or verify individual hash values. This blocks
> bonus item **B3** ("Identifier strings: add, modify, delete" in
> [`../phases/phase-bonus/README.md`](../phases/phase-bonus/README.md)):
> adding or renaming an `Identifier` requires reproducing the exact hash the
> Hermes compiler would have produced, or the VM's property cache may behave
> incorrectly. The leading hypothesis is the Jenkins-hash family from
> `hermes/Support/JenkinsHash.h` in the v96 source, to be confirmed by
> recomputing the hash of every `Identifier` in a real file (`ridemovi.hbc`)
> and comparing byte-for-byte against the stored `idHash_` before writing
> any encoder. It is also unconfirmed whether the hash is computed over the
> 8-bit bytes, the UTF-16 code units, or differs by `isUTF16` (§5.3).

### 5.3 String Table (`stringCount` × 4 B, `SmallStringTableEntry`)

| Bits | Field | Notes |
|---|---|---|
| 0 | `isUTF16` | `0` = the string is stored in CESU-8 (Modified UTF-8 — ASCII-compatible for the ASCII subset, but not standard UTF-8 for surrogate-pair characters); `1` = stored as UTF-16LE in String Storage |
| 1–23 | `offset` | into String Storage (§5.5) |
| 24–31 | `length` | **in characters**, not bytes; the sentinel value `255` means "look up the real offset/length in the Overflow String Table instead" (§5.4) |

Real React Native bundles make real use of both encodings: `ridemovi.hbc`
has 9198 UTF-16 entries out of 54719 total strings, and 647 entries that
hit the overflow sentinel (one observed example: a 458-character inlined
CSS block).

### 5.4 Overflow String Table (`overflowStringCount` × 8 B)

| Field | Off | B | Notes |
|---|---|---|---|
| `offset` | 0x0 | 4 | into String Storage |
| `length` | 0x4 | 4 | in characters |

Correspondence with the String Table (§5.3) is **purely positional**: the
*N*-th `SmallStringTableEntry` with `length == 255` (scanning in table
order) is paired with the *N*-th entry of this table. `isUTF16` is **not**
duplicated here — it stays on the small entry even when the real
offset/length live in the overflow table.

### 5.5 String Storage (`stringStorageSize` B)

A single raw blob holding every string's encoded bytes back to back, with
**no separators** — individual string boundaries exist only through the
`(offset, length)` pairs of §5.3/§5.4, never through a terminator byte.
Each string is encoded per its own `isUTF16` flag (CESU-8 or UTF-16LE); two
different strings' encoded bytes are allowed to overlap when one is a
suffix or prefix of the other, which Hermes's compiler exploits as a space
optimization.

---

## 6. Literal Buffers

Three buffers — Array Buffer, Object Key Buffer, Object Value Buffer — each
holding a sequence of `SerializedLiteral` records (the "SLP" format), used
by `NewArrayWithBuffer[Long]` / `NewObjectWithBuffer[Long]` to materialize
array/object literals without emitting one instruction per element. Each
instruction's `bufferIdx` (and, for objects, `keyBufferIdx`) operand is a
byte offset into the relevant buffer where its own run of entries starts.

### 6.1 Tag byte

Every record starts with one tag byte, optionally followed by a second
length byte, then by `len` fixed-size element values of the same tag — i.e.
the format runs as alternating homogeneous runs ("3 numbers, then 2
strings, then 1 bool, ..."), not as one tag per element.

| Bits | Field | Notes |
|---|---|---|
| 7 | `ext` | `0`: `len` is the 4-bit field below; `1`: `len` is 12 bits wide — the low 4 bits here combine with a second byte right after the tag byte to form the full count |
| 4–6 | `tag` | element type, see below |
| 0–3 | `len` | element count of this run (low 4 bits; see `ext` above for the extended form) |

| Tag | Name | Bytes/element | Value |
|---|---|---|---|
| 0 | `Null` | 0 | — |
| 1 | `True` | 0 | — |
| 2 | `False` | 0 | — |
| 3 | `Number` | 8 | `f64` |
| 4 | `LongString` | 4 | String Table index, `u32` |
| 5 | `ShortString` | 2 | String Table index, `u16` |
| 6 | `ByteString` | 1 | String Table index, `u8` |
| 7 | `Integer` | 4 | `i32` (signed — a run of small negative array/object values is still tag `Integer`, not a separate signed/unsigned distinction) |

This format is documented in the Hermes source
(`SerializedLiteralGenerator.h`), not invented by hermit; hermit's decoder
lives in `cli/literals.py`.

---

## 7. BigInt Table and Storage — **NOT YET EXPLORED**

| Field (`BigIntTableEntry`, 8 B × `bigIntCount`) | Notes |
|---|---|
| `offset` (`u32`) | relative to the start of BigInt storage |
| `length` (`u32`) | in bytes |

hermit's parser sizes this section (for correct round-trip and
re-serialization) but never decodes an individual entry's bytes into a
value. `header.bigIntCount` has been **0 in every file observed so far**,
including `ridemovi.hbc` — a real, large production bundle — so there is
currently no real sample from which to reverse the on-disk encoding of an
arbitrary-precision BigInt literal (the leading hypothesis is a
sign+magnitude byte encoding, unverified). The decompiler already flags
this gap honestly rather than guessing: a `LoadConstBigInt[LongIndex]`
instruction is rendered as `r0 = 0n; /* BigInt table #N, not resolved */` —
visibly wrong but never silently passed off as the real value.

**To unblock:** produce a `.hbc` file that actually contains a BigInt
literal (e.g. `123456789012345678901234567890n` in a seeder sample compiled
with `hermes -emit-binary`) and inspect the resulting `bigIntCount` and
storage bytes before writing a decoder. See bonus item **B20** in
[`../phases/phase-bonus/README.md`](../phases/phase-bonus/README.md).

---

## 8. RegExp Table and Storage — **NOT YET EXPLORED**

| Field (`RegExpTableEntry`, 8 B × `regExpCount`) | Notes |
|---|---|
| `offset` (`u32`) | relative to the start of RegExp storage |
| `length` (`u32`) | in bytes |

`ridemovi.hbc` has 461 entries (3688 bytes of table, 58455 bytes of
storage). The section's content is **not** the regex's source pattern or
flags — those live as ordinary String Table entries, referenced directly by
the `patternIdx`/`flagsIdx` operands of the `CreateRegExp` instruction
(§13). What this section's storage holds is Hermes's own **precompiled
regex bytecode** (its internal regex execution format), referenced by
`CreateRegExp`'s third operand `regexpIdx`. The entry layout (`offset`,
`length`) is known from the struct definition; the byte format of the
precompiled bytecode itself has not been decoded. See bonus item **B20**.

---

## 9. CJS Module Table

Hermes supports a second, native way of compiling a set of JS files into one
or more CJS modules, entirely independent of Metro's bundling convention
(the one real React Native apps use — see §9.1). This section exists when
that native mode is used. It is **not** a hermit discovery: the full
mechanism is documented in the Hermes source tree itself,
`doc/Modules.md`, and in `include/hermes/BCGen/HBC/Bytecode.h`,
`lib/VM/Domain.cpp`, `lib/CompilerDriver/CompilerDriver.cpp`. Confirmed by
reading that source and by compiling real samples with the `hermes` v96
binary (§9.4).

### 9.1 How a file ends up with `cjsModuleCount > 0`

Native CJS mode is activated with the `-commonjs` compiler flag, given a
directory or zip containing a `metadata.json` manifest:

```json
{
  "segments": {
    "0": ["./main.js", "./foo.js"]
  }
}
```

`segments` maps a segment ID (§9.3) to the list of JS files placed in it;
segment `"0"` is the main segment, loaded at startup, and its first file is
the entry point. Each listed file is wrapped by the compiler in a CJS
module function `(exports, require, module) => { ... }` (`wrapCJSModule`)
and registered with a module ID (sequential by default, or taken from an
optional `moduleIDs` map in the manifest).

**This has no relationship to how React Native actually ships code.** A
Metro-produced bundle is a single JS file using Metro's own
`__d(factory, moduleId, deps)` wrapper convention, handed to `hermes` as
ordinary input (no `-commonjs`). `HBCFile::recover_modules()` in hermit
resolves module boundaries entirely through that `__d()` pattern by
bytecode matching (`TryGetById "__d"` → `CreateClosure` → `LoadConst*` →
`Call4`), never through this section. The two module systems are mutually
exclusive at the compiler-invocation level, which is why `cjsModuleCount`
is 0 in every Metro-produced file ever observed, including `ridemovi.hbc`.

### 9.2 On-disk entry format — two variants, never both populated

8 bytes per entry, `{first:u32, second:u32}`, but the *meaning* of the pair
depends on `options.cjsModulesStaticallyResolved` (§3). Hermes's own
serializer (`BytecodeStream.cpp`, `serializeCJSModuleTable`) writes
whichever of its two in-memory tables is non-empty — the other is always
empty, so only one shape is ever actually on disk in a given file:

| `cjsModulesStaticallyResolved` | Entry meaning | Populated by |
|---|---|---|
| `0` (dynamic / "slow require") | `(stringID, functionID)` — `stringID` is a String Table index holding the module's file path, `functionID` is the function table index of that module's wrapper function | `BytecodeModuleGenerator::addCJSModule` |
| `1` (static / "fast require") | `(moduleID, functionID)` — `moduleID` is a small compiler-assigned integer (not a string), `functionID` as above | `BytecodeModuleGenerator::addCJSModuleStatic` |

At runtime (`lib/VM/Domain.cpp`, `Domain::importCJSModuleTable`): the
dynamic form backs `require("path")` — the path string is turned into a
`SymbolID` and looked up in a `DenseMap<SymbolID, uint32_t>` to find the
module's slot. The static form backs `require.context`'s
`HermesBuiltin_requireFast(moduleID)` — a direct indexed lookup into an
`ArrayStorage`, no string hashing at all. Both forms ultimately populate
the same per-`Domain` module array (`cjsModules_`, one `{cachedExports,
module, functionIndex}` triplet per module); only the lookup path differs.

### 9.3 `segmentID` and multi-segment loading

`segmentID` (header field, §3) does **not** change this document's file
layout in any way — every segment is a complete, independent, fully
self-contained `.hbc` file (full header, every section). It is purely a
diagnostic tag, read in exactly these places in the Hermes source and
nowhere else: stack-trace frame construction (`JSError.cpp`,
`StackTracesTree.cpp`, +1 for 1-based display), source-map generation
(`SourceMapGenerator`), the sampling profiler's packed frame address
(`frameAddress = (segmentID << 32) + virtualOffset`, `SamplingProfiler*.cpp`)
and code-coverage symbolication (`CodeCoverageProfiler.cpp`). None of these
consumers reinterpret any *other* section based on this value — confirmed
both by reading every `getSegmentID()` call site and by compiling and
byte-inspecting a genuine two-segment sample (§9.4): the non-zero-segment
file parses with the exact same `section_map()` this document already
describes.

Segment 0 is loaded at startup; segment N > 0 is loaded on demand via the
`Domain::loadSegment` runtime API, reachable from JS through the
`require.context` object bound inside every CJS module function. The
compiler emits one physical file per segment when asked to compile more
than one: `<out>` for segment 0, `<out>.<segmentID>` for every other
segment (`CompilerDriver.cpp`), optionally alongside a JSON manifest
listing them (`-bytecode-output-manifest`).

### 9.4 Verified samples

Three source trees, compiled with the project's `hermes` v96 binary
(`hermes -commonjs [-fstatic-require] [-O] -emit-binary -out <file> <dir>`),
decoded with a throwaway script reading the header fields and the raw CJS
Module Table bytes directly (no format assumptions beyond what §2/§3
already state):

**`cjs-dynamic.hbc`** — `main.js` doing `require('./foo.js')`, `foo.js`
exporting a `greet` function:

```
cjsModuleCount=2  options=0b00000000 (cjsModulesStaticallyResolved=0)
CJS Module Table @ offset 300, 16 bytes
raw hex: 06000000 01000000 02000000 02000000
  entry[0] = (stringID=6, functionID=1)   # strings()[6] = 'main.js', functions()[1].name = 'cjs_module'
  entry[1] = (stringID=2, functionID=2)   # strings()[2] = 'foo.js',  functions()[2].name = 'cjs_module'
```

Every CJS module wrapper function is literally named `cjs_module` in the
function table (not the file name) — the file name lives only in this
table's `stringID`, and the stack-trace machinery resolves it from there,
not from `functionName`.

**`cjs-static.hbc`, same source, compiled with `-fstatic-require -O` and
with an explicit `moduleIDs` map in `metadata.json`** — byte-for-byte
**identical** to the dynamic case above, `cjsModulesStaticallyResolved`
still `0`. This is not a sample-selection problem: it reproduces across
every combination tried (`-fstatic-require` alone, with `-O`, with
`-Og`, with and without an explicit `moduleIDs` map). The cause is in the
public Hermes source itself, confirmed by reading it directly
(`include/hermes/AST/Context.h`):

```cpp
/// A hack to disable CJS modules while preserving the same interface.
void setUseCJSModules(bool useCJSModules) {}
bool getUseCJSModules() const {
  return false;
}
```

`ResolveStaticRequire::runOnModule` (`lib/Optimizer/Scalar/ResolveStaticRequire.cpp`)
bails out unconditionally when `getUseCJSModules()` is false:

```cpp
bool ResolveStaticRequire::runOnModule(Module *M) {
  if (!M->getContext().getUseCJSModules() ||
      !M->getContext().getOptimizationSettings().staticRequire) {
    return false;
  }
  ...
}
```

So in the public Hermes source at this revision (and this reads as a
permanent, intentional disablement, not a transient bug — the comment says
so explicitly), **the static/`requireFast` variant of this table can never
be emitted by the standard CLI compiler**, regardless of
`-fstatic-require`. Only the dynamic `(stringID, functionID)` form is
reachable in practice. Whether some other Hermes build (a fork, or an
internal Meta build predating this hack) can still emit it is unknown and
irrelevant to this project's target (public `hermes`/`hermesc`, v94–96).

**`cjs-segmented.hbc` / `cjs-segmented.hbc.1`** — two-segment compile
(`metadata.json` with `segments: {"0": ["./main.js"], "1": ["./lazy.js"]}`,
no `-fstatic-require`):

```
cjs-segmented.hbc     segmentID=0  cjsModuleCount=1  entry[0]=(stringID=3 'main.js', functionID=1)
cjs-segmented.hbc.1   segmentID=1  cjsModuleCount=1  entry[0]=(stringID=2 'lazy.js', functionID=1)
```

Both files parse cleanly with the layout already described in §2/§3/§5 —
confirming §9.3's claim that `segmentID` changes no other section's shape.

---

## 10. Bytecode section — see §2 and §4

Covered under §2 (overall layout) and §4.3/§4.4 (per-function jump tables
and function-info sections, which live physically inside this span).

---

## 11. Debug Info

Located at the absolute offset `debugInfoOffset` (§3) — the one section the
header locates directly rather than by cumulative summation. Entirely
optional at the file level: a function only has debug info if its own
`flags.hasDebugInfo` bit is set (§4.1) and its function-info section (§4.4)
carries a valid offset triple into the three cumulative sub-sections below.

### 11.1 Debug Info Header (7 × `u32`, 28 bytes)

| Field | Off | B | Notes |
|---|---|---|---|
| `filenameCount` | 0x00 | 4 | number of distinct source file names |
| `filenameStorageSize` | 0x04 | 4 | bytes of the filename storage blob |
| `fileRegionCount` | 0x08 | 4 | number of `FileRegion` records |
| `sourceLocDataEnd` | 0x0C | 4 | **cumulative** end offset (relative to the start of the data blob, see below) of the Source Location stream |
| `scopeDescDataEnd` | 0x10 | 4 | cumulative end of the Scope Descriptor data |
| `textifiedCalleesEnd` | 0x14 | 4 | cumulative end of the Textified Callees data |
| `stringTableEnd` | 0x18 | 4 | cumulative end of the Debug String Table = total blob size |

Each of the last four fields marks the *end* of its own sub-section, which
is simultaneously the *start* of the next one — Hermes's own source calls
the same four quantities `scopeDescDataOffset`, `textifiedCalleeOffset`,
`stringTableOffset` and `debugDataSize` (read as starts rather than ends;
numerically identical).

Physical layout from `debugInfoOffset`:

```
debugInfoOffset
  + 0                                     DebugInfoHeader (28 B, above)
  + 28                                    StringTableEntry[filenameCount]  (8 B each: {offset:u32, length:u32} into filename storage)
  + 28 + filenameCount*8                  filename storage (filenameStorageSize B, \0-free utf8 names back to back)
  + ... + filenameStorageSize             FileRegion[fileRegionCount]      (12 B each: {filenameId, line, virtualOffset}, see below)
  + ... + fileRegionCount*12              "blobStart": data blob, laid out as the four
                                           cumulative sub-sections below
```

Inside the data blob, relative to `blobStart`:

| Range | Content |
|---|---|
| `[0, sourceLocDataEnd)` | Source Location stream (§11.2) |
| `[sourceLocDataEnd, scopeDescDataEnd)` | Scope Descriptor data (§11.3) |
| `[scopeDescDataEnd, textifiedCalleesEnd)` | Textified Callees data (§11.4) |
| `[textifiedCalleesEnd, stringTableEnd)` | Debug String Table (§11.5) |

### `FileRegion` and multi-file source — **NOT YET EXPLORED**

`FileRegion` (12 bytes: `filenameId:u32, line:u32, virtualOffset:u32`) and
the `StringTableEntry[filenameCount]` array that locates each source file's
name are present in the on-disk layout above, but hermit's parser only uses
them to **skip past** their byte ranges when computing
`filenameStorageStart`/`fileRegionsStart`/`blobStart` — it never decodes an
individual `FileRegion`, and `HBCFile::source_files()` rebuilds the filename
list by scanning the filename storage blob for `\0` separators in
appearance order rather than honestly indexing `StringTableEntry[]`. This
works today only because Metro always concatenates every module into one
synthetic source file before invoking Hermes, so `filenameCount == 1` in
every sample seen so far — a different value has never been observed, in
the seeder corpus or in `ridemovi.hbc`. Consistently, hermit's own
`SourceLocation` struct has no file/`filenameId` field at all, and every
consumer uniquely assumes `source_files()[0]`.

**To unblock:** build an `.hbc` with `filenameCount > 1` by invoking
`hermes` on several separate `.js` sources directly (bypassing Metro), to
get real `FileRegion` values. The leading hypothesis is that
`virtualOffset` is a cumulative "logical" offset across the concatenated
sources, compared against the resolved bytecode address to pick the right
`FileRegion` (and thus file) — unconfirmed. See bonus item **B17**.

### 11.2 Source Location stream (SLEB128, delta-encoded)

A per-function header, followed by a record stream terminated by a
sentinel delta.

| Field | Type | Notes |
|---|---|---|
| **Per-function header** | | |
| `functionIndex` | SLEB128 | |
| `start_line` | SLEB128 | absolute, 1-based |
| `start_col` | SLEB128 | absolute, 1-based |
| **Record (repeated until the terminator)** | | |
| `addr_delta` | SLEB128 | `0x7F` (decoded value `−1`) terminates this function's records |
| `line_delta_raw` | SLEB128 | bit 0 is a presence flag for the trailing `stmt_delta` field; the actual line delta is `line_delta_raw >> 1` |
| `col_delta` | SLEB128 | |
| `scope_address` | SLEB128 | absence is represented as `0xFFFFFFFF`, not by omitting the field |
| `env_register` | SLEB128 | |
| `stmt_delta` | SLEB128 | present only if `line_delta_raw` bit 0 is set |

hermit resolves a bytecode address to a line/column by delta-accumulating
this stream and picking the *nearest preceding* record (binary search over
the accumulated addresses) — there is no record for every address, only for
the ones where source position actually changes.

### 11.3 Scope Descriptor

| Field | Type | Notes |
|---|---|---|
| `parentId` | SLEB128 | `−1` = no parent scope; otherwise a byte offset into this same section, identifying the parent descriptor |
| `flags` | SLEB128 | bit 0 = `InnerScope`, bit 1 = `Dynamic` (confirmed against the Hermes source's `DebugScopeDescriptor::Flags`, tag `hermes-2023-09-26-RNv0.73.0`) |
| `varNamesCount` | SLEB128 | |
| `nameOffset × varNamesCount` | SLEB128 each | byte offset into the Debug String Table (§11.5) for each captured/declared variable's name, in declaration order |

> **`flags` — bit meaning confirmed, non-zero value never observed.** Every
> sample compiled so far (seeder corpus, `ridemovi.hbc`) has `flags == 0`
> for every scope. The two-bit meaning above is taken directly from the
> Hermes compiler source, not guessed — but no corpus sample has ever
> exercised `InnerScope` or `Dynamic` being set, so there is no empirical
> confirmation of *which JS construct* sets them (candidate hypotheses: a
> `var`/`let`/`const` distinction at the same block level, a captured
> parameter vs. a captured local, or a closure nested inside try/catch or a
> generator — none tested). hermit does not currently need this distinction
> for its own use (captured-variable *names* for the decompiler, resolved
> correctly regardless of these bits), but exposing and verifying it remains
> open. See bonus item **B18**.

> **Related gap — outer-scope resolution.** `scope_variables()` resolves
> names only for a function's *own* `CreateEnvironment`; a nested function
> reading a captured variable of an *outer* function via
> `GetEnvironment(dest, levelsUp > 0)` currently falls back to a generic
> `envSlot_N` placeholder rather than walking the `parentId` chain above to
> find the real name. This is a decompiler-side limitation (see bonus item
> **B19**), not a format gap — the format already carries enough
> information (`parentId` links between `Scope Descriptor` entries) to
> resolve it, it is simply not wired up yet.

### 11.4 Textified Callees

| Field | Type | Notes |
|---|---|---|
| `count` | SLEB128 | |
| `(bcOffset, nameOffset) × count` | SLEB128 each | `bcOffset`: bytecode offset of a call instruction, relative to the start of the calling function's bytecode. `nameOffset`: byte offset into the Debug String Table (§11.5) giving the **callee's textual name**, independent of whether the call target is statically known — this is what lets Hermes print a readable stack-trace frame name even for a call through a register holding an unresolved closure |

### 11.5 Debug String Table

| Field | Type | Notes |
|---|---|---|
| `len` | SLEB128 | byte length |
| data | `u8 × len` | UTF-8, **no terminator byte** — `len` is the only boundary |

Every `nameOffset`/`fnNameOffset` elsewhere in the debug-info section (§11.3,
§11.4) is a **byte offset** into this sub-section's own data blob, not an
index into a logical list of entries — resolving a name means seeking to
that byte offset and reading one `(len, data)` record from there.

---

## 12. File Footer

20 bytes, the file's last 20 bytes: a SHA-1 digest of every byte that
precedes it. **The Hermes runtime does not verify this digest when loading
a bytecode file** — it exists for integrity *tooling*, not for execution-time
protection. (See `doc/security/integrity-ios.md` in the thesis repository
for the full implication: no bytecode-integrity check exists anywhere in
the shipped runtime, on either platform.) hermit's patcher recomputes this
digest on every write so the file stays internally consistent, even though
nothing downstream checks it.

---

## 13. Instructions

### 13.1 Encoding

Every instruction is `opcode:u8` followed by its operands, in the order
given in the opcode table (§13.3), with **no padding** between operands and
none between consecutive instructions. The "Size (B)" column in §13.3
already includes the leading opcode byte.

**Variants.** A `Long` / `LongIndex` suffix (e.g. `GetByIdLong`,
`CreateClosureLongIndex`) denotes a *distinct opcode*, not a flag on the
base one — same semantics as its short counterpart, wider operand(s) to
reach indices the short form's narrower field cannot address. Every
conditional and unconditional jump exists in exactly two such variants: a
short form taking an `Addr8` target and a long form (suffixed `Long`)
taking an `Addr32` target — both relative to the start of the jump
instruction itself (§4.3 for the one structural exception, `SwitchImm`'s
externally-stored jump table, which follows the same "relative to the
instruction" convention for its targets).

### 13.2 Opcode count

206 opcodes are defined for v96, out of 256 possible byte values;
`0xCE`–`0xFF` are unassigned. The table below is generated directly from
hermit's own per-version opcode table
(`core/src/format/opcodes_v96.cpp`), cross-referenced against the semantic
comment attached to each opcode's definition in the Hermes compiler source
(`BytecodeList.def`, same `hermes-2023-09-26-RNv0.73.0` tag as the rest of
this document) — the "Meaning" column is Hermes's own description of what
the instruction does, not a guess from the operand shape alone.

A handful of opcode families have **known, intentional gaps** in semantic
reconstruction further downstream in the decompiler (not in this format
description, which covers every defined opcode): the 14 WASM/asm.js
load/store opcodes (`0xC0`–`0xCD`, present only because this Hermes build
enables `HERMES_RUN_WASM`, never observed emitted by ordinary JS), the 11
generator/`for-in`/`for-of` opcodes (whose *body* reconstruction as JS
control flow is unimplemented — note `SaveGenerator`'s jump target is
classified below as a non-jump `Addr8` operand, which may mean the current
CFG builder misses a real generator re-entry edge, not yet empirically
confirmed), `PutOwnGetterSetterByVal` (getter/setter object-literal
definition), and `ThrowIfHasRestrictedGlobalProperty` (an ECMAScript-spec
guard, likely emission-paired with `DeclareGlobalVar`). See bonus items
**B21–B25** for the decompiler-side detail; this document records what each
opcode *means*, independent of whether hermit's decompiler currently
reconstructs idiomatic JS for it.

### 13.3 Opcode table

`Op` = opcode byte (hex). `Operands` lists each operand's physical type in
encoding order; a trailing `*` marks a String Table index (§1).

<!-- GENERATED by gen_opcode_md.py from core/src/format/opcodes_v96.cpp
     and references/hermes BytecodeList.def (hermes-2023-09-26-RNv0.73.0 tag).
     206 opcodes defined out of 256. Do not edit by hand. -->

| Op | Mnemonic | Operands | Size (B) | Meaning |
|----|----------|----------|----------|---------|
| `00` | `Unreachable` | `—` | 1 | Unreachable opcode for stubs and similar. This is first so that it has the value zero. |
| `01` | `NewObjectWithBuffer` | `Reg8, UInt16, UInt16, UInt16, UInt16` | 10 | Create an object from a static map of values, as for var={'a': 3}. Any non-constant elements can be set afterwards with PutOwnByInd. Arg1 is the destination. Arg2 is a preallocation size hint. Arg3 is the number of static elements. Arg4 is the index in the object key buffer table. Arg5 is the index in the object val buffer table. |
| `02` | `NewObjectWithBufferLong` | `Reg8, UInt16, UInt16, UInt32, UInt32` | 14 | Create an object from a static map of values, as for var={'a': 3}. Any non-constant elements can be set afterwards with PutOwnByInd. Arg1 is the destination. Arg2 is a preallocation size hint. Arg3 is the number of static elements. Arg4 is the index in the object key buffer table. Arg5 is the index in the object val buffer table. |
| `03` | `NewObject` | `Reg8` | 2 | Create a new, empty Object using the built-in constructor (regardless of whether it was overridden). Arg1 = {} |
| `04` | `NewObjectWithParent` | `Reg8, Reg8` | 3 | Create a new empty Object with the specified parent. If the parent is null, no parent is used. If the parent is not an object, the builtin Object.prototype is used. Otherwise the parent itself is used. Arg1 = the created object Arg2 = the parent. |
| `05` | `NewArrayWithBuffer` | `Reg8, UInt16, UInt16, UInt16` | 8 | Create an array from a static list of values, as for var=[1,2,3]. Any non-constant elements can be set afterwards with PutOwnByIndex. Arg1 is the destination. Arg2 is a preallocation size hint. Arg3 is the number of static elements. Arg4 is the index in the array buffer table. |
| `06` | `NewArrayWithBufferLong` | `Reg8, UInt16, UInt16, UInt32` | 10 | Create an array from a static list of values, as for var=[1,2,3]. Any non-constant elements can be set afterwards with PutOwnByIndex. Arg1 is the destination. Arg2 is a preallocation size hint. Arg3 is the number of static elements. Arg4 is the index in the array buffer table. |
| `07` | `NewArray` | `Reg8, UInt16` | 4 | Create a new array of a given size. Arg1 = new Array(Arg2) |
| `08` | `Mov` | `Reg8, Reg8` | 3 | Arg1 = Arg2 (Register copy) |
| `09` | `MovLong` | `Reg32, Reg32` | 9 | Arg1 = Arg2 (Register copy, long index) |
| `0A` | `Negate` | `Reg8, Reg8` | 3 | Arg1 = -Arg2 (Unary minus) |
| `0B` | `Not` | `Reg8, Reg8` | 3 | Arg1 = !Arg2 (Boolean not) |
| `0C` | `BitNot` | `Reg8, Reg8` | 3 | Arg1 = ~Arg2 (Bitwise not) |
| `0D` | `TypeOf` | `Reg8, Reg8` | 3 | Arg1 = typeof Arg2 (JS typeof) |
| `0E` | `Eq` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 == Arg3 (JS equality) |
| `0F` | `StrictEq` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 === Arg3 (JS strict equality) |
| `10` | `Neq` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 != Arg3 (JS inequality) |
| `11` | `StrictNeq` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 !== Arg3 (JS strict inequality) |
| `12` | `Less` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 < Arg3 (JS less-than) |
| `13` | `LessEq` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 <= Arg3 (JS less-than-or-equals) |
| `14` | `Greater` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 > Arg3 (JS greater-than) |
| `15` | `GreaterEq` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 >= Arg3 (JS greater-than-or-equals) |
| `16` | `Add` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 + Arg3 (JS addition/concatenation) |
| `17` | `AddN` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 + Arg3 (Numeric addition, skips number check) |
| `18` | `Mul` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 * Arg3 (JS multiplication) |
| `19` | `MulN` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 * Arg3 (Numeric multiplication, skips number check) |
| `1A` | `Div` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 / Arg3 (JS division) |
| `1B` | `DivN` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 / Arg3 (Numeric division, skips number check) |
| `1C` | `Mod` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 % Arg3 (JS remainder) |
| `1D` | `Sub` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 - Arg3 (JS subtraction) |
| `1E` | `SubN` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 - Arg3 (Numeric subtraction, skips number check) |
| `1F` | `LShift` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 << Arg3 (JS bitshift left) |
| `20` | `RShift` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 >> Arg3 (JS signed bitshift right) |
| `21` | `URshift` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 >>> Arg3 (JS unsigned bitshift right) |
| `22` | `BitAnd` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 & Arg3 (JS bitwise AND) |
| `23` | `BitXor` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 ^ Arg3 (JS bitwise XOR) |
| `24` | `BitOr` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 \| Arg3 (JS bitwise OR) |
| `25` | `Inc` | `Reg8, Reg8` | 3 | Arg1 = Arg2 + 1 (JS increment, skips number check) |
| `26` | `Dec` | `Reg8, Reg8` | 3 | Arg1 = Arg2 - 1 (JS decrement, skips number check) |
| `27` | `InstanceOf` | `Reg8, Reg8, Reg8` | 4 | Check whether Arg2 contains Arg3 in its prototype chain. Note that this is not the same as JS instanceof. Pseudocode: Arg1 = prototypechain(Arg2).contains(Arg3) |
| `28` | `IsIn` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 in Arg3 (JS relational 'in') |
| `29` | `GetEnvironment` | `Reg8, UInt8` | 3 | Get an environment (scope) from N levels up the stack. 0 is the current environment, 1 is the caller's environment, etc. |
| `2A` | `StoreToEnvironment` | `Reg8, UInt8, Reg8` | 4 | Store a value in an environment. StoreNPToEnvironment[L] store a non-pointer value in an environment Arg1 is the environment (as fetched by GetEnvironment). Arg2 is the environment index slot number. Arg3 is the value. |
| `2B` | `StoreToEnvironmentL` | `Reg8, UInt16, Reg8` | 5 | Store a value in an environment. StoreNPToEnvironment[L] store a non-pointer value in an environment Arg1 is the environment (as fetched by GetEnvironment). Arg2 is the environment index slot number. Arg3 is the value. |
| `2C` | `StoreNPToEnvironment` | `Reg8, UInt8, Reg8` | 4 | Store a value in an environment. StoreNPToEnvironment[L] store a non-pointer value in an environment Arg1 is the environment (as fetched by GetEnvironment). Arg2 is the environment index slot number. Arg3 is the value. |
| `2D` | `StoreNPToEnvironmentL` | `Reg8, UInt16, Reg8` | 5 | Store a value in an environment. StoreNPToEnvironment[L] store a non-pointer value in an environment Arg1 is the environment (as fetched by GetEnvironment). Arg2 is the environment index slot number. Arg3 is the value. |
| `2E` | `LoadFromEnvironment` | `Reg8, Reg8, UInt8` | 4 | Load a value from an environment. Arg1 is the destination. Arg2 is the environment (as fetched by GetEnvironment). Arg3 is the environment index slot number. |
| `2F` | `LoadFromEnvironmentL` | `Reg8, Reg8, UInt16` | 5 | Load a value from an environment. Arg1 is the destination. Arg2 is the environment (as fetched by GetEnvironment). Arg3 is the environment index slot number. |
| `30` | `GetGlobalObject` | `Reg8` | 2 | Get the global object (the object in which global variables are stored). |
| `31` | `GetNewTarget` | `Reg8` | 2 | Obtain the value of NewTarget from the frame. Arg1 = NewTarget |
| `32` | `CreateEnvironment` | `Reg8` | 2 | Create a new environment, to store values captured by closures. |
| `33` | `CreateInnerEnvironment` | `Reg8, Reg8, UInt32` | 7 | Create a new inner environment, to store values captured by closures. Arg1 is the destination. Arg2 is the parent environment. Arg3 is the number of slots in the environment. |
| `34` | `DeclareGlobalVar` | `UInt32*` | 5 | Declare a global variable by string table index. The variable will be set to undefined. |
| `35` | `ThrowIfHasRestrictedGlobalProperty` | `UInt32*` | 5 | Checks if globalThis has a restricted global property with the given name. Arg1 is the name to check. |
| `36` | `GetByIdShort` | `Reg8, Reg8, UInt8, UInt8*` | 5 | Get an object property by string table index. Arg1 = Arg2[stringtable[Arg4]] Arg3 is a cache index used to speed up the above operation. |
| `37` | `GetById` | `Reg8, Reg8, UInt8, UInt16*` | 6 | Get an object property by string table index. Arg1 = Arg2[stringtable[Arg4]] Arg3 is a cache index used to speed up the above operation. |
| `38` | `GetByIdLong` | `Reg8, Reg8, UInt8, UInt32*` | 8 | Get an object property by string table index. Arg1 = Arg2[stringtable[Arg4]] Arg3 is a cache index used to speed up the above operation. |
| `39` | `TryGetById` | `Reg8, Reg8, UInt8, UInt16*` | 6 | Get an object property by string table index, or throw if not found. This is similar to GetById, but intended for use with global variables where Arg2 = GetGlobalObject. |
| `3A` | `TryGetByIdLong` | `Reg8, Reg8, UInt8, UInt32*` | 8 | Get an object property by string table index, or throw if not found. This is similar to GetById, but intended for use with global variables where Arg2 = GetGlobalObject. |
| `3B` | `PutById` | `Reg8, Reg8, UInt8, UInt16*` | 6 | Set an object property by string index. Arg1[stringtable[Arg4]] = Arg2. |
| `3C` | `PutByIdLong` | `Reg8, Reg8, UInt8, UInt32*` | 8 | Set an object property by string index. Arg1[stringtable[Arg4]] = Arg2. |
| `3D` | `TryPutById` | `Reg8, Reg8, UInt8, UInt16*` | 6 | Set an object property by string index, or throw if undeclared. This is similar to PutById, but intended for use with global variables where Arg1 = GetGlobalObject. |
| `3E` | `TryPutByIdLong` | `Reg8, Reg8, UInt8, UInt32*` | 8 | Set an object property by string index, or throw if undeclared. This is similar to PutById, but intended for use with global variables where Arg1 = GetGlobalObject. |
| `3F` | `PutNewOwnByIdShort` | `Reg8, Reg8, UInt8*` | 4 | Create a new own property on an object. This is similar to PutById, but the destination must be an object, it only deals with own properties, ignoring the prototype chain, and the property must not already be defined. Similarly to PutById, the property name cannot be a valid array index. Arg1 is the destination object, which is known to be an object. Arg2 is the value to write. Arg3 is the string table ID of the property name. Arg1[stringtable[Arg3]] = Arg2 |
| `40` | `PutNewOwnById` | `Reg8, Reg8, UInt16*` | 5 | Create a new own property on an object. This is similar to PutById, but the destination must be an object, it only deals with own properties, ignoring the prototype chain, and the property must not already be defined. Similarly to PutById, the property name cannot be a valid array index. Arg1 is the destination object, which is known to be an object. Arg2 is the value to write. Arg3 is the string table ID of the property name. Arg1[stringtable[Arg3]] = Arg2 |
| `41` | `PutNewOwnByIdLong` | `Reg8, Reg8, UInt32*` | 7 | Create a new own property on an object. This is similar to PutById, but the destination must be an object, it only deals with own properties, ignoring the prototype chain, and the property must not already be defined. Similarly to PutById, the property name cannot be a valid array index. Arg1 is the destination object, which is known to be an object. Arg2 is the value to write. Arg3 is the string table ID of the property name. Arg1[stringtable[Arg3]] = Arg2 |
| `42` | `PutNewOwnNEById` | `Reg8, Reg8, UInt16*` | 5 | Create a new non-enumerable own property on an object. This is the same as PutNewOwnById, but creates the property with different enumerability. Arg1 is the destination object. Arg2 is the value to write. Arg3 is the string table ID of the property name. Arg1[stringtable[Arg3]] = Arg2 |
| `43` | `PutNewOwnNEByIdLong` | `Reg8, Reg8, UInt32*` | 7 | Create a new non-enumerable own property on an object. This is the same as PutNewOwnById, but creates the property with different enumerability. Arg1 is the destination object. Arg2 is the value to write. Arg3 is the string table ID of the property name. Arg1[stringtable[Arg3]] = Arg2 |
| `44` | `PutOwnByIndex` | `Reg8, Reg8, UInt8` | 4 | Assign a value to a constant integer own property which will be created as enumerable. This is used (potentially in conjunction with NewArrayWithBuffer) for arr=[foo,bar] initializations. Arg1[Arg3] = Arg2; |
| `45` | `PutOwnByIndexL` | `Reg8, Reg8, UInt32` | 7 | Assign a value to a constant integer own property which will be created as enumerable. This is used (potentially in conjunction with NewArrayWithBuffer) for arr=[foo,bar] initializations. Arg1[Arg3] = Arg2; |
| `46` | `PutOwnByVal` | `Reg8, Reg8, Reg8, UInt8` | 5 | Set an own property identified by value. Arg1 is the destination object. Arg2 is the value to write. Arg3 is the property name. Arg4 : bool -> enumerable. If true, the property is created as enumerable, non-enumerable otherwise. Arg1[Arg3] = Arg2; |
| `47` | `DelById` | `Reg8, Reg8, UInt16*` | 5 | Delete a property by string table index. Arg1 = delete Arg2[stringtable[Arg3]] |
| `48` | `DelByIdLong` | `Reg8, Reg8, UInt32*` | 7 | Delete a property by string table index. Arg1 = delete Arg2[stringtable[Arg3]] |
| `49` | `GetByVal` | `Reg8, Reg8, Reg8` | 4 | Get a property by value. Constants string values should instead use GetById. Arg1 = Arg2[Arg3] |
| `4A` | `PutByVal` | `Reg8, Reg8, Reg8` | 4 | Set a property by value. Constant string values should instead use GetById (unless they are array indices according to ES5.1 section 15.4, in which case this is still the right opcode). Arg1[Arg2] = Arg3 |
| `4B` | `DelByVal` | `Reg8, Reg8, Reg8` | 4 | Delete a property by value (when the value is not known at compile time). Arg1 = delete Arg2[Arg3] |
| `4C` | `PutOwnGetterSetterByVal` | `Reg8, Reg8, Reg8, Reg8, UInt8` | 6 | Add a getter and a setter for a property by value. Object.defineProperty(Arg1, Arg2, { get: Arg3, set: Arg4 }). Arg1 is the target object which will have a property defined. Arg2 is the property name Arg3 is the getter closure or undefined Arg4 is the setter closure or undefined Arg5 : boolean - if true, the property will be enumerable. |
| `4D` | `GetPNameList` | `Reg8, Reg8, Reg8, Reg8` | 5 | Get the list of properties from an object to implement for..in loop. Returns Arg1, which is the register that holds array of properties. Returns Undefined if the object is null/undefined. Arg2 is the register that holds the object. Arg3 is the register that holds the iterating index. Arg4 is the register that holds the size of the property list. |
| `4E` | `GetNextPName` | `Reg8, Reg8, Reg8, Reg8, Reg8` | 6 | Get the next property in the for..in iterator. Returns Arg1, which is the next property. Undefined if unavailable. Arg2 is the register that holds array of properties. Arg3 is the register that holds the object. Arg4 is the register that holds the iterating index. Arg5 is the register that holds the size of the property list. |
| `4F` | `Call` | `Reg8, Reg8, UInt8` | 4 | Call a function. Arg1 is the destination of the return value. Arg2 is the closure to invoke. Arg3 is the number of arguments, assumed to be found in reverse order from the end of the current frame. |
| `50` | `Construct` | `Reg8, Reg8, UInt8` | 4 | Call a constructor, with semantics identical to Call. Arg1 is the destination of the return value. Arg2 is the closure to invoke. Arg3 is the number of arguments, assumed to be found in reverse order from the end of the current frame. The first argument 'this' is assumed to be created with CreateThis. |
| `51` | `Call1` | `Reg8, Reg8, Reg8` | 4 | Call a function with one arg. Arg1 is the destination of the return value. Arg2 is the closure to invoke. Arg3 is the first argument. |
| `52` | `CallDirect` | `Reg8, UInt8, UInt16` | 5 | Call a function directly without a closure. Arg1 is the destination of the return value. Arg2 is the number of arguments, assumed to be found in reverse order from the end of the current frame. The first argument 'this' is assumed to be created with CreateThis. Arg3 is index in the function table. Note that we expect the variable-sized argument to be last. |
| `53` | `Call2` | `Reg8, Reg8, Reg8, Reg8` | 5 | Call a function with two args. Arg1 is the destination of the return value. Arg2 is the closure to invoke. Arg3 is the first argument. Arg4 is the second argument. |
| `54` | `Call3` | `Reg8, Reg8, Reg8, Reg8, Reg8` | 6 | Call a function with three args. Arg1 is the destination of the return value. Arg2 is the closure to invoke. Arg3 is the first argument. Arg4 is the second argument. Arg5 is the third argument. |
| `55` | `Call4` | `Reg8, Reg8, Reg8, Reg8, Reg8, Reg8` | 7 | Call a function with four args. Arg1 is the destination of the return value. Arg2 is the closure to invoke. Arg3 is the first argument. Arg4 is the second argument. Arg5 is the third argument. Arg6 is the fourth argument. |
| `56` | `CallLong` | `Reg8, Reg8, UInt32` | 7 | Identical to Call, but allowing more arguments. |
| `57` | `ConstructLong` | `Reg8, Reg8, UInt32` | 7 | Identical to Construct, but allowing more arguments. |
| `58` | `CallDirectLongIndex` | `Reg8, UInt8, UInt32` | 7 | Identical to CallDirect, but the function index is 32-bit. |
| `59` | `CallBuiltin` | `Reg8, UInt8, UInt8` | 4 | Call a builtin function. Note this is NOT marked as a Ret target, because the callee is native and therefore never JS. Arg1 is the destination of the return value. Arg2 is the builtin number. Arg3 is the number of arguments, assumed to be found in reverse order from the end of the current frame. thisArg is set to "undefined". |
| `5A` | `CallBuiltinLong` | `Reg8, UInt8, UInt32` | 7 | Call a builtin function. Note this is NOT marked as a Ret target, because the callee is native and therefore never JS. Arg1 is the destination of the return value. Arg2 is the builtin number. Arg3 is the number of arguments, assumed to be found in reverse order from the end of the current frame. thisArg is set to "undefined". |
| `5B` | `GetBuiltinClosure` | `Reg8, UInt8` | 3 | Get a closure from a builtin function. Arg1 is the destination of the return value. Arg2 is the builtin number. |
| `5C` | `Ret` | `Reg8` | 2 | Return a value from the current function. return Arg1; |
| `5D` | `Catch` | `Reg8` | 2 | Catch an exception (the first instruction in an exception handler). } catch(Arg1) { |
| `5E` | `DirectEval` | `Reg8, Reg8, UInt8` | 4 | ES6 18.2.1.1 PerformEval(Arg2, evalRealm, strictCaller=true, direct=true) Arg1 is the destination of the return value. Arg2 is the value to eval. Arg3 is a boolean which is true if the eval should be performed in strict mode. |
| `5F` | `Throw` | `Reg8` | 2 | Throw an exception. throw Arg1; |
| `60` | `ThrowIfEmpty` | `Reg8, Reg8` | 3 | If Arg2 is Empty, throw ReferenceError, otherwise move it into Arg1. Arg1 is the destination of the return value Arg2 is the value to check |
| `61` | `Debugger` | `—` | 1 | Implementation dependent debugger action. |
| `62` | `AsyncBreakCheck` | `—` | 1 | Fast check for an async interrupt request. |
| `63` | `ProfilePoint` | `UInt16` | 3 | Define a profile point. Arg1 is the function local profile point index. The first one will have the largest index. If there are more than 2^16 profile points in the function, all the overflowed profile points have index zero. |
| `64` | `CreateClosure` | `Reg8, Reg8, UInt16` | 5 | Create a closure. Arg1 is the register in which to store the closure. Arg2 is the current environment as loaded by GetEnvironment 0. Arg3 is index in the function table. |
| `65` | `CreateClosureLongIndex` | `Reg8, Reg8, UInt32` | 7 | Create a closure. Arg1 is the register in which to store the closure. Arg2 is the current environment as loaded by GetEnvironment 0. Arg3 is index in the function table. |
| `66` | `CreateGeneratorClosure` | `Reg8, Reg8, UInt16` | 5 | Create a closure for a GeneratorFunction. Arg1 is the register in which to store the closure. Arg2 is the current environment as loaded by GetEnvironment 0. Arg3 is index in the function table. |
| `67` | `CreateGeneratorClosureLongIndex` | `Reg8, Reg8, UInt32` | 7 | Create a closure for a GeneratorFunction. Arg1 is the register in which to store the closure. Arg2 is the current environment as loaded by GetEnvironment 0. Arg3 is index in the function table. |
| `68` | `CreateAsyncClosure` | `Reg8, Reg8, UInt16` | 5 | Create a closure for an AsyncFunction. Arg1 is the register in which to store the closure. Arg2 is the current environment as loaded by GetEnvironment 0. Arg3 is index in the function table. |
| `69` | `CreateAsyncClosureLongIndex` | `Reg8, Reg8, UInt32` | 7 | Create a closure for an AsyncFunction. Arg1 is the register in which to store the closure. Arg2 is the current environment as loaded by GetEnvironment 0. Arg3 is index in the function table. |
| `6A` | `CreateThis` | `Reg8, Reg8, Reg8` | 4 | Allocate an empty, uninitialized object (immediately before a constructor). Arg1 is the destination register. Arg2 is the prototype to assign it. Arg3 is the constructor closure that will be used*. * To allow internal constructors to have special objects allocated. |
| `6B` | `SelectObject` | `Reg8, Reg8, Reg8` | 4 | Choose the result of a constructor: 'this' or a returned object. Arg1 is the result. Arg2 is the 'this' object used for the constructor. Arg3 is the constructor's return value. Arg1 = Arg3 instanceof Object ? Arg3 : Arg2 |
| `6C` | `LoadParam` | `Reg8, UInt8` | 3 | Load a function parameter by index. Starts at 0 with 'this'. Arg1 = Arg2 == 0 ? this : arguments[Arg2 - 1]; |
| `6D` | `LoadParamLong` | `Reg8, UInt32` | 6 | Like LoadParam, but allows accessing arguments >= 255. |
| `6E` | `LoadConstUInt8` | `Reg8, UInt8` | 3 | Load a constant integer value. |
| `6F` | `LoadConstInt` | `Reg8, Imm32` | 6 | Load a constant integer value. |
| `70` | `LoadConstDouble` | `Reg8, Double` | 10 | Load a constant double value. |
| `71` | `LoadConstBigInt` | `Reg8, UInt16` | 4 | Load a constant BigInt value by bigint table index. |
| `72` | `LoadConstBigIntLongIndex` | `Reg8, UInt32` | 6 | Load a constant BigInt value by bigint table index. |
| `73` | `LoadConstString` | `Reg8, UInt16*` | 4 | Load a constant string value by string table index. |
| `74` | `LoadConstStringLongIndex` | `Reg8, UInt32*` | 6 | Load a constant string value by string table index. |
| `75` | `LoadConstEmpty` | `Reg8` | 2 | Load common constants. |
| `76` | `LoadConstUndefined` | `Reg8` | 2 | Load common constants. |
| `77` | `LoadConstNull` | `Reg8` | 2 | Load common constants. |
| `78` | `LoadConstTrue` | `Reg8` | 2 | Load common constants. |
| `79` | `LoadConstFalse` | `Reg8` | 2 | Load common constants. |
| `7A` | `LoadConstZero` | `Reg8` | 2 | Load common constants. |
| `7B` | `CoerceThisNS` | `Reg8, Reg8` | 3 | Coerce a value assumed to contain 'this' to an object using non-strict mode rules. Primitives are boxed, \c null or \c undefed produce the global object. Arg1 = coerce_to_object(Arg2) |
| `7C` | `LoadThisNS` | `Reg8` | 2 | Obtain the raw \c this value and coerce it to an object. Equivalent to: \code LoadParam    Arg1, #0 CoerceThisNS Arg1, Arg1 \endcode |
| `7D` | `ToNumber` | `Reg8, Reg8` | 3 | Convert a value to a number. Arg1 = Arg2 - 0 |
| `7E` | `ToNumeric` | `Reg8, Reg8` | 3 | Convert a value to a numberic. Arg1 = ToNumeric(Arg2) |
| `7F` | `ToInt32` | `Reg8, Reg8` | 3 | Convert a value to a 32-bit signed integer. Arg1 = Arg2 \| 0 |
| `80` | `AddEmptyString` | `Reg8, Reg8` | 3 | Convert a value to a string as if evaluating the expression: Arg1 = "" + Arg2 In practice this means Arg1 = ToString(ToPrimitive(Arg2, PreferredType::NONE)) with ToPrimitive (ES5.1 9.1) and ToString (ES5.1 9.8). |
| `81` | `GetArgumentsPropByVal` | `Reg8, Reg8, Reg8` | 4 | Get a property of the 'arguments' array by value. Arg1 is the result. Arg2 is the index. Arg3 is the lazy loaded register. Arg1 = arguments[Arg2] |
| `82` | `GetArgumentsLength` | `Reg8, Reg8` | 3 | Get the length of the 'arguments' array. Arg1 is the result. Arg2 is the lazy loaded register. Arg1 = arguments.length |
| `83` | `ReifyArguments` | `Reg8` | 2 | Create an actual 'arguments' array, if get-by-index and length isn't enough. Arg1 is the lazy loaded register, which afterwards will contain a proper object that can be used by non-*Arguments* opcodes like Return. |
| `84` | `CreateRegExp` | `Reg8, UInt32*, UInt32*, UInt32` | 14 | Create a regular expression. Arg1 is the result. Arg2 is the string index of the pattern. Arg3 is the string index of the flags. Arg4 is the regexp bytecode index in the regexp table. |
| `85` | `SwitchImm` | `Reg8, UInt32, Addr32, UInt32, UInt32` | 18 | Jump table switch - using a table of offset, jump to the offset of the given input or to the default block if out of range (or not right type) Arg 1 is the value to be branched upon Arg 2 is the relative offset of the jump table to be used by this instruction. Jump tables are appended to the bytecode. Arg 3 is the relative offset for the "default" jump. Arg 4 is the unsigned min value, if arg 1 is less than this value jmp to default block Arg 5 is the unsigned max value, if arg 1 is greater than this value jmp to default block. Given the above, the jump table entry for a given value (that is in range) is located at offset ip + arg2 + arg1 - arg4. We subtract arg4 to avoid wasting space when compiling denses switches that do not start at zero. Note that Arg2 is *unaligned* it is dynamically aligned at runtime. |
| `86` | `StartGenerator` | `—` | 1 | Start the generator by jumping to the next instruction to begin. Restore the stack frame if this generator has previously been suspended. |
| `87` | `ResumeGenerator` | `Reg8, Reg8` | 3 | Resume generator by performing one of the following user-requested actions: - next(val): Set Arg1 to val, Arg2 to false, run next instruction - return(val): Set Arg1 to val, Arg2 to true, run next instruction - throw(val): Throw val as an error Arg1 is the result provided by the user. Arg2 is a boolean which is true if the user requested a return(). |
| `88` | `CompleteGenerator` | `—` | 1 | Set the generator status to complete, but do not return. |
| `89` | `CreateGenerator` | `Reg8, Reg8, UInt16` | 5 | Create a generator. Arg1 is the register in which to store the generator. Arg2 is the current environment as loaded by GetEnvironment 0. Arg3 is index in the function table. |
| `8A` | `CreateGeneratorLongIndex` | `Reg8, Reg8, UInt32` | 7 | Create a generator. Arg1 is the register in which to store the generator. Arg2 is the current environment as loaded by GetEnvironment 0. Arg3 is index in the function table. |
| `8B` | `IteratorBegin` | `Reg8, Reg8` | 3 | Arg1 [out] is the result iterator or index. Arg2 [in/out] is the source. Output for either the source or next method. If source is an array with an unmodified [Symbol.iterator], the result is 0. Else the result is source[Symbol.iterator] and the output is the .next() method on the iterator. See IR.md for IteratorBeginInst. |
| `8C` | `IteratorNext` | `Reg8, Reg8, Reg8` | 4 | Arg1 [out] is the result, or undefined if done. Arg2 [in/out] is the iterator or index. Arg2 [in] is the source or the next method. If iterator is undefined, result = undefined. If iterator is a number: If iterator is less than source.length, return source[iterator++] Else iterator = undefined and result = undefined Else: n = iterator.next() If n.done, iterator = undefined and result = undefined. Else result = n.value See IR.md for IteratorNextInst. |
| `8D` | `IteratorClose` | `Reg8, UInt8` | 3 | Arg1 [in] is the iterator or array index. Arg2 is a bool indicating whether to ignore the inner exception. If the iterator is an object, call iterator.return(). If Arg2 is true, ignore exceptions which are thrown by iterator.return(). See IR.md for IteratorCloseInst. |
| `8E` | `Jmp` | `Addr8` | 2 | Unconditional branch to Arg1. |
| `8F` | `JmpLong` | `Addr32` | 5 | Unconditional branch to Arg1. (long-index variant of `Jmp`, wider operand.) |
| `90` | `JmpTrue` | `Addr8, Reg8` | 3 | Conditional branches to Arg1 based on Arg2. |
| `91` | `JmpTrueLong` | `Addr32, Reg8` | 6 | Conditional branches to Arg1 based on Arg2. (long-index variant of `JmpTrue`, wider operand.) |
| `92` | `JmpFalse` | `Addr8, Reg8` | 3 | Conditional branches to Arg1 based on Arg2. |
| `93` | `JmpFalseLong` | `Addr32, Reg8` | 6 | Conditional branches to Arg1 based on Arg2. (long-index variant of `JmpFalse`, wider operand.) |
| `94` | `JmpUndefined` | `Addr8, Reg8` | 3 | Jump if the value is undefined. |
| `95` | `JmpUndefinedLong` | `Addr32, Reg8` | 6 | Jump if the value is undefined. (long-index variant of `JmpUndefined`, wider operand.) |
| `96` | `SaveGenerator` | `Addr8` | 2 | Save the provided value, yield, and signal the VM to restart execution at the provided target. |
| `97` | `SaveGeneratorLong` | `Addr32` | 5 | Save the provided value, yield, and signal the VM to restart execution at the provided target. (long-index variant of `SaveGenerator`, wider operand.) |
| `98` | `JLess` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `99` | `JLessLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JLess`, wider operand.) |
| `9A` | `JNotLess` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `9B` | `JNotLessLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JNotLess`, wider operand.) |
| `9C` | `JLessN` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `9D` | `JLessNLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JLessN`, wider operand.) |
| `9E` | `JNotLessN` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `9F` | `JNotLessNLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JNotLessN`, wider operand.) |
| `A0` | `JLessEqual` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `A1` | `JLessEqualLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JLessEqual`, wider operand.) |
| `A2` | `JNotLessEqual` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `A3` | `JNotLessEqualLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JNotLessEqual`, wider operand.) |
| `A4` | `JLessEqualN` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `A5` | `JLessEqualNLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JLessEqualN`, wider operand.) |
| `A6` | `JNotLessEqualN` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `A7` | `JNotLessEqualNLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JNotLessEqualN`, wider operand.) |
| `A8` | `JGreater` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `A9` | `JGreaterLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JGreater`, wider operand.) |
| `AA` | `JNotGreater` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `AB` | `JNotGreaterLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JNotGreater`, wider operand.) |
| `AC` | `JGreaterN` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `AD` | `JGreaterNLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JGreaterN`, wider operand.) |
| `AE` | `JNotGreaterN` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `AF` | `JNotGreaterNLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JNotGreaterN`, wider operand.) |
| `B0` | `JGreaterEqual` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `B1` | `JGreaterEqualLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JGreaterEqual`, wider operand.) |
| `B2` | `JNotGreaterEqual` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `B3` | `JNotGreaterEqualLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JNotGreaterEqual`, wider operand.) |
| `B4` | `JGreaterEqualN` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `B5` | `JGreaterEqualNLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JGreaterEqualN`, wider operand.) |
| `B6` | `JNotGreaterEqualN` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `B7` | `JNotGreaterEqualNLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JNotGreaterEqualN`, wider operand.) |
| `B8` | `JEqual` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `B9` | `JEqualLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JEqual`, wider operand.) |
| `BA` | `JNotEqual` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `BB` | `JNotEqualLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JNotEqual`, wider operand.) |
| `BC` | `JStrictEqual` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `BD` | `JStrictEqualLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JStrictEqual`, wider operand.) |
| `BE` | `JStrictNotEqual` | `Addr8, Reg8, Reg8` | 4 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" |
| `BF` | `JStrictNotEqualLong` | `Addr32, Reg8, Reg8` | 7 | Not conditionals are required for NaN comparisons Since we want to be able to reorder targets to allow for fall-throughs, we need to be able to say "jump when not less than to BB2" instead of "jump when less than to BB1". Since NaN comparisons always return false, "not less" != "greater or equal" (long-index variant of `JStrictNotEqual`, wider operand.) |
| `C0` | `Add32` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 + Arg3 (32-bit integer addition) |
| `C1` | `Sub32` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 - Arg3 (32-bit integer subtraction) |
| `C2` | `Mul32` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 * Arg3 (32-bit integer multiplication) |
| `C3` | `Divi32` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 / Arg3 (32-bit signed integer division) |
| `C4` | `Divu32` | `Reg8, Reg8, Reg8` | 4 | Arg1 = Arg2 / Arg3 (32-bit unsigned integer division) |
| `C5` | `Loadi8` | `Reg8, Reg8, Reg8` | 4 | Arg1 = HEAP8[Arg3] (load signed 8-bit integer) |
| `C6` | `Loadu8` | `Reg8, Reg8, Reg8` | 4 | Arg1 = HEAPU8[Arg3] (load unsigned 8-bit integer) |
| `C7` | `Loadi16` | `Reg8, Reg8, Reg8` | 4 | Arg1 = HEAP16[Arg3 >> 1] (load signed 16-bit integer) |
| `C8` | `Loadu16` | `Reg8, Reg8, Reg8` | 4 | Arg1 = HEAPU16[Arg3 >> 1] (load unsigned 16-bit integer) |
| `C9` | `Loadi32` | `Reg8, Reg8, Reg8` | 4 | Arg1 = HEAP32[Arg3 >> 2] (load signed 32-bit integer) |
| `CA` | `Loadu32` | `Reg8, Reg8, Reg8` | 4 | Arg1 = HEAPU32[Arg3 >> 2] (load unsigned 32-bit integer) |
| `CB` | `Store8` | `Reg8, Reg8, Reg8` | 4 | HEAP8[Arg2] = Arg3 (store signed or unsigned 8-bit integer) |
| `CC` | `Store16` | `Reg8, Reg8, Reg8` | 4 | HEAP16[Arg2] = Arg3 (store signed or unsigned 16-bit integer) |
| `CD` | `Store32` | `Reg8, Reg8, Reg8` | 4 | HEAP32[Arg2] = Arg3 (store signed or unsigned 32-bit integer) |

---

## Open points summary

| Item | Section here | Bonus entry | Blocking step |
|---|---|---|---|
| Identifier hash algorithm | §5.2 | B3 | recompute candidate hash (Jenkins family) over a real corpus, compare byte-for-byte |
| BigInt literal encoding | §7 | B20 | produce a sample with `bigIntCount > 0` |
| RegExp precompiled bytecode | §8 | B20 | out of scope (regex *source* already recovered via String Table, no decompilation need) |
| CJS Module Table entry layout | §9.2 | B16 | **resolved** — confirmed from source + compiled samples; a decoder is not yet wired into hermit's parser |
| `segmentID` / multi-segment layout | §9.3 | B16 | **resolved** — confirmed to change nothing else in the layout |
| Multi-file debug info (`FileRegion`) | §11.1 | B17 | compile several `.js` sources directly with `hermes`, bypassing Metro |
| Scope Descriptor `flags` non-zero cases | §11.3 | B18 | find/construct a JS pattern that sets `InnerScope`/`Dynamic` |
| Outer-scope name resolution | §11.3 | B19 | decompiler-side, format already sufficient (`parentId` chain) |
