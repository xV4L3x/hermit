# Hermit

**Hermes Reverse Engineering & Manipulation Interactive Toolkit.**

hermit inspects, decompiles and structurally patches Hermes bytecode (HBC), the
format React Native apps ship their JavaScript in. Open an `.hbc` file, browse
its functions, read the decompiled JavaScript, edit instructions, strings and
literals in an interactive editor, and write back a file that the Hermes VM
still loads and runs.

Unlike tools that only analyse bytecode, or that patch it in place without
changing sizes, hermit re-serializes the whole file. It can insert and delete
instructions, relocate jumps and exception handlers, rebuild the string and
literal tables, add new functions and recompute the SHA-1 footer. An unmodified
file round-trips byte for byte.

- [Features](#features)
- [Supported HBC versions](#supported-hbc-versions)
- [Prebuilt binaries](#prebuilt-binaries)
- [Build](#build)
- [Getting an `.hbc` file](#getting-an-hbc-file)
- [Using the REPL](#using-the-repl)
- [Editing and patching](#editing-and-patching)
- [Command-line tools](#command-line-tools)
- [Python API](#python-api)
- [Tests](#tests)
- [Project layout](#project-layout)
- [Limitations](#limitations)

## Features

- **Parser**: header, string table (UTF-16 and overflow entries included),
  function table (overflow headers and shared bytecode bodies included),
  literal buffers, exception handlers, debug info, function source table.
- **Disassembler** with per-version opcode tables (94, 95, 96).
- **Control-flow graph** with DOT export, call graph, string cross-references,
  Metro module recovery.
- **Decompiler** from the CFG to readable JavaScript: if/else, loops,
  try/catch, closures, array and object literals. Deterministic, no AI.
- **Structural patcher**: edit a function body, change string and literal
  values, add a function. Branch relaxation, exception-handler relocation,
  overflow function headers, byte-identical round-trip.
- **Interactive REPL** (GDB style) with a full-screen bytecode editor.

## Supported HBC versions

| Version | Role |
|---|---|
| **96** | Primary target. What shipping React Native apps use: RN 0.73 to 0.83, and RN 0.84+ with Hermes V1 disabled. |
| 94, 95 | Validated. The format is identical to 96, only the opcode tables differ. |

Apps built with Hermes V1 (the default from RN 0.84) emit HBC 98, which is not
supported yet. Format notes are in [`doc/hbc-format/`](doc/hbc-format/).

## Prebuilt binaries

Each [release](https://github.com/xV4L3x/hermit/releases) has archives for
macOS and Linux, on arm64 and x86_64. No Python or compiler is needed:

| Archive | Content |
|---|---|
| `hermit-<version>-<platform>.tar.gz` | The interactive REPL: `./hermit file.hbc`. |
| `hermit-tools-<version>-<platform>.tar.gz` | The standalone `hbc_*` command-line tools. |
| `SHA256SUMS` | Checksums: `shasum -a 256 -c SHA256SUMS`. |

```bash
tar -xzf hermit-0.1.0-macos-arm64.tar.gz
./hermit-0.1.0-macos-arm64/hermit --version
```

The macOS binaries are not signed. After downloading, clear the quarantine flag
once: `xattr -dr com.apple.quarantine hermit-0.1.0-macos-arm64`. The Linux builds
need glibc 2.35 or newer. The `cfg` command still needs `graph-easy` installed.

Releases are produced by `.github/workflows/release.yml` when a `v*` tag is
pushed, using `packaging/build-release.sh`, which you can also run locally.

## Build

### Requirements

- CMake 3.20 or newer and a C++20 compiler (Clang or GCC).
- Python 3.10+ with development headers (needed by pybind11).
- Network access on the first configure: pybind11 and Catch2 are fetched with
  CMake `FetchContent` unless they are already installed.
- Optional: [`graph-easy`](https://metacpan.org/pod/Graph::Easy) for the `cfg`
  command (`cpanm Graph::Easy`), `less` as its pager.

### Compile

Run everything from this directory (the one containing `CMakeLists.txt`):

```bash
cmake -B build
cmake --build build -j
```

This builds the `libhbc` static library, the Python module `_libhbc` (in
`build/bindings/`), the command-line tools (in `build/tools/`) and the unit
tests. The REPL only needs the Python module:

```bash
cmake --build build --target _libhbc
```

The tools and the tests can be left out of a build, for example for a release:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DHERMIT_BUILD_TOOLS=OFF -DHERMIT_BUILD_TESTS=OFF
```

Rebuild with the same command after every change to `core/` or `bindings/`.

> **macOS:** if linking fails with an error about a malformed SDK, point the
> build at a complete SDK, for example
> `SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk cmake --build build`.

### Python environment

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

The module `_libhbc` is not installed into the virtualenv. Add its build
directory to `PYTHONPATH` whenever you run hermit:

```bash
export PYTHONPATH=$PWD/build/bindings:$PYTHONPATH
```

## Getting an `.hbc` file

A React Native app ships its bundle as a plain file:

| Platform | Location inside the package |
|---|---|
| Android (`.apk`, a zip) | `assets/index.android.bundle` |
| iOS (`.ipa`, a zip) | `Payload/<App>.app/main.jsbundle` |

If the file starts with the bytes `C6 1F BC 03 C1 03 19 1F`, it is Hermes
bytecode. The REPL only opens files with the `.hbc` extension, so rename it:

```bash
unzip -p app.apk assets/index.android.bundle > app.hbc
xxd app.hbc | head -1      # c61f bc03 c103 191f ...
```

Only analyse apps you are allowed to study, for example your own, or
open-source apps for research.

### Building test bundles from source

To get small, controlled inputs, compile JavaScript or TypeScript with a Hermes
compiler that emits HBC 96 (for example `hermesc` from `react-native` 0.73 to
0.83, under `node_modules/react-native/sdks/hermesc/`):

```bash
hermes -emit-binary -g0 -out hello.hbc hello.js     # -g0: no debug info
```

The `seeder/` package automates this for the sample programs in
`seeder/samples/` and writes the results to `seeder/corpus/`:

```bash
.venv/bin/python -m seeder.compile                       # all samples
.venv/bin/python -m seeder.compile --g0 seeder/samples/loop_sum.js
.venv/bin/python -m seeder.compile --debug seeder/samples/greeting.js
```

`--g0` writes `<name>_g0.hbc` without debug info, `--debug` writes
`<name>_g.hbc` with full debug info. `.ts` samples need `tsc` on your `PATH`;
the compiler is taken from `--hermes` (default `hermes`). `seeder/build-real-bundle.sh`
builds a real open-source React Native app into an `.hbc`.

## Using the REPL

```bash
export PYTHONPATH=$PWD/build/bindings:$PYTHONPATH
.venv/bin/python -m cli.main app.hbc
```

Without a file argument the REPL starts empty; use `open <file.hbc>`. The
prompt shows the HBC version and the file name, with a `+` when there are
unsaved changes: `[hbc v96 | app+] >`. History is kept in `~/.hermit_history`
and commands are tab-completed.

```
> info
> strings --identifiers filter:fetch
> functions
> disasm func:12
> decompile func:isPremium
> xrefs string:"api.example.com"
> edit func:isPremium
> patch patched.hbc
```

Functions are addressed by index or name (`func:12`, `func:isPremium`), strings
by index or value (`string:42`, `string:"hello"`), literal sites by index
(`array:7`).

| Command | What it does |
|---|---|
| `open <file.hbc>` | Opens a file. |
| `info` | Header summary: version, sizes, counts, global function, source hash. |
| `strings [--identifiers\|--strings] [filter:<text>]` | String table with kinds. |
| `arrays [--arrays\|--objects] [filter:<text>] [func:<N\|name>]` | Array and object literal sites, decoded. |
| `functions` | All functions with parameters, registers and flags; Metro module ids. |
| `disasm func:<N\|name>` | Disassembly with source lines. `s` toggles the debug source table. |
| `src [func:<N\|name>]` | Debug source table, for all functions or one. |
| `cfg func:<N\|name> [--compact=true\|false]` | Control-flow graph rendered with graph-easy. |
| `xrefs` | String cross-reference summary. |
| `xrefs string:<value\|N>` | Every instruction that uses a string. |
| `xrefs func:<N\|name> [--callers] [--callees]` | Call graph around a function. |
| `decompile func:<N\|name>` | Decompiled JavaScript. |
| `decompile all [--output=<dir>]` | Decompile every function to `.js` files. |
| `edit func:<N\|name\|new>` | Bytecode editor; `func:new` builds a new function. |
| `edit string:<idx\|value\|new>` | String editor; `string:new` adds a string. |
| `edit array:<N>` | Array and object literal editor. |
| `patch [file.hbc]` | Writes the edited image to a new file. |
| `help`, `exit` | Help and quit. |

Long listings open in a scrollable viewer: arrow keys, PgUp/PgDn, `g`/`G` for
top and bottom, `/` to search as you type, `Esc` to clear the search, `q` to
leave.

## Editing and patching

Edits happen in memory. `edit ...` opens a full-screen editor; confirming with
`Ctrl+X` then `Y` applies the change to the in-memory image after an integrity
check (the file is re-parsed and compared with what was expected). Nothing is
written to disk until `patch`, which never overwrites the original file.

**Instruction editor** (`edit func:`):

| Key | Action |
|---|---|
| Left / Right | Move between the mnemonic and the operands. |
| Up / Down, PgUp / PgDn | Move between instructions. |
| `Enter` | Edit the selected token. A mnemonic can become any opcode with the same operand signature; operands are validated against their type. |
| `o` / `O` | Insert an instruction below / above (opcode picker). For a jump, `Enter` on its address operand lets you choose the target. |
| `Ctrl+K` | Delete the current instruction. |
| `R` | Restore the current line. |
| `Ctrl+Z` / `Ctrl+R` | Undo / redo. |
| `Ctrl+X` | Save and exit (Y applies, N discards, Esc cancels). |

Same-size edits are spliced in place. Inserting or deleting instructions goes
through the structural serializer, which relaxes short jumps to long ones when
needed, remaps exception-handler ranges and rewrites every offset that moved.

**String editor** (`edit string:`): a one-line editor. Same-length values are
written in place; any other length appends the new bytes to the string storage
and re-points the entry, which also detaches strings that share storage.
`Ctrl+N` switches to adding a new string. Identifiers are read-only, because
they are tied to a hash table.

**Literal editor** (`edit array:`): select an element with Up and Down and press
`Enter` to retype it. `Ctrl+T` changes its type (including `null`, `true` and
`false`), `a` appends an element or key-value pair and `d` removes the last one.

**Shared bytecode.** Metro and Hermes store identical function bodies once, so
several functions can point to the same bytes. The editor warns about it and
asks whether to apply the edit to all of them; editing just one is not
supported.

A typical session patches an in-app check and runs the result with the Hermes VM
of the same version:

```
> edit func:isPremium        # change a LoadConstFalse into LoadConstTrue
> patch isPremium_patched.hbc
```
```bash
hermes isPremium_patched.hbc
```

## Command-line tools

Small standalone tools are built in `build/tools/`:

| Tool | Purpose |
|---|---|
| `hbc_dump <file>` | Every header field. |
| `hbc_section_map <file>` | Offset, size and alignment of every section, with a coverage check. |
| `hbc_functions <file>` | The function table. |
| `hbc_strings <file>` | Strings with their kind. |
| `hbc_disasm <file> [func:N] [-o out.txt]` | Disassembly of all functions or one. |
| `hbc_roundtrip <in> <out>` | Parse and re-serialize; compare with `cmp`. |
| `hbc_patch <in> --offset <hex> --value <hex> -o <out>` | Overwrite one byte (does not fix the SHA-1 footer). |

Helper scripts in `tools/`: `inspect_real_bundle.py <bundle.hbc>` summarizes the
structural features of a bundle (shared bodies, overflow functions, exception
handlers, UTF-16 strings), and `gen_opcode_table_typ.py` generates the opcode
table of the thesis.

## Python API

The REPL is a thin layer over the `_libhbc` module, which you can use directly:

```python
import _libhbc

f = _libhbc.HBCFile.open("app.hbc")
print(f.version(), len(f.functions()), "functions")

for fn in f.functions()[:5]:
    print(fn.index, fn.name, fn.bytecodeSizeInBytes)

print(f.decompile(3))                    # JavaScript for function 3
for ins in f.disasm(3):
    print(f"{ins.offset:#06x} {ins.mnemonic}")
cfg = f.cfg(3)                           # basic blocks and edges
```

Structural patching goes through `Module`: queue one kind of edit, serialize,
write.

```python
m = _libhbc.Module.from_hbcfile(f)
idx = m.add_string("hello from hermit")  # index of the new string
open("patched.hbc", "wb").write(m.serialize())
```

A `Module` accepts one kind of edit per `serialize()` (`set_function_body`,
`add_string`, `set_string_value`, `set_literal_value`, `resize_literal` or
`add_function`); for several edits, apply them one after the other, re-opening
the result each time.

## Tests

```bash
ctest --test-dir build --output-on-failure   # C++ unit tests (Catch2)
.venv/bin/python -m pytest tests/            # integration tests
```

Most tests read files from `seeder/corpus/`, which is generated and not stored
in git. A test whose input is missing is skipped, so generate the corpus first
(see [Building test bundles from source](#building-test-bundles-from-source)).
Some tests also use real bundles under `seeder/real-apps/`.

Many tests patch a file and run it on the Hermes VM to check its output. They
need a `hermes` binary on your `PATH` (HBC 96), and for the multi-version tests
a binary per version: `$HERMES94` and `$HERMES95`, or
`~/.local/share/hermit/hermes/hermes94` and `hermes95`. Tests whose VM is not
available are skipped.

## Project layout

| Path | Content |
|---|---|
| `core/` | C++ library `libhbc`: parser, disassembler, CFG, analysis, decompiler, patcher. Public API in `core/include/hbc/hbc.h`. |
| `bindings/` | pybind11 module `_libhbc`. |
| `cli/` | Python REPL (`repl.py`), project state (`project.py`), output formatting, and the editors in `editor/`. |
| `tools/` | Standalone C++ command-line tools and helper scripts. |
| `packaging/` | Release build: PyInstaller entry point and `build-release.sh`. |
| `seeder/` | Corpus generation: samples compiled with the Hermes compiler. |
| `tests/` | Catch2 unit tests (`unit/`) and pytest integration tests (`integration/`). |
| `doc/` | HBC format notes. |

## Limitations

- HBC 98 (Hermes V1) is not supported.
- Functions containing `SwitchImm` or generator state (`SaveGenerator`) cannot
  be edited, and those opcodes cannot be inserted.
- Increasing the register count of a function with an exception handler is
  rejected: it crashes the Hermes runtime.
- New strings are ASCII only. A string cannot grow from the small to the
  overflow format, and non-BMP characters cannot be written into UTF-16 strings.
- A new function has fixed metadata: one parameter (`this`), no name, strict
  mode, no exception handlers or debug info.
- Debug source locations are not re-encoded after a structural edit, so stack
  traces may show wrong lines. Compile with `-g0` to avoid that.

## Author

Valerio De Nicola. Released under the [MIT License](LICENSE).
