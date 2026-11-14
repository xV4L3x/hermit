"""`functions` command: lists all functions, optionally one Metro module."""

from cli import display
from cli import modules_view


class FunctionsCommand:
    def cmd_functions(self, args: list[str]) -> None:
        """functions [--module=<id>]  - lists all functions with parameters and flags (--module: only the functions of one Metro module)"""
        module_arg = None
        rest = list(args)
        while rest:
            a = rest.pop(0)
            if a.startswith("--module="):
                module_arg = a.split("=", 1)[1]
            elif a == "--module" and rest:
                module_arg = rest.pop(0)
            else:
                self.console.print("Usage: functions [--module=<id>]")
                return

        proj        = self._require_project()
        funcs       = proj.hbc.functions()
        global_idx  = proj.hbc.header().global_code_index

        module = None
        cg = None  # fetched once; crossing pybind copies the whole call graph
        if module_arg is not None:
            module = self._resolve_module_arg(f"module:{module_arg}")
            cg     = proj.hbc.call_graph()
            funcs  = [funcs[i] for i in modules_view.module_functions(cg, module)]

        # Every function of a Metro module is tagged `# module N` (the factory
        # `# module N entry`). Analysis failures on a malformed file only drop
        # the tags.
        mods: list = []
        func_modules: dict = {}
        try:
            mods = proj.hbc.recover_modules()
            if mods:
                if cg is None:
                    cg = proj.hbc.call_graph()
                func_modules = modules_view.function_modules(cg, mods)
        except Exception:
            pass

        # header.cjsModuleCount > 0 only for a file compiled with Hermes's
        # native `-commonjs` mode -- never both this and recover_modules()
        # (Metro) on the same file in practice, see doc/hbc-format/v96-format.md §9.
        cjs_module_paths: dict[int, str] = {}
        try:
            for cm in proj.hbc.cjs_modules():
                cjs_module_paths[cm.function_index] = cm.module_path
        except Exception:
            pass

        rows  = display.functions_to_ansi_rows(funcs, global_idx, func_modules, cjs_module_paths)
        title = f"functions ({len(rows)})"
        if module is not None:
            title += (f"  module #{module.module_id} (factory [{module.function_index}]"
                      " + nested functions)")
        elif mods:
            title += f"  {len(mods)} modules"
        if cjs_module_paths:
            title += f"  {len(cjs_module_paths)} CJS modules"
        title += "  * = global entry"
        self._show_rows(rows, title)
