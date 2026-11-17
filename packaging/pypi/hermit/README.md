# hermit

**Hermes Reverse Engineering & Manipulation Interactive Toolkit** — an
interactive (GDB/pwndbg-style) REPL to disassemble, decompile and **structurally
patch** Hermes bytecode (HBC) from React Native apps.

```bash
pipx install hermit-hbc
hermit app.hbc
```

This package is the interactive CLI. The compiled parser/patcher engine ships as
the separate [`libhbc`](https://pypi.org/project/libhbc/) distribution, pulled in
automatically as a dependency; install it on its own (`pip install libhbc`) to use
the native `_libhbc` bindings directly.

Supported HBC versions: 94, 95, 96 (every React Native app from RN 0.73 to 0.84)
and 98 (Static Hermes / Hermes V1).

Source, documentation and issues: <https://github.com/xV4L3x/hermit>
