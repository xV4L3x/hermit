# HBC v98 (Static Hermes / Hermes V1) format

> How HBC version 98 differs from v94/95/96. Anchored to the Hermes source at tag
> **`hermes-v250829098.0.14`** — the Hermes V1 that React Native 0.84-0.86 ship.

## "Version 98" is not one format

v96 is on the facebook/hermes **mainline**; v98 is on the divergent **`static_h`**
branch (Static Hermes, which ships as Hermes V1). Crucially, **Static Hermes
changed the v98 wire format several times without bumping the version number.**
The first v98 commit (`c00cc5759`, Aug 2024) and the shipped Hermes V1 (tag
`hermes-v250829098.0.x`, built 2025-08) are both "version 98" but have
different function-header bitfields, a different file header, and different
per-function debug offsets.

**hermit targets the shipped Hermes V1** (`hermes-v250829098.0.14`), i.e. what RN
0.84-0.86 bundles actually use — not the first v98 commit. The version number
alone does not identify the layout, so a bundle from a much older or newer
static_h snapshot may not match; the shipped V1 is the real-world target.

hermit treats v98 as a second wire format: its logic lives in a `FormatVersion`
module (`core/src/format/format_v98.cpp`) selected at runtime by
`formatForVersion()`, alongside `format_legacy.cpp` (v94/95/96). Build the v98
compiler + VM from the tag (see [`bytecode-versions.md`](bytecode-versions.md)).

## What differs from v94/95/96

### File header — same 128 bytes, a new word shifts the tail

A `numStringSwitchImms` word is inserted after `objShapeTableCount`, shifting the
last four words (`padding` shrinks 19 → 15 bytes):

| Offset | v94-96 | v98 (shipped) |
|---|---|---|
| 0x50 | `arrayBufferSize` | `literalValueBufferSize` |
| 0x54 | `objKeyBufferSize` | `objKeyBufferSize` |
| 0x58 | `objValueBufferSize` | `objShapeTableCount` (entry count) |
| 0x5C | `segmentID` | **`numStringSwitchImms`** (new) |
| 0x60 | `cjsModuleCount` | `segmentID` |
| 0x64 | `functionSourceCount` | `cjsModuleCount` |
| 0x68 | `debugInfoOffset` | `functionSourceCount` |
| 0x6C | `options` | `debugInfoOffset` |

The array and object-value buffers are merged into one literal value buffer, and
an object shape table is added (as in the first v98). The **misread
`debugInfoOffset`** (0x68 vs 0x6C) is what made `hbc_section_map` report a
negative coverage gap on real bundles before this was fixed.

### Function header — 12 bytes, different bitfields

v98's `SmallFuncHeader` is 12 bytes (as v98, not the 16 of v94-96) but its bit
layout is entirely different and uses Hermes's `BitField` storage, so hermit
parses it bit by bit:

```
w1 (uint32): Offset[0:25]  ParamCount[25:30]  LoopDepth[30:32]
w2 (uint32): BytecodeSizeInBytes[0:14]  FunctionName[14:22]  NumberRegCount[22:27]  NonPtrRegCount[27:32]
b8: FrameSize   b9: ReadCacheSize   b10: WriteCacheSize[0:6] NumCacheNewObject[6] PrivateNameCache[7]   b11: flags
```

Note `ParamCount` is 5 bits and `FunctionName` only 8 bits, so most functions in
a real bundle overflow (name index > 255). The overflow (large) header is 37
bytes; the overflow offset is packed as **`offset(low 24) | functionName(high
8)`** → `getLargeHeaderOffset()`. A function has an info section (large header +
EH table + debug offsets) iff its small header's `overflowed` flag (byte 11, bit
5) is set; the info section follows the large header, aligned to 4.

The flag byte is unchanged from the first v98:
`prohibitInvoke[0:2] strictMode[2] hasExceptionHandler[3] hasDebugInfo[4] overflowed[5] kind[6:8]`.
(The large header carries the real flags with `overflowed = 0`, so `overflowed`
is read from the small header, never the large one.)

### Per-function debug offsets — one word

v98 shipped stores a single `{ sourceLocations }` (4 bytes), not the v94-96
three-word `{ source locations, scope descriptor, textified callees }` (12
bytes). (`lexicalData` was deleted within the v98 window.)

### Object literals — shape table (unchanged from the first v98)

`NewObjectWithBuffer (dst, shapeTableIndex, valueBufferIdx)`: the key-buffer
offset and property count come from the object shape table
(`ShapeTableEntry { uint32 keyBufferOffset; uint32 numProps; }`). Resize appends
a new shape entry and repoints `shapeTableIndex`.

### Opcode table — 219 opcodes, renumbered

Generated from the shipped `BytecodeList.def` by `seeder/gen_opcodes.py`. Adds
`StringSwitchImm` (a string `switch` with an appended case table, like
`SwitchImm`'s jump table — hermit does not yet reconstruct that case table) and
`GetOwnBySlotIdx`, among others, on top of the first-v98 changes.

## How hermit implements it

- `core/src/format/opcodes_v98.cpp` (generated), via `getOpcodeTable()`.
- `core/include/hbc/format_v98.h` + `core/src/format/format_v98.cpp` (the
  `FormatVersion` module), via `formatForVersion()`. The shifted header words are
  read/written through the module (`debugInfoOffset()` / `cjsModuleCount()` /
  `functionSourceCount()` / `setDebugInfoOffset()`); the function header is parsed
  and written bit by bit.
- Verified on v98 samples built with the shipped-tag compiler: parse, disasm,
  CFG, decompiler, string/literal xrefs, byte-identical round-trip, the full
  structural patcher, and `section_map` coverage, each run on the v98 VM.

## Known gaps

- `StringSwitchImm`'s appended case table is not reconstructed (as `SwitchImm` is
  a special case, this one is not yet) — a function using a large string `switch`
  disassembles its case table as stray instructions.
