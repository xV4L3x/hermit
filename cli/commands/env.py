"""`env` command: closure environments of a function."""

from cli import display


class EnvCommand:
    def cmd_env(self, args: list[str]) -> None:
        """env func:<N|name>  - closure environments: the slots the function's environment holds (debug name, provable value, who stores/loads them), the chain it captured, its own environment accesses"""
        if len(args) != 1 or not args[0].startswith("func:"):
            self.console.print("Usage: env func:<N|name>")
            return
        proj  = self._require_project()
        func  = self._resolve_func_arg(args[0])
        funcs = proj.hbc.functions()
        cg    = proj.hbc.call_graph()
        mod   = None
        mod_id = proj.hbc.function_module(func.index)
        if mod_id is not None:
            factory = next((m.function_index for m in proj.hbc.recover_modules()
                            if m.module_id == mod_id), None)
            mod = (mod_id, factory == func.index)
        with self.console.status("analysing environments..."):
            rows = display.env_view_rows(proj.hbc, func, funcs, cg, mod)
        self._show_rows(rows, f"env  func:{func.index} ({func.name})")
