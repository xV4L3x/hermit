"""`src` command: the debug source table."""

from cli import display


class SrcCommand:
    def cmd_src(self, args: list[str]) -> None:
        """src [func:<N|name>]  - debug source table (all functions or just one)"""
        proj = self._require_project()
        if not args:
            rows  = display.all_debug_source_table_rows(proj.hbc)
            title = "debug source table  (all functions)"
            self._show_rows(rows, title)
            return
        if len(args) == 1 and args[0].startswith("func:"):
            func     = self._resolve_func_arg(args[0])
            src_locs = proj.hbc.source_locations(func.index) if func.hasDebugInfo else []
            src_files = proj.hbc.source_files()
            rows      = display.debug_source_table_rows(func, src_locs, src_files)
            title     = f"src table  func:{func.index} ({func.name})"
            self._show_rows(rows, title)
            return
        self.console.print("Usage: src  |  src func:<N|name>")
