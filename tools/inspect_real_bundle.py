#!/usr/bin/env python3
"""Inspect real-app HBC bundles for the structural features the patcher tests need.

Run:
    export PYTHONPATH=build/bindings:$PYTHONPATH
    .venv/bin/python tools/inspect_real_bundle.py seeder/real-apps/*.hbc

For each bundle it reports: version, #functions, #strings; shared-body groups
(the resize-shared test); overflow function headers; functions with an exception
handler (the EH-relocation test); UTF-16 and overflow string entries. It also
prints concrete candidate indices to use as test targets.
"""
import os
import sys
from collections import defaultdict

import _libhbc


def inspect(path):
    hbc = _libhbc.HBCFile.open(path)
    funcs = list(hbc.functions())
    strings = list(hbc.string_entries())
    version = hbc.version()

    # shared bodies: same (offset, size)
    groups = defaultdict(list)
    for i, f in enumerate(funcs):
        groups[(f.offset, f.bytecodeSizeInBytes)].append(i)
    shared_groups = {k: v for k, v in groups.items() if len(v) > 1}
    shared_members = sum(len(v) for v in shared_groups.values())

    overflow_funcs = [i for i, f in enumerate(funcs) if f.overflowed]
    eh_funcs = [i for i, f in enumerate(funcs) if f.hasExceptionHandler]
    eh_single = [i for i in eh_funcs if not funcs[i].shared]

    utf16 = [j for j, s in enumerate(strings) if s.is_utf16]
    ovf_str = [j for j, s in enumerate(strings) if s.is_overflow]

    # a shared group to use as a test target (the smallest body)
    sample_group = None
    if shared_groups:
        (off, size), members = min(shared_groups.items(), key=lambda kv: kv[0][1])
        sample_group = (off, size, members[:6])

    print(f"\n=== {os.path.basename(path)} ===")
    print(f"  version            : {version}")
    print(f"  functions          : {len(funcs)}")
    print(f"  strings            : {len(strings)}")
    print(f"  shared-body groups : {len(shared_groups)}  "
          f"({shared_members} funcs, {100*shared_members//max(1,len(funcs))}%)")
    if sample_group:
        off, size, members = sample_group
        print(f"    sample group     : off={off:#x} size={size} members={members}")
    print(f"  overflow functions : {len(overflow_funcs)}"
          + (f"  e.g. {overflow_funcs[:5]}" if overflow_funcs else ""))
    print(f"  funcs with EH      : {len(eh_funcs)}"
          + (f"  (non-shared e.g. {eh_single[:5]})" if eh_single else "  (none non-shared!)"))
    print(f"  UTF-16 strings     : {len(utf16)}"
          + (f"  e.g. {utf16[:5]}" if utf16 else ""))
    print(f"  overflow strings   : {len(ovf_str)}"
          + (f"  e.g. {ovf_str[:5]}" if ovf_str else ""))

    # EH-resize target: a non-shared function with a non-empty EH table
    for i in eh_single:
        ehs = list(funcs[i].exceptionHandlers)
        if ehs:
            print(f"  EH-resize target   : func {i}  "
                  f"EH={[(e.start, e.end, e.target) for e in ehs][:3]}")
            break


if __name__ == "__main__":
    paths = sys.argv[1:]
    if not paths:
        print(__doc__)
        sys.exit(2)
    for p in paths:
        try:
            inspect(p)
        except Exception as e:
            print(f"\n=== {os.path.basename(p)} ===\n  ERROR: {e}")
