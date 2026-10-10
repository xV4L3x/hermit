# HBC Shared Function Bodies

> When Hermes/Metro compiles a real React Native bundle, **byte-identical function
> bodies are emitted once and pointed at by many function-table entries**. This is
> a size optimisation that has **no documentation outside the Hermes source**: it
> is not covered by the reverse-engineered spec, `hbctool`, `hermes-dec` or
> REUNIFY. hermit ran into it while bringing the structural patcher up on
> `ridemovi.hbc`. This file is the reference.
>
> **Correction (2026-10-03):** an earlier version said "not documented
> anywhere". That was an overclaim. The optimisation is implemented in the
> public Hermes source (`lib/BCGen/HBC/BytecodeStream.cpp`, "Deduplicate the
> bytecode during layout phase", with a `DedupKey` map from opcode arrays to
> offsets) and is named in Meta's 2019 announcement ("function deduplication").
> What is new here is the analysis of its consequences for patching, not the
> discovery of the mechanism.

**Verified empirically on:** `ridemovi.hbc` (real RN app, HBC v96, 12.7 MB,
37 774 functions), 2026-09-10.

---

## TL;DR

- Two or more `SmallFuncHeader` entries can carry the **same
  `(offset, bytecodeSizeInBytes)`** pair. They point at the **same bytes** in the
  bytecode blob.
- Each entry still has its **own** name, `paramCount`, `frameSize`, exception
  handlers and debug info - only the *code* is shared.
- In `ridemovi.hbc`: **1862 shared groups, 8851 functions** (23 % of all
  functions) share a body with at least one other function.
- Detection is trivial: group the function table by `(offset, size)`; any group
  with more than one member is shared. hermit exposes this as
  `FunctionInfo.shared` (bool) and `FunctionInfo.shareCount` (int).
- **Patching implication:** editing the bytes of a shared body changes **every**
  function in the group. hermit's editor shows a `⚠ condiviso ×N` banner and, on
  save, asks *"applicare la modifica a TUTTE?"*. Patching a **single** member
  (clone the bytes + add a new function-table entry) is not supported yet.

---

## What it is

The HBC function table is an array of `SmallFuncHeader` (16 bytes each). The
relevant bitfields:

| field | bits | meaning |
|---|---|---|
| `offset` | 25 | byte offset of the function body in the file |
| `bytecodeSizeInBytes` | 15 | length of the body |
| `functionName` | 17 | string-table index of the name |
| `infoOffset` | 25 | offset of this function's info section (EH table + debug triplet) |
| `paramCount` | 7 | JS args + 1 (implicit `this`) |
| `frameSize` | 7 | register count |

Nothing in the format says `offset` must be unique. Metro/Hermes exploits this:
if two functions compile to the **exact same byte sequence**, the body is written
to the blob **once**, and both `SmallFuncHeader` entries get the same `offset`
and `bytecodeSizeInBytes`. The `infoOffset`, `functionName`, `paramCount`,
`frameSize` and the whole info section stay **per-entry** - so the two functions
can have different names and even different exception handlers while executing
the identical code.

## Why it happens

Real RN bundles are Metro output: every module is wrapped, and Babel/Metro inject
the **same helper functions into every module that needs them**. After
compilation these helpers are byte-identical across hundreds of modules. Same for
trivial stubs. Emitting each copy would waste hundreds of KB, so Hermes
deduplicates the body while keeping the table entries.

Typical shared bodies:

- **Babel/interop helpers** injected per-module: `_interopRequireWildcard`,
  `_interopRequireDefault`, `_isNativeReflectConstruct`, `_getPrototypeOf`, …
- **Empty / trivial stubs**: `function () {}` -> `LoadConstUndefined r0; Ret r0`.
- **Boilerplate accessors** and closure-returning one-liners emitted by the same
  code path in many modules.

## How to detect it

```python
import collections
groups = collections.defaultdict(list)
for f in hbc.functions():
    groups[(f.offset, f.bytecodeSizeInBytes)].append(f.index)
shared = {k: v for k, v in groups.items() if len(v) > 1}
```

In hermit this is done once in `parseFunctionTable`
(`core/src/parser/function_table.cpp`): after the main loop the result vector is
grouped by `(offset, bytecodeSizeInBytes)` and every member of a group of size
> 1 gets `shared = true`, `shareCount = <group size>`. Exposed to Python as
`FunctionInfo.shared` / `FunctionInfo.shareCount`.

> Note: grouping on `(offset, size)` is exact here because Hermes only ever
> reuses a body when it is *identical* - it never points two entries at
> *overlapping* ranges. A group therefore always describes one shared body with N
> owners.

## Real data - `ridemovi.hbc`

37 774 functions · 1862 shared groups · **8851 functions in a shared group** ·
4757 body-offset inversions in index order ·
bytecode blob span `0x2df460 … 0x769dc3` (≈ 4.76 MB).

### Largest shared groups

