"""`info` command: header summary and per-section tables."""

from cli import display


class InfoCommand:
    _INFO_VIEWS = {
        "sections":  ("Sections",  display.make_info_sections_table),
        "strings":   ("Strings",   display.make_info_strings_table),
        "functions": ("Functions", display.make_info_functions_table),
    }

    def cmd_info(self, args: list[str]) -> None:
        """info [sections|strings|functions|all]  - header summary and layout statistics"""
        proj = self._require_project()
        if not args:
            self.console.print(display.make_info_table(proj.hbc))
            return
        if len(args) > 1 or (args[0] != "all" and args[0] not in self._INFO_VIEWS):
            self.console.print(
                "Usage: info \\[sections|strings|functions|all]", style="red")
            return
        if args[0] == "all":
            self.console.print(display.make_info_table(proj.hbc))
            names = list(self._INFO_VIEWS)
        else:
            names = [args[0]]
        for name in names:
            title, make = self._INFO_VIEWS[name]
            self.console.print(f"\n[bold cyan]{title}[/bold cyan]")
            self.console.print(make(proj.hbc))
