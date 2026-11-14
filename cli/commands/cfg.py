"""`cfg` command: control-flow graph of a function or a Metro module."""

from cli import cfg_dot


class CfgCommand:
    def cmd_cfg(self, args: list[str]) -> None:
        """cfg func:<N|name> | cfg module:<id> [--compact=true|false]  - control flow graph, or a Metro module and its direct dependencies"""
        force_compact: bool | None = None
        positional = []
        for a in args:
            if a.startswith("--compact="):
                val = a.split("=", 1)[1].lower()
                if val in ("false", "0", "no"):
                    force_compact = False
                elif val in ("true", "1", "yes"):
                    force_compact = True
                else:
                    self.console.print(f"[red]Unknown --compact value: {val!r}[/red]")
                    return
            else:
                positional.append(a)

        if len(positional) != 1:
            self.console.print("Usage: cfg func:<N|name> | cfg module:<id> [--compact=true|false]")
            return
        proj     = self._require_project()
        if positional[0].startswith("module:"):
            mod = self._resolve_module_arg(positional[0])
            cfg_dot.show_module_graph(mod, proj.hbc.recover_modules(), proj.hbc.functions(),
                                      proj.hbc.metro_entry_modules(),
                                      force_compact=force_compact)
            return
        func     = self._resolve_func_arg(positional[0])
        cfg_data = proj.hbc.cfg(func.index)
        cfg_dot.show_cfg(cfg_data, func, force_compact=force_compact)
