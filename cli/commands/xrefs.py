"""`xrefs` command: cross-references of strings, functions and Metro modules."""

from cli import display
from cli import modules_view


class XrefsCommand:
    def cmd_xrefs(self, args: list[str]) -> None:
        """xrefs [string:<value|N>] [func:<N|name>|module:<id> [--callees]]  (callers by default, --callees for callees; string refs include array/object literal elements; module: callers = modules that include it, callees = modules it includes)"""
        proj = self._require_project()

        if not args:
            all_xrefs = proj.hbc.string_xrefs()
            rows  = display.xrefs_summary_to_ansi_rows(all_xrefs)
            title = f"xrefs  {len(rows)} referenced strings"
            self._show_rows(rows, title)
            return

        # --- module: branch ---
        module_arg = next((a for a in args if a.startswith("module:")), None)
        if module_arg is not None:
            mod   = self._resolve_module_arg(module_arg)
            mods  = proj.hbc.recover_modules()
            funcs = proj.hbc.functions()
            show_callees = "--callees" in args
            show_callers = not show_callees
            if show_callers:
                rows  = modules_view.module_callers_rows(mod, mods, funcs)
                title = (f"xrefs  module #{mod.module_id}  callers ({len(rows)})"
                         "  modules that include it")
                if rows:
                    self._show_rows(rows, title)
                else:
                    self.console.print(f"  [dim]module #{mod.module_id}: no module includes it[/]")
            if show_callees:
                rows  = modules_view.module_callees_rows(mod, mods, funcs)
                title = (f"xrefs  module #{mod.module_id}  callees ({len(mod.dependencies)})"
                         "  modules it includes")
                if rows:
                    self._show_rows(rows, title)
                else:
                    self.console.print(f"  [dim]module #{mod.module_id}: includes no module[/]")
            return

        # --- func: branch ---
        func_arg = next((a for a in args if a.startswith("func:")), None)
        if func_arg is not None:
            func         = self._resolve_func_arg(func_arg)
            show_callees = "--callees" in args
            show_callers = not show_callees

            cg    = proj.hbc.call_graph()
            funcs = proj.hbc.functions()

            if show_callers:
                with self.console.status("resolving calls in the importing modules..."):
                    rows = display.function_callers_rows(proj.hbc, func, funcs, cg)
                title = f"xrefs  func:{func.index} ({func.name})  callers"
                self._show_rows(rows, title)

            if show_callees:
                with self.console.status("resolving calls..."):
                    rows = display.function_callees_rows(proj.hbc, func, funcs, cg)
                title = f"xrefs  func:{func.index} ({func.name})  callees ({len(rows)})"
                self._show_rows(rows, title)
            return

        # --- string: branch ---
        spec = next((a[len("string:"):] for a in args if a.startswith("string:")), None)
        if spec is None:
            self.console.print("Usage: xrefs [string:<value|N>] [func:<N|name>|module:<id> [--callees]]")
            return

        all_xrefs = proj.hbc.string_xrefs()
        if spec.isdigit():
            target = int(spec)
            xrefs  = [x for x in all_xrefs if x.string_index == target]
            if not xrefs:
                strs = proj.hbc.strings()
                if target < len(strs):
                    self.console.print(f"  [dim]string [{target}] {strs[target]!r} - no references[/]")
                else:
                    self.console.print(f"[red]String index {target} out of range (0–{len(strs)-1})[/]")
                return
            title = f"xrefs  string:{target}  {sum(len(x.sites) for x in xrefs)} sites"
        else:
            xrefs = [x for x in all_xrefs if x.string_value == spec]
            if not xrefs:
                self.console.print(f"  [dim]No references to string {spec!r}[/]")
                return
            title = f"xrefs  string:{spec!r}  {sum(len(x.sites) for x in xrefs)} sites"

        funcs = proj.hbc.functions()
        rows  = display.xrefs_detail_to_ansi_rows(xrefs, funcs)
        self._show_rows(rows, title)
