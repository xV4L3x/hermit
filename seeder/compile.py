"""Compile TypeScript/JavaScript samples in seeder/samples/ to HBC files in seeder/corpus/."""

import subprocess
import sys
import tempfile
from pathlib import Path

import click
from rich.console import Console
from rich.table import Table

console = Console()

SAMPLES_DIR = Path(__file__).parent / "samples"
CORPUS_DIR = Path(__file__).parent / "corpus"


@click.command()
@click.option("--hermes", default="hermes", help="Path to hermes/hermesc binary.")
@click.option("--tsc", default="tsc", help="Path to tsc binary.")
@click.option(
    "--debug", "-g", "debug", is_flag=True,
    help="Compile with `hermes -g` (embeds full debug info: source locations, scope "
         "descriptors, textified callees). Output goes to <stem>_g.hbc.",
)
@click.option(
    "--g0", "g0", is_flag=True,
    help="Compile with `hermes -g0` (NO debug info at all). Output goes to "
         "<stem>_g0.hbc. Used by structural patching so there is no"
         "source-location stream to relocate.",
)
@click.argument("files", nargs=-1, type=click.Path(exists=True))
def main(hermes: str, tsc: str, debug: bool, g0: bool, files: tuple) -> None:
    """Compile TypeScript/JavaScript samples to HBC bytecode corpus.

    Without arguments, compiles every .ts file in seeder/samples/ (plus any .js
    file with no .ts sibling). Pass explicit FILE paths to compile a subset;
    `.js` files skip `tsc`.
    """
    if debug and g0:
        console.print("[red]--debug and --g0 are mutually exclusive.[/red]")
        sys.exit(2)

    CORPUS_DIR.mkdir(exist_ok=True)

    if files:
        samples = [Path(f) for f in files]
    else:
        ts = sorted(SAMPLES_DIR.glob("*.ts"))
        ts_stems = {p.stem for p in ts}
        js_only = [p for p in sorted(SAMPLES_DIR.glob("*.js")) if p.stem not in ts_stems]
        samples = ts + js_only

    if not samples:
        console.print("[yellow]No .ts/.js files found.[/yellow]")
        return

    table = Table(show_header=True)
    table.add_column("Sample")
    table.add_column("Status")
    table.add_column("Detail")

    ok = fail = 0
    suffix = "_g" if debug else ("_g0" if g0 else "")

    with tempfile.TemporaryDirectory() as tmpdir:
        for src_path in samples:
            hbc_path = CORPUS_DIR / (src_path.stem + suffix + ".hbc")

            if src_path.suffix == ".js":
                js_path = src_path
            else:
                js_path = Path(tmpdir) / (src_path.stem + ".js")
                r = subprocess.run(
                    [tsc, "--target", "ES6", "--module", "commonjs",
                     "--outDir", tmpdir, str(src_path)],
                    capture_output=True, text=True,
                )
                if r.returncode != 0:
                    table.add_row(src_path.name, "[red]tsc failed[/red]", r.stderr.strip())
                    fail += 1
                    continue

            hermes_args = [hermes, "-emit-binary"]
            if debug:
                hermes_args.append("-g")
            elif g0:
                hermes_args.append("-g0")
            hermes_args += ["-out", str(hbc_path), str(js_path)]

            r = subprocess.run(hermes_args, capture_output=True, text=True)
            if r.returncode != 0:
                table.add_row(src_path.name, "[red]hermes failed[/red]", r.stderr.strip())
                fail += 1
                continue

            table.add_row(src_path.name, "[green]ok[/green]", str(hbc_path.relative_to(Path.cwd())))
            ok += 1

    console.print(table)
    console.print(f"\n[bold]{ok} compiled, {fail} failed[/bold]")
    if fail:
        sys.exit(1)


if __name__ == "__main__":
    main()
