# HBC v98 (Static Hermes) format

> How HBC version 98 differs from v94/95/96. Anchored to the Hermes source on the
> `static_h` branch at commit `c00cc5759` ("Support PrLoad in bytecode"), the
> first commit that sets `BYTECODE_VERSION = 98`.

## Lineage: v98 is not a successor of v96

v96 is on the facebook/hermes **mainline**; v98 is on the divergent **`static_h`**
branch (Static Hermes, the AOT compiler that ships as Hermes V1 in React Native
0.84+). The two lineages split in 2022 (merge-base `7a9359527`), so v96 is not an
ancestor of v98. On `static_h` the version chain is:

```
… 90 (906b5cff3) → 97 (16b5ada82, 2024-05) → 98 (c00cc5759, 2024-08) → 99 (42235b8d9, 2026-02) → …
```

`97 → 98` adds only two opcodes (`GetOwnBySlotIdx` / `GetOwnBySlotIdxLong`), with
no struct change, so v98's header/function-header/section layout equals v97's.
The whole structural weight is the `90 → 97` rewrite. hermit therefore treats v98
as a **second wire format**, not a version bump: per-version logic lives in a
`FormatVersion` module (`core/src/format/format_v98.cpp`) selected at runtime by
`formatForVersion()`, alongside the v94-96 module (`format_legacy.cpp`).

To build a v98 compiler + VM, see
[`bytecode-versions.md`](bytecode-versions.md): a `static_h` worktree at a v98
commit, `ninja hermes`. The stock `hermes` there compiles plain JS to a runnable
v98 `.hbc`. (Note: this build rejects ES6 `class` at runtime, so v98 test samples
are class-free.)

## What differs from v94/95/96

### File header (`BytecodeFileHeader`) — same 128 bytes, two words repurposed

The header keeps its size and field offsets, but the three literal-buffer words
change meaning:

| Offset | v94-96 | v98 |
|---|---|---|
| 0x50 | `arrayBufferSize` | `literalValueBufferSize` |
| 0x54 | `objKeyBufferSize` | `objKeyBufferSize` (same) |
| 0x58 | `objValueBufferSize` | `objShapeTableCount` (a count of 8-byte entries) |

The array buffer and the object-value buffer are **merged** into one literal
value buffer, and a new **object shape table** section is added. Section order:
`… string storage → literal value buffer → object key buffer → object shape table
→ bigint → regexp → cjs → function source → debug info`.

### Function header — 12 bytes (was 16)

`FUNC_HEADER_FIELDS` at v98:

```
offset:25  paramCount:7  | bytecodeSizeInBytes:15  functionName:17  | frameSize:u8  highestReadCacheIndex:u8  highestWriteCacheIndex:u8  flags:u8
```

- **`infoOffset` (25-bit) removed** and **`environmentSize` (8-bit) removed**;
  `frameSize` widened 7→8 bit. The `SmallFuncHeader` is 12 bytes; the overflow
  `FunctionHeader` is 23 bytes (`frameSize` is a full `uint32` there).
- **`FunctionHeaderFlag` gains a 2-bit `kind`** field (`Normal=0 / Generator=1 /
  Async=2`) in the old `_reserved` bits.
- **Locating a function's info section** (exception-handler table + debug
  offsets) changed. There is no `infoOffset` field, so a function has an info
  section **iff its small header is `overflowed`**: the overflow offset is packed
  as `offset | (functionName << 16)`, the large header sits there, and the info
  section follows it (aligned to 4). A non-overflowed function has no info
  section (no exception handler, no debug info).
- Per-function **debug offsets** are the older two-word form
  `{ sourceLocations, lexicalData }` (8 bytes), not the v94-96 three-word
  `{ source locations, scope descriptor, textified callees }` (12 bytes).

### Object literals — shape table instead of inline operands

`NewObjectWithBuffer` changed from `(dst, sizeHint, count, keyBufferIdx,
valueBufferIdx)` to **`(dst, shapeTableIndex, valueBufferIdx)`**. The key-buffer
offset and property count come from the shape table entry
(`ShapeTableEntry { uint32 keyBufferOffset; uint32 numProps; }`), not from
operands. The object value buffer is the merged literal value buffer. (Array
literals are unchanged: `dst, sizeHint, count, bufferOffset`.)

This is why object-literal **resize** on v98 appends a new shape entry (new key
offset + count) and repoints `shapeTableIndex`, rather than rewriting inline
count/key operands.

### Opcode table — fully renumbered

v98 has 201 opcodes (v96 has 206); every opcode byte shifts, so a v96 table
cannot decode v98. Beyond the object-literal change above: generators/async and
the `HERMES_RUN_WASM` block and `CallDirect*`/`CallLong`/`ConstructLong` are
removed; `CallWithNewTarget[Long]`, `AddS`, `GetByIndex`, `PutOwnBySlotIdx[Long]`,
`GetOwnBySlotIdx[Long]` (the v98 addition), and explicit-environment opcodes
(`CreateFunctionEnvironment`, `CreateTopLevelEnvironment`) are added; `Put/Del/
TryPut ById/ByVal` are split into Loose/Strict variants; `DirectEval`,
`GetEnvironment`, `CreateEnvironment` change arity. hermit's table is generated
from the v98 `BytecodeList.def` by `seeder/gen_opcodes.py`.

## How hermit implements it

- Opcode table: `core/src/format/opcodes_v98.cpp` (generated), dispatched by
  `getOpcodeTable()`.
- Layout/structs: `core/include/hbc/format_v98.h` + the `FormatVersion` module
  `core/src/format/format_v98.cpp`, dispatched by `formatForVersion()`. The
  parser, analysis and `Module` serializer call the module instead of branching
  on the version.
- Verified on v98 samples (built with the `static_h` c00cc5759 compiler): parse,
  disassembly, CFG, decompiler, string/literal xrefs, byte-identical round-trip,
  and the full structural patcher (instruction insert + jump/EH relocation,
  string edits, `add_function`, literal value edits, array and object-shape
  resize), each executed on the v98 VM.
