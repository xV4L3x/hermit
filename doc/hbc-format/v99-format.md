# HBC v99 (Static Hermes) format

> How HBC version 99 differs from v98. Anchored to the Hermes source on the
> **`static_h`** branch at commit **`c8dab081f`** (bytecode-version bump
> `42235b8d`, "Bump bytecode version to 99", 2026-02-12).

## Where v99 sits

v99 lives on the divergent **`static_h`** branch (Static Hermes), the successor
line to v98. It is **not** a mainline HBC version: no React Native stable release
emits it by default, so it is covered as a forward-looking second-to-v98 wire
format, not a shipping target. It is a small delta over
[v98](v98-format.md): same file header, same 12-byte small function header, same
one-word debug offsets, same object shape table and literal-buffer sections. The
two layout differences are a **36-byte overflow header** and a **renumbered
opcode table**.

hermit treats v99 as its own `FormatVersion` module (`core/src/format/format_v99.cpp`)
selected at runtime by `formatForVersion()`, next to `format_v98.cpp` (v97/98)
and `format_legacy.cpp` (v94/95/96).

## "Version 99" drifts within itself, like v98

As with v98, **Static Hermes changed the v99 wire format without bumping the
version number.** Between the v99 bump (`42235b8d`, 2026-02-12) and `c8dab081f`
the opcode table moved three times:

| snapshot | opcode table |
|---|---|
| bump `42235b8d` (2026-02-12) | 219 ops, no `NewTypedObjectWithBuffer`, `NewFastArray` arity 2 |
| `a6671652a` (2026-02-28) | +`NewTypedObjectWithBuffer` (shifts every later opcode) |
| `d4f5193f0` (2026-04-07) → `c8dab081f` | `NewFastArray` arity 2 → 3 |

The file header did **not** change across the window. hermit targets the latest
snapshot (`c8dab081f`), whose opcode numbering matches real production v99
bundles: the insertion of `NewTypedObjectWithBuffer` renumbers every opcode after
it, and observed bundles encode against the post-insertion numbering (a bundle
decoded cleanly, tiling every function body to its exact length, only under the
220-opcode table, not the 219-opcode bump table).

## What differs from v98

### Overflow (large) function header — 36 bytes, not 37

The small (table) function header stays **12 bytes**. The overflow header shrinks
from 37 to **36 bytes**: `CacheNewObject` and its `NumCacheNewObject` header field
were removed (`7193d4485`, "Remove CacheNewObject", 2026-01-21, which landed
*before* the v99 bump, so every v99 overflow header is 36 bytes — there is no
37-byte v99 variant). Every field hermit reads keeps its position
(`Offset@0`, `BytecodeSizeInBytes@12`, `FrameSize@28`); only the trailing flag
byte moves from offset 36 to **35** (= `largeHeaderSize() - 1`), and the total
size shrinks by one.

In the shared `FUNC_HEADER_FIELDS` macro (described only in the Hermes source),
the third storage byte changes from
`WriteCacheSize[6] NumCacheNewObject[1] PrivateNameCacheSize[1]` to
`WriteCacheSize[7] PrivateNameCacheSize[1]`. hermit reads only
Offset / ParamCount / BytecodeSize / FunctionName / FrameSize / flags from the
small header (words `w1`/`w2`, byte `b8`, flag byte `b11`), none of which move, so
the small-header path is version-neutral and byte-exact round-trips preserve the
cache bits unchanged.

### Opcode table — 220 opcodes, renumbered

Generated from the `static_h` `BytecodeList.def` by `seeder/gen_opcodes.py` into
`core/src/format/opcodes_v99.cpp`. Relative to the shipped v98 (219 ops):

- `+ NewTypedObjectWithBuffer` (0x04, `Reg8, Reg8, UInt32, UInt32, UInt8`) — a
  typed-object literal for Static Hermes typed classes;
- `+ ToUint32` (0x9D, `Reg8, Reg8`) — `a >>> 0`;
- `- CacheNewObject` removed;
- `NewFastArray` arity 2 → 3 (`Reg8, UInt16` → `Reg8, Reg8, UInt16`): its
  prototype becomes an explicit operand;
- every opcode after `NewTypedObjectWithBuffer` is renumbered by +1.

### Everything else — unchanged from v98

The 128-byte file header (including the `numStringSwitchImms` word that shifts the
tail), the `{ sourceLocations }` one-word per-function debug offsets, the object
shape table (`NewObjectWithBuffer` keys/count via `ShapeTableEntry`), and the
merged literal value buffer are identical to v98. The per-function debug-offset
layout and the source-locations encoding did not change (the v99 `DebugInfo.h`
diff is in-memory API only).

## How hermit implements it

- `core/src/format/opcodes_v99.cpp` (generated), via `getOpcodeTable(99)`.
- `core/src/format/format_v99.cpp`: `FormatV99` subclasses `FormatV98` (extracted
  into `core/src/format/format_v98_class.h`) and overrides only
  `largeHeaderSize()` → 36. `FormatV98` reads the overflow flag byte at
  `largeHeaderSize() - 1` and bounds-checks against `largeHeaderSize()`, so the
  one override covers the whole difference.
- `formatForVersion()` and the parser's header validation accept 99 (the file
  header is reinterpreted through the v98 struct, same layout).
- Verified on v99 samples built with a v99 compiler from `static_h` `c8dab081f`,
  and against a real production React Native v99 bundle: parse, disasm, CFG,
  decompiler, string/literal xrefs, byte-identical round-trip, the full structural
  patcher (insertion + EH relocation + branch relaxation + string rebuild +
  literal resize + `add_function`, each executed on the v99 VM), and `section_map`
  coverage with no gap.

## Known gaps

- **`NewTypedObjectWithBuffer`** is not recognised as an object-literal producer,
  so its typed-object literals are not resolved or resized by the literal xref /
  resize paths. It is emitted only by Static Hermes typed classes and was absent
  from the production bundles observed, so it does not affect ordinary bundles.
- **`StringSwitchImm`**'s appended case table is not reconstructed (inherited from
  v98): a function with a large string `switch` disassembles its case table as
  stray instructions.
