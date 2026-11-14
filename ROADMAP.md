# hermit — Roadmap

Planned and in-progress features, grouped by area. This is a feature backlog, not a
schedule. Short IDs (e.g. `B30`) are stable internal tracking tags used by commit
messages and detailed notes.

Status legend: **[done]** shipped · **[wip]** partially implemented · **[planned]** not started.

## Recently shipped

- **[done]** Identifier strings: add and rename (`B3`) — delete still planned.
- **[done]** Closure variable names for outer scopes via `GetEnvironment` (`B19`).
- **[done]** BigInt table decoding (`B20`, RegExp table still planned).
- **[done]** Cross-references of strings used inside array/object literal buffers (`B28`).
- **[done]** Metro module reconstruction, steps 1–2: module recovery and
  module-aware analysis (`B30`, more steps planned).
- **[done]** Full English translation of code and output (`B7`).

## Parser & format

- **[wip]** `B20` RegExp table contents (BigInt side done).
- **[planned]** `B16` interpret header fields left unparsed: native CommonJS, `segmentID`.
- **[planned]** `B17` multi-source-file debug info (`StringTableEntry` / `FileRegion`).
- **[planned]** `B18` determine the Scope Descriptor `flags` byte semantics.
- **[planned]** `B27` HBC v97/v98 support (Hermes V1, default from RN 0.84).
- **[planned]** `B34` dump of additional sections (object shapes, regexp, cjs-modules).
- **[planned]** `B37` read legacy HBC versions (40–93).

## Decompiler

- **[planned]** `B21` reconstruct structured `switch` from `SwitchImm` (currently a flat dump).
- **[planned]** `B23` iteration protocol for generators and `for-in`/`for-of`
  (CFG resume edges exist; the decompiler is still resume-free).
- **[planned]** `B24` reconstruct computed-key getter/setter pairs
  (`PutOwnGetterSetterByVal`).
- **[planned]** `B25` recognise and suppress the `ThrowIfHasRestrictedGlobalProperty`
  engine guard paired with `DeclareGlobalVar`.
- **[planned]** `B22` JS semantics for the WASM/asm.js opcodes (`0xC0`–`0xCD`).
- **[planned]** `B29` deeper, stable deterministic name recovery
  (inter-procedural / closure-aware).
- **[planned]** `B4` JavaScript-level editor: edit the decompiled JS and recompile,
  instead of editing raw bytecode.

## Patcher & assembler

- **[wip]** `B3` Identifier strings: delete (add and rename done).
- **[planned]** `B1` quick patching macro commands composing the insert / same-size /
  string-rebuild primitives into one-shot actions.
- **[planned]** `B2` removal of a whole function (reclaim + relocation).
- **[planned]** `B8` edit and add non-ASCII (UTF-16) string entries.
- **[planned]** `B9` promote a non-overflow string entry into the overflow table.
- **[planned]** `B10` literal edit on a function whose bytecode body is shared.
- **[planned]** `B11` insert/remove at an arbitrary position inside a literal
  (currently append-only).
- **[planned]** `B12` clone-on-edit of a shared bytecode body so siblings are unaffected.
- **[planned]** `B13` editable metadata for `add_function` (paramCount, etc.).
- **[planned]** `B5` array/object buffer promotion to the `*Long` opcodes
  (`bufferIdx` past 65535).
- **[planned]** `B38` route new string storage past the 8 MB / 23-bit offset limit.
- **[planned]** `B39` preserve isolated UTF-16 surrogates when editing strings.
- **[planned]** `B26` fix the latent `frameSize`-growth bug (breaks the Call/Construct
  argument window).
- **[planned]** `B36` `create` / `inject-stub`: build an HBC from scratch or inject a stub.
- **[planned]** `B6` persist patches across sessions (currently in-memory only).

## Static analysis

- **[wip]** `B30` Metro module reconstruction, further steps: per-module split with
  dependencies (`extract` / `modules` / `deps`).
- **[planned]** `B40` track call return values inter-procedurally (e.g. Babel classes).
- **[planned]** `B41` recognise the namespace-import / `export *`
  (`_interopRequireWildcard`) interop pattern.
- **[planned]** `B35` dedicated inter-procedural call-graph export (graphviz/DOT).
- **[planned]** `B31` secrets scanner: auto-detect API keys/tokens in bytecode.
- **[planned]** `B33` diff between two bundles (compare two app versions).

## Integration & tooling

- **[planned]** `B14` MCP server + AI-assisted symbol renaming on top of the
  deterministic decompiler.
- **[planned]** `B15` validation on real shipped apps as case studies.
- **[planned]** `B32` generate ready-to-use Frida instrumentation scripts.
