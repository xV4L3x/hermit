"""
bundle_metro.py - Mini Metro-format bundler for the hermit corpus.

Takes an ordered list of TypeScript files (in dependency order),
compiles each with tsc, rewrites local require() calls into _$$_REQUIRE(N)
and wraps each module in __d(factory, moduleId, deps) - exactly the
format produced by React Native's Metro bundler.

The resulting bundle is compiled by hermes into a .hbc file in the corpus.

Usage:
    python bundle_metro.py [--hermes PATH] [--tsc PATH] [--output NAME]
                           module0.ts module1.ts ... entryN.ts

    The last file is the entry point. The first gets module ID 0, the second 1, etc.

Example:
    python bundle_metro.py samples/metro/math_utils.ts \\
                           samples/metro/string_utils.ts \\
                           samples/metro/app.ts
    -> corpus/metro_module.hbc
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

import click
from rich.console import Console

console = Console()

CORPUS_DIR = Path(__file__).parent / "corpus"

# __d/__r polyfill that the Hermes runtime does NOT provide in standalone files.
# In a real React Native app this code does not exist in the bundle - the
# runtime provides it natively. We include it only to make the file executable.
_POLYFILL = """\
"use strict";
var __modules = {};
function __d(factory, id, deps) {
  __modules[id] = { factory: factory, deps: deps, exports: {}, initialized: false };
}
function __r(id) {
  var m = __modules[id];
  if (!m.initialized) {
    m.initialized = true;
    m.factory(
      {},
      function(depId) { return __r(depId); },
      null, null,
      { exports: m.exports },
      m.exports
    );
  }
  return m.exports;
}
"""


def _stem(path: Path) -> str:
    return path.stem


def _replace_requires(js_source: str, stem_to_id: dict[str, int]) -> str:
    """Replaces require("./mod") with _$$_REQUIRE(id) for local modules."""
    def _sub(m: re.Match) -> str:
        raw = m.group(1)
        # Normalize: "./math_utils" -> "math_utils"
        stem = re.sub(r"^\.{1,2}/", "", raw)
        stem = stem.split("/")[-1]          # strip any sub-paths
        if stem in stem_to_id:
            return f"_$$_REQUIRE({stem_to_id[stem]})"
        return m.group(0)                  # external dependency: leave unchanged

    # require("...") and require('...')
    js_source = re.sub(r'require\("([^"]+)"\)', _sub, js_source)
    js_source = re.sub(r"require\('([^']+)'\)", _sub, js_source)
    return js_source


@click.command()
@click.option("--hermes",  default="hermes", help="Path to hermes/hermesc binary.")
@click.option("--tsc",     default="tsc",    help="Path to tsc binary.")
@click.option("--output",  default="metro_module", help="Output basename (no extension).")
@click.argument("modules", nargs=-1, required=True, type=click.Path(exists=True))
def main(hermes: str, tsc: str, output: str, modules: tuple) -> None:
    """Bundle TS modules in Metro __d/__r format and compile to HBC."""

    CORPUS_DIR.mkdir(exist_ok=True)

    paths   = [Path(m) for m in modules]
    stems   = [_stem(p) for p in paths]
    stem_to_id = {s: i for i, s in enumerate(stems)}

    console.print(f"[bold]Bundling {len(paths)} modules -> [cyan]{output}.hbc[/]")
    for i, (s, p) in enumerate(zip(stems, paths)):
        console.print(f"  [{i}] [dim]{p}[/]  [yellow]{s}[/]")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)

        # 1. Compile each .ts -> .js with tsc
        js_parts: list[str] = []
        for ts_path in paths:
            r = subprocess.run(
                [tsc, "--target", "ES6", "--module", "commonjs",
                 "--outDir", tmpdir, str(ts_path)],
                capture_output=True, text=True,
            )
            if r.returncode != 0:
                console.print(f"[red]tsc failed on {ts_path.name}:[/]\n{r.stderr.strip()}")
                sys.exit(1)
            js_path = tmp / (ts_path.stem + ".js")
            js_parts.append(js_path.read_text())

        # 2. Build the Metro-format bundle
        bundle_lines = [_POLYFILL]
        for i, (stem, js_source) in enumerate(zip(stems, js_parts)):
            transformed = _replace_requires(js_source, stem_to_id)
            # Indent the factory body by 2 spaces (readability)
            body = "\n".join("  " + line for line in transformed.splitlines())
            bundle_lines.append(
                f"__d(function(global, _$$_REQUIRE, _$$_IMPORT_DEFAULT, "
                f"_$$_IMPORT_ALL, module, exports) {{\n"
                f"{body}\n"
                f"}}, {i}, []);\n"
            )

        entry_id = len(paths) - 1
        bundle_lines.append(f"\n__r({entry_id});\n")
        bundle_js = "\n".join(bundle_lines)

        # 3. Write the bundle .js into tmpdir
        bundle_js_path = tmp / f"{output}.js"
        bundle_js_path.write_text(bundle_js)
        console.print(f"  [dim]bundle JS: {len(bundle_js)} bytes[/]")

        # 4. Compile with hermes -> .hbc
        hbc_path = CORPUS_DIR / f"{output}.hbc"
        r = subprocess.run(
            [hermes, "-emit-binary", "-out", str(hbc_path), str(bundle_js_path)],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            console.print(f"[red]hermes failed:[/]\n{r.stderr.strip()}")
            sys.exit(1)

        console.print(f"  [green]-> {hbc_path}  ({hbc_path.stat().st_size} bytes)[/]")


if __name__ == "__main__":
    main()
