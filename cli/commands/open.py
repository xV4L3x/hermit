"""`open` command: opens an HBC file into the project."""

import os

import _libhbc
from cli.project import HermitProject


class OpenCommand:
    def cmd_open(self, args: list[str]) -> None:
        """open [--permissive] <file.hbc>  - opens an HBC file (permissive: read-only, tolerates bad string entries)"""
        permissive = "--permissive" in args
        args = [a for a in args if a != "--permissive"]
        if len(args) != 1:
            self.console.print("Usage: open [--permissive] <file.hbc>")
            return
        self._open_file(args[0], permissive)

    def _open_file(self, path: str, permissive: bool = False) -> None:
        path = os.path.expanduser(os.path.abspath(path))
        if not os.path.exists(path):
            raise FileNotFoundError(f"File not found: {path}")

        ext = os.path.splitext(path)[1].lower()
        if ext != ".hbc":
            raise ValueError(f"Unknown extension {ext!r} - expected .hbc")
        proj = HermitProject.from_hbc(path, permissive)
        try:
            proj.hbc.string_entries()   # strict: fail here, not at the first command
        except _libhbc.InvalidStringEntryError as exc:
            raise ValueError(f"{exc}\nUse `open --permissive <file>` to analyse it read-only") from exc

        self.project = proj
        name = os.path.splitext(os.path.basename(proj.hbc_path))[0]
        ver  = proj.hbc.version()
        hdr  = proj.hbc.header()

        self.console.print(
            f"[green]Opened[/] [bold]{name}[/] "
            f"(HBC v{ver}, {hdr.function_count} functions, {hdr.string_count} strings)"
        )
        if permissive:
            bad = [e for e in proj.hbc.string_entries()
                   if e.verdict != _libhbc.StringVerdict.InsideStorage]
            self.console.print(
                f"[yellow]Permissive, read-only:[/] {len(bad)} string entries outside the "
                "string storage (see `info strings`)")
