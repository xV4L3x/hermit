"""`decompile` command: HBC -> pseudo-JS for a function, a module, or all."""

import os

from cli import display
from cli import modules_view


class DecompileCommand:
    def cmd_decompile(self, args: list[str]) -> None:
        """decompile func:<N|name> | decompile module:<id> | decompile all [--output=<dir>]  - decompiles to pseudo-JS (module: every function of a Metro module in sequence)"""
        proj = self._require_project()
        if not args:
            self.console.print("Usage: decompile func:<N|name>  |  decompile module:<id>  |  "
                               "decompile all [--output=<dir>]")
            return

        if args[0] == "all":
            output_dir = next(
                (a.split("=", 1)[1] for a in args[1:] if a.startswith("--output=")), None
            )
            if output_dir is None:
                output_dir = os.path.join(os.path.dirname(proj.hbc_path), "decompiled")
            os.makedirs(output_dir, exist_ok=True)

            funcs = proj.hbc.functions()
            written = 0
            for f in funcs:
                try:
                    code = proj.hbc.decompile(f.index)
                except Exception as exc:
                    self.console.print(f"[red]func {f.index} ({f.name}) failed:[/] {exc}")
                    continue
                safe_name = "".join(c if c.isalnum() or c == "_" else "_" for c in f.name)
                out_path = os.path.join(output_dir, f"func_{f.index}_{safe_name}.js")
                with open(out_path, "w") as fh:
                    fh.write(code)
                written += 1
            self.console.print(
                f"[green]Decompiled[/] {written}/{len(funcs)} functions -> [bold]{output_dir}[/]"
            )
            return

        if args[0].startswith("module:"):
            mod       = self._resolve_module_arg(args[0])
            all_funcs = proj.hbc.functions()
            indices   = modules_view.module_functions(proj.hbc.call_graph(), mod)
            rows: list[str] = []
            for idx in indices:
                if rows:
                    rows.append("")
                rows.append(modules_view.module_function_separator(all_funcs[idx], mod))
                rows += display.decompiled_function_rows(proj.hbc, all_funcs[idx])
            title = f"decompile  module #{mod.module_id}  {len(indices)} functions"
            self._show_rows(rows, title, full_screen=True)
            return

        func  = self._resolve_func_arg(args[0])
        rows  = display.decompiled_function_rows(proj.hbc, func)
        title = f"decompile  func:{func.index} ({func.name})"
        self._show_rows(rows, title, full_screen=True)
