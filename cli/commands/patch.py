"""`patch` command: writes the in-memory patched image to a new .hbc file."""

import os

import _libhbc


class PatchCommand:
    def cmd_patch(self, args: list[str]) -> None:
        """patch [file.hbc]  - writes the patched image (from `edit` + Ctrl+X->Y) to a new .hbc"""
        proj = self._require_project()
        if not proj.is_patched:
            self.console.print(
                "No changes applied. Use [bold]edit func:<N>[/], edit, "
                "then [bold]Ctrl+X[/] -> [bold]Y[/]."
            )
            return

        stem = os.path.splitext(os.path.basename(proj.hbc_path))[0]
        default_out = os.path.join(os.path.dirname(proj.hbc_path), f"{stem}_patched.hbc")

        if args:
            out_path = os.path.abspath(os.path.expanduser(args[0]))
        else:
            try:
                raw = self.session.prompt("File output: ", default=default_out)
            except (EOFError, KeyboardInterrupt):
                self.console.print("[yellow]Cancelled.[/]")
                return
            out_path = os.path.abspath(os.path.expanduser(raw.strip() or default_out))

        if out_path == os.path.abspath(proj.hbc_path):
            self.console.print(
                "[red]Refusing to overwrite the original file.[/] Choose another name."
            )
            return

        try:
            n_bytes = proj.write_patched(out_path)
        except Exception as exc:
            self.console.print(f"[red]patch failed:[/] {exc}")
            return

        # sanity check: the written file must reopen
        try:
            _libhbc.HBCFile.open(out_path)
            check = "reopened OK"
        except Exception as exc:
            check = f"[yellow]WARN: reopen failed ({exc})[/]"
        self.console.print(
            f"[green]Written[/] -> [bold]{out_path}[/]  "
            f"({n_bytes} bytes, SHA-1 footer updated) - {check}"
        )
