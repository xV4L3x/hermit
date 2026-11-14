"""`modules` command: Metro modules, their functions and dependencies."""

from cli import modules_view


class ModulesCommand:
    def cmd_modules(self, args: list[str]) -> None:
        """modules [module:<id>]  - Metro modules: functions implemented and dependencies (who includes a module: xrefs module:<id>)"""
        proj = self._require_project()
        if len(args) > 1 or (args and not args[0].startswith("module:")):
            self.console.print("Usage: modules  |  modules module:<id>")
            return
        mods    = proj.hbc.recover_modules()
        funcs   = proj.hbc.functions()
        entries = proj.hbc.metro_entry_modules()
        if not mods:
            self.console.print("  [dim]No Metro modules: no __d(...) calls in the global function[/]")
            return
        counts = modules_view.module_function_counts(proj.hbc.call_graph(), mods)
        if args:
            mod   = self._resolve_module_arg(args[0])
            rows  = modules_view.module_detail_rows(mod, mods, funcs, entries,
                                               counts[mod.module_id],
                                               proj.hbc.module_exports(mod.module_id))
            title = (f"module #{mod.module_id}  [{mod.function_index}] "
                     f"{funcs[mod.function_index].name}")
            self._show_rows(rows, title)
            return
        rows  = modules_view.modules_to_ansi_rows(mods, funcs, entries, counts)
        edges = sum(len(m.dependencies) for m in mods)
        title = f"modules ({len(mods)})  {edges} dependencies  entries (__r): {entries}"
        self._show_rows(rows, title)