| copies | offset | size | body (disasm of one member) | notes |
|---:|---|---:|---|---|
| **1004** | `0x2ffb84` | 273 | `_interopRequireWildcard` (see below) | all 1004 entries even keep the name `_interopRequireWildcard` |
| **844** | `0x2fb19e` | 4 | `LoadConstUndefined r0` · `Ret r0` | empty function `() => {}`; names vary per entry: `noop`, `setupOnce`, `onPress`, `reset`, `then`, `F`, … |
| **440** | `0x2fb339` | 15 | `GetEnvironment r0,0` · `LoadFromEnvironment r0,r0,0` · `Not r0,r0` · `Not r0,r0` · `Ret r0` | 439 entries named `_isNativeReflectConstruct`, 1 named `inGuard` |
| **331** | `0x2fb53a` | 29 | `LoadConstUndefined` · `GetEnvironment` · `LoadFromEnvironment` · `GetByIdShort` · `ReifyArguments` · `Mov` · `LoadThisNS` · `Call3` | closure that forwards `arguments`; names all over the place (`fetchFunc`, `action`, `componentDidMount`, …) |
| **128** | `0x2fa1bd` | 9 | `GetEnvironment r0,0` · `LoadFromEnvironment r0,r0,0` · `Ret r0` | env-var getter |
| **111** | `0x312836` | 30 | like the ×331 body but `LoadParam` instead of `LoadThisNS` | |
| **84** | `0x484198` | 29 | `GetEnvironment` · `LoadFromEnvironment` · `GetByIdShort` · `CreateThis` · `Mov` · `Construct` · `SelectObject` · `Ret` | `createInstance` factory |
| **53** | `0x2ffd31` | 20 | `GetEnvironment` · `LoadFromEnvironment` · `GetEnvironment` · `LoadFromEnvironment` · `GetByVal` · `Ret` | property `get` accessor |

`_interopRequireWildcard` @ `0x2ffb84`, size 273 - full disasm of the first
handful of instructions:

```
0x0000  LoadParam            r3, 1
0x0003  LoadParam            r2, 2
0x0006  JmpTrue              +16, r2
0x0009  JmpFalse             +13, r3
0x000c  GetByIdShort         r0, r3, 1, "prototype"
0x0011  JmpFalse             +5, r0
0x0014  Ret                  r3
0x0016  LoadConstNull        r1
...
```

### Key observations

1. **Names can differ within a group** (the ×844 stub, the ×440 group). Each
   `SmallFuncHeader.functionName` is independent; only `offset` + size are
   shared. So "group by name" is *not* how you find shared bodies - group by
   `(offset, size)`.
2. **Shared bodies are clustered low in the blob** (`0x2fa…`–`0x312…`), i.e. near
   the modules that first defined them. They are *not* moved to a dedup pool.
3. **A shared body can itself belong to an EH-carrying function** - the EH table
   lives in the per-entry info section, not the body. In `ridemovi.hbc` none of
   the largest shared groups have more than one EH member, which is why the serializer
   can assume that (see limits below).
4. **Body order is not index order.** 4757 times `offset[i] < offset[i-1]`. Metro
   emits bodies in dependency/emission order, then the function table indexes
   them in declaration order. The structural serializer must not assume the blob
   is sorted (this is why the serializer uses a verbatim-splice model).

## Implications for patching

Editing the bytes at a shared `offset` **rewrites the code of every function in
the group**. There is no way around it at the byte level: they are the same
bytes.

hermit handles this as follows:

- **Parser** flags the function: `FunctionInfo.shared`, `FunctionInfo.shareCount`.
- **Editor** (`cli/editor/`) shows `⚠ condiviso ×N` in the title bar for the
  whole session (not just at save time).
- **On `Ctrl+X`**, *before* the normal save prompt, a dedicated `share_ask` mode
  asks: *"⚠ questo codice è condiviso da N funzioni - applicare la modifica a
  TUTTE? Y sì / N·Esc annulla"*.
  - `Y` -> `state.edit_all = True`; the commit proceeds. `Module` rewrites the
    body once and updates `bytecodeSizeInBytes` (and `frameSize` if bumped) on
    **every** `SmallFuncHeader` in the group.
  - `N` / `Esc` -> the commit is aborted, nothing is written, the editor returns
    to nav with an explanatory error line.
- **`Module`** (`core/src/patcher/module.cpp`): `set_function_body(idx, ir,
  frameSize, editAll)` throws if the body is shared and `editAll` is false -
  *"corpo condiviso da N funzioni: patch di una singola funzione (clone privato)
  non supportata"*.

### The single-function case (clone-on-edit) - not supported yet

To patch **one** member of a shared group without touching the others you must:

1. copy the body bytes to a new location (end of blob),
2. add a new function-table entry (or repoint the existing one) at that location,
3. give it its own info section,
4. re-point only that one `SmallFuncHeader`.

Steps 2–3 need the "add a function" machinery. Until
then the only operation on a shared body is *all or nothing*.

### `editAll` with exception handlers

Each member's EH table lives in its own info section, but its offsets are all
relative to the start of the (shared) body. With `editAll`, `serialize()`
therefore relocates the EH table of **every** member of the group with the same
old->new offset map (built once from the dirty IR), not only the table of the
function that was opened in the editor. A guard skips an info section already
relocated, in case two entries point at the same one.

`ridemovi.hbc` has 20 shared groups where **every** member has an EH (identical
bodies with try/catch -> identical tables, one per entry) and no mixed group.
The mixed case (EH member ≠ opened function) is covered by a synthetic test that
repoints a non-EH function of `try_catch_g0.hbc` at the body of `risky`.

The frameSize-bump refusal (SIGSEGV on the throw path) is checked on the
**whole group**: with `editAll` the new `frameSize` is written on every member,
so it is refused if any member has an EH.

History: an earlier version refused `editAll` whenever more than one member had an EH, and only
relocated the opened function's table - a group with a single EH member that
was not the opened function passed the check and kept stale EH offsets. Fixed
on 2026-09-29.

---

## See also

- [`bytecode-versions.md`](bytecode-versions.md) - why v96 is the
  version that matters.
