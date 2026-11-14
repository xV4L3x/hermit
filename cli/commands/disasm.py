"""`disasm` command: disassembles a function or a whole Metro module."""

from cli import display
from cli import modules_view
from cli.literal_preview import LiteralPreview


class DisasmCommand:
    def cmd_disasm(self, args: list[str]) -> None:
        """disasm func:<N|name> | disasm module:<id>  - disassembles a function, or every function of a Metro module in sequence (s = debug source table)"""
        if len(args) != 1:
            self.console.print("Usage: disasm func:<N|name>  |  disasm module:<id>")
            return
        proj        = self._require_project()
        if args[0].startswith("module:"):
            self._disasm_module(args[0])
            return
        func        = self._resolve_func_arg(args[0])
        instrs      = proj.hbc.disasm(func.index)
        src_locs    = proj.hbc.source_locations(func.index) if func.hasDebugInfo else []
        src_files   = proj.hbc.source_files()

        with self.console.status("analysing environments and calls..."):
            notes = display.disasm_annotations(proj.hbc, func.index, proj.hbc.functions())
        rows     = display.disasm_to_ansi_rows(func, instrs, src_locs, LiteralPreview(proj.hbc),
                                               notes)
        alt_rows = display.debug_source_table_rows(func, src_locs, src_files)
        title    = f"disasm  func:{func.index} ({func.name})  ({len(instrs)} instrs)"
        alt_title = f"src table  func:{func.index} ({func.name})"
        self._show_rows(rows, title, alt_rows=alt_rows, alt_title=alt_title,
                        full_screen=True)

    def _disasm_module(self, arg: str) -> None:
        """`disasm module:<id>`: the module's functions (factory first, then the
        nested ones, by index), each under a `[N] name  # module X` separator;
        the `s` view concatenates their debug source tables."""
        proj      = self._require_project()
        mod       = self._resolve_module_arg(arg)
        all_funcs = proj.hbc.functions()
        indices   = modules_view.module_functions(proj.hbc.call_graph(), mod)
        src_files = proj.hbc.source_files()
        preview   = LiteralPreview(proj.hbc)
        namer     = display.SlotNamer(proj.hbc, all_funcs)

        rows: list[str] = []
        alt_rows: list[str] = []
        n_instrs = 0
        for idx in indices:
            func     = all_funcs[idx]
            instrs   = proj.hbc.disasm(idx)
            src_locs = proj.hbc.source_locations(idx) if func.hasDebugInfo else []
            n_instrs += len(instrs)
            sep = modules_view.module_function_separator(func, mod)
            if rows:
                rows.append("")
            with self.console.status("analysing environments and calls..."):
                notes = display.disasm_annotations(proj.hbc, idx, all_funcs, namer)
            rows += [sep] + display.disasm_to_ansi_rows(func, instrs, src_locs, preview, notes)
            if alt_rows:
                alt_rows.append("")
            alt_rows += display.debug_source_table_rows(func, src_locs, src_files)

        title = (f"disasm  module #{mod.module_id}  {len(indices)} functions"
                 f"  ({n_instrs} instrs)")
        self._show_rows(rows, title, alt_rows=alt_rows,
                        alt_title=f"src table  module #{mod.module_id}", full_screen=True)
