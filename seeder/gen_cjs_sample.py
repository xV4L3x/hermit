#!/usr/bin/env python3
"""
gen_cjs_sample.py - generates and compiles the CJS Module Table samples.

Hermes's native CommonJS mode (`hermes -commonjs`) is a separate compilation
path from the one React Native/Metro ever uses (Metro bundles everything into
a single file using its own `__d()` runtime convention instead -- see
recover_modules()). It is the only way to produce an .hbc file with
header.cjsModuleCount > 0, which seeder/compile.py's single-file pipeline
cannot do: it needs a directory with a metadata.json manifest
(doc/Modules.md in the Hermes source).

Produces two corpus files:
  cjs_modules.hbc              - 1 segment, require('./foo.js') (dynamic table)
  cjs_modules_segmented.hbc(.1) - 2 segments (main.js + lazily-loaded lazy.js)

Both use the plain `-commonjs` path (header.options bit 1 = 0, the dynamic
(stringID, functionID) entry shape). `-fstatic-require` was tried too (with
and without -O/-Og, with and without an explicit moduleIDs map) and produced
byte-identical output every time: ResolveStaticRequire::runOnModule is gated
on Context::getUseCJSModules(), hardcoded to return false in the public
Hermes source ("A hack to disable CJS modules while preserving the same
interface"). The static (moduleID, functionID) shape is therefore not
reachable from the public CLI compiler and has no sample here -- see
doc/hbc-format/v96-format.md §9.4.

Usage:
    cd hermit/
    python seeder/gen_cjs_sample.py [--hermes PATH]
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

CORPUS_DIR = Path(__file__).parent / "corpus"


def write_tree(root: Path, files: dict[str, str]) -> None:
    root.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        (root / name).write_text(content)


def compile_commonjs(hermes: str, src_dir: Path, out_path: Path) -> None:
    r = subprocess.run(
        [hermes, "-commonjs", "-emit-binary", "-out", str(out_path), str(src_dir)],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        print(f"hermes failed on {src_dir}:\n{r.stderr.strip()}")
        sys.exit(1)
    print(f"Compiled: {out_path}  ({out_path.stat().st_size} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hermes", default="hermes", help="Path to hermes binary")
    args = parser.parse_args()

    CORPUS_DIR.mkdir(exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)

        # --- cjs_modules.hbc: one segment, main.js requires foo.js ---
        single_dir = tmp_path / "cjs-single"
        write_tree(single_dir, {
            "main.js": "var foo = require('./foo.js');\nprint(foo.greet('world'));\n",
            "foo.js": "exports.greet = function (name) {\n  return 'hello ' + name;\n};\n",
            "metadata.json": '{\n  "segments": {\n    "0": ["./main.js", "./foo.js"]\n  }\n}\n',
        })
        compile_commonjs(args.hermes, single_dir, CORPUS_DIR / "cjs_modules.hbc")

        # --- cjs_modules_segmented.hbc(.1): two segments ---
        seg_dir = tmp_path / "cjs-segmented"
        write_tree(seg_dir, {
            "main.js": "print('main loaded');\n",
            "lazy.js": "exports.value = 42;\n",
            "metadata.json": (
                '{\n  "segments": {\n'
                '    "0": ["./main.js"],\n'
                '    "1": ["./lazy.js"]\n'
                "  }\n}\n"
            ),
        })
        out_path = CORPUS_DIR / "cjs_modules_segmented.hbc"
        compile_commonjs(args.hermes, seg_dir, out_path)
        # The CLI driver names non-zero segments "<out>.<segmentID>", next to
        # -out itself (CompilerDriver.cpp): segment 0 -> cjs_modules_segmented.hbc,
        # segment 1 -> cjs_modules_segmented.hbc.1, both already in CORPUS_DIR.
        seg1_path = out_path.parent / (out_path.name + ".1")
        if seg1_path.exists():
            print(f"Compiled: {seg1_path}  ({seg1_path.stat().st_size} bytes)")
        else:
            print("WARNING: segment 1 output not found; check hermes version")


if __name__ == "__main__":
    main()
