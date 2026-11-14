#!/usr/bin/env python3
"""Generate bindings/_libhbc.pyi (the type stub for the native module).

pybind11-stubgen recovers most signatures from the compiled module, but it
cannot infer the element type of the containers returned by const-reference
accessors, leaving them as `list[...]`. This script runs stubgen and then
patches those known return (and a few argument) annotations back to their real
types, so an IDE sees the full analysis API.

Usage (module must be importable, i.e. built):

    PYTHONPATH=build/bindings .venv/bin/python bindings/genstub.py

Requires pybind11-stubgen (`pip install pybind11-stubgen`); it is a dev-only
tool, not a runtime dependency.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "_libhbc.pyi"

# method name -> real return annotation (stubgen emits `list[...]` / `...`)
RETURNS = {
    "bigints": "list[BigIntInfo]",
    "call_graph": "list[CallSite]",
    "captured_environment": "EnvKey | None",
    "cfg": "CFGData",
    "cjs_modules": "list[CJSModuleInfo]",
    "debug_info_header": "DebugInfoHeader",
    "debug_strings": "list[str]",
    "env_accesses": "list[EnvAccess]",
    "env_slot_uses": "list[EnvSlotUse]",
    "env_slot_value": "AbstractValue | None",
    "environments": "list[EnvironmentInfo]",
    "function_callers": "list[CallerSite]",
    "function_modules": "list[int | None]",
    "functions": "list[FunctionInfo]",
    "module_exports": "ModuleExports",
    "recover_modules": "list[MetroModule]",
    "resolve_value": "AbstractValue",
    "resolved_calls": "list[ResolvedCall]",
    "scope_variables": "list[ScopeVariable]",
    "source_files": "list[str]",
    "source_locations": "list[SourceLocation]",
    "strings": "list[str]",
    "string_xrefs": "list[StringXRef] | StringXRef | None",
    "textified_callees": "list[TextifiedCallee]",
    "value_flow": "list[InstrValues]",
}

# argument name -> real type (stubgen emits `...` for custom-class args)
ARG_TYPES = {
    "env": "EnvKey",
    "value": "AbstractValue",
}


def patch(text: str) -> str:
    for name, ret in RETURNS.items():
        # `def name(<args>) -> <whatever>:`  ->  keep args, fix the return
        text, n = re.subn(
            rf"(\n(\s*)def {re.escape(name)}\(self[^\n]*?\) -> )[^\n:]+:",
            rf"\g<1>{ret}:",
            text,
        )
        if n == 0:
            print(f"warning: no match for return of {name}()", file=sys.stderr)
    for arg, typ in ARG_TYPES.items():
        text = re.sub(rf"(\b{re.escape(arg)}: )\.\.\.", rf"\g<1>{typ}", text)
    return text


def main() -> int:
    try:
        import pybind11_stubgen  # noqa: F401
    except ImportError:
        print("pybind11-stubgen not installed: pip install pybind11-stubgen", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(
            [sys.executable, "-m", "pybind11_stubgen", "_libhbc", "-o", tmp],
            check=True,
        )
        generated = Path(tmp) / "_libhbc.pyi"
        text = generated.read_text()

    text = patch(text)
    if "..." in re.sub(r"\.\.\.(\s*#|\s*$|\s*\n)", "", text):
        # ellipsis bodies (`...`) are fine; flag only leftover `...` in signatures
        leftover = [ln for ln in text.splitlines()
                    if "->" in ln and "..." in ln.split("->", 1)[1]]
        for ln in leftover:
            print(f"warning: unresolved return still present: {ln.strip()}", file=sys.stderr)

    OUT.write_text(text)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
