"""HermitREPL - interactive REPL for analyzing HBC files.

The REPL core (the prompt loop, command dispatch and the shared helpers every
command relies on) lives here. The commands themselves are one module each under
`cli/commands/`; `HermitREPL` inherits them as mixins, so a handler is still a
plain `cmd_<name>` method found by `getattr(self, "cmd_" + name)`.
"""

import os
import shlex
from typing import Optional

from prompt_toolkit import PromptSession
from prompt_toolkit.completion import NestedCompleter
from prompt_toolkit.history import FileHistory
from prompt_toolkit.styles import Style
from rich.console import Console

from cli.project import HermitProject
from cli.viewer import show_list

from cli.commands.open import OpenCommand
from cli.commands.info import InfoCommand
from cli.commands.strings import StringsCommand
from cli.commands.arrays import ArraysCommand
from cli.commands.functions import FunctionsCommand
from cli.commands.modules import ModulesCommand
from cli.commands.disasm import DisasmCommand
from cli.commands.src import SrcCommand
from cli.commands.cfg import CfgCommand
from cli.commands.xrefs import XrefsCommand
from cli.commands.env import EnvCommand
from cli.commands.decompile import DecompileCommand
from cli.commands.edit import EditCommand
from cli.commands.patch import PatchCommand


_COMMANDS = [
    "open", "info", "strings", "arrays", "functions", "modules",
    "disasm", "cfg", "src", "xrefs", "env", "decompile", "edit", "patch", "help", "exit", "quit",
]

_PROMPT_STYLE = Style.from_dict({"prompt": "bold cyan"})


class _SafeFileHistory(FileHistory):
    """FileHistory that silently stops saving when the file can't be written
    (no home directory, read-only filesystem, ...)."""

    def store_string(self, string: str) -> None:
        try:
            super().store_string(string)
        except OSError:
            pass


class HermitREPL(
    OpenCommand, InfoCommand, StringsCommand, ArraysCommand, FunctionsCommand,
    ModulesCommand, DisasmCommand, SrcCommand, CfgCommand, XrefsCommand,
    EnvCommand, DecompileCommand, EditCommand, PatchCommand,
):
    def __init__(self) -> None:
        self.project: Optional[HermitProject] = None
        self.console = Console()

        history_path = os.path.expanduser("~/.hermit_history")
        self.session: PromptSession = PromptSession(
            history=_SafeFileHistory(history_path),
            completer=NestedCompleter(
                {c: (NestedCompleter({k: None for k in (*self._INFO_VIEWS, "all")},
                                     ignore_case=True) if c == "info" else None)
                 for c in _COMMANDS},
                ignore_case=True),
            style=_PROMPT_STYLE,
        )

    # Main loop

    def run(self, initial_file: Optional[str] = None, permissive: bool = False) -> None:
        if initial_file:
            self._open_file(initial_file, permissive)

        self.console.print(
            "[bold cyan]hermit[/] - Hermes Reverse Engineering & Manipulation Interactive Toolkit\n"
            "Type [bold]help[/] for available commands.\n"
        )

        while True:
            try:
                raw = self.session.prompt(self._prompt_str())
            except KeyboardInterrupt:
                continue
            except EOFError:
                break

            line = raw.strip()
            if not line:
                continue

            try:
                self._dispatch(line)
            except SystemExit:
                break
            except Exception as exc:
                self.console.print(f"[red]Error:[/] {exc}")

    def _prompt_str(self) -> str:
        if self.project is None:
            return "[hermit] > "
        name = os.path.splitext(os.path.basename(self.project.hbc_path))[0]
        ver  = self.project.hbc.version()
        # + = bytecode patched in memory (not written)
        mark = "+" if self.project.is_patched else ""
        # ro = opened --permissive: read-only analysis of a malformed file
        ro = " ro" if self.project.is_permissive else ""
        return f"[hbc v{ver}{ro} | {name}{mark}] > "

    def _dispatch(self, line: str) -> None:
        try:
            parts = shlex.split(line)
        except ValueError as e:
            self.console.print(f"[red]Parse error:[/] {e}")
            return
        if not parts:
            return
        cmd, *args = parts
        handler = getattr(self, f"cmd_{cmd}", None)
        if handler is None:
            self.console.print(f"[red]Unknown command:[/] {cmd!r}  (type [bold]help[/])")
            return
        handler(args)

    # Internal helpers shared by the command mixins

    def _require_project(self) -> HermitProject:
        if self.project is None:
            raise RuntimeError("No file open. Use: open <file.hbc>")
        return self.project

    def _show_rows(
        self,
        rows: list[str],
        title: str,
        alt_rows: list[str] | None = None,
        alt_title: str = "",
        full_screen: bool = False,
    ) -> None:
        show_list(rows, title=title, alt_rows=alt_rows, alt_title=alt_title,
                  full_screen=full_screen)

    def _resolve_func_arg(self, arg: str):
        """'func:<N>' or 'func:<name>' -> FunctionInfo."""
        if not arg.startswith("func:"):
            raise ValueError(f"Expected func:<N> or func:<name>, got: {arg!r}")
        spec  = arg[len("func:"):]
        funcs = self._require_project().hbc.functions()

        if spec.isdigit():
            idx = int(spec)
            if idx >= len(funcs):
                raise ValueError(f"Function index {idx} out of range (0–{len(funcs)-1})")
            return funcs[idx]

        matches = [f for f in funcs if f.name == spec]
        if not matches:
            raise ValueError(f"No function named {spec!r}")
        return matches[0]

    def _resolve_module_arg(self, arg: str):
        """'module:<id>' -> MetroModule (Metro module id, from recover_modules())."""
        if not arg.startswith("module:"):
            raise ValueError(f"Expected module:<id>, got: {arg!r}")
        spec = arg[len("module:"):]
        if not spec.isdigit():
            raise ValueError(f"module:<id> requires a numeric module id, got: {spec!r}")
        mods = self._require_project().hbc.recover_modules()
        if not mods:
            raise ValueError("No Metro modules in this file (no __d(...) calls in the global function)")
        mod_id = int(spec)
        for m in mods:
            if m.module_id == mod_id:
                return m
        raise ValueError(f"No module #{mod_id} (ids: {min(m.module_id for m in mods)}–"
                         f"{max(m.module_id for m in mods)})")

    def _resolve_string_arg(self, arg: str) -> int:
        """'string:<idx>' or 'string:<value>' -> string index."""
        if not arg.startswith("string:"):
            raise ValueError(f"Expected string:<idx> or string:<value>, got: {arg!r}")
        spec    = arg[len("string:"):]
        strings = self._require_project().hbc.strings()

        if spec.isdigit():
            idx = int(spec)
            if idx >= len(strings):
                raise ValueError(f"String index {idx} out of range (0–{len(strings)-1})")
            return idx

        matches = [i for i, s in enumerate(strings) if s == spec]
        if not matches:
            raise ValueError(f"No string equal to {spec!r}")
        if len(matches) > 1:
            raise ValueError(
                f"{len(matches)} strings equal {spec!r} - use string:<idx> "
                f"(indices: {matches[:20]})"
            )
        return matches[0]

    def _resolve_array_arg(self, arg: str) -> int:
        """'array:<N>' -> literal site index."""
        if not arg.startswith("array:"):
            raise ValueError(f"Expected array:<N>, got: {arg!r}")
        spec  = arg[len("array:"):]
        sites = self._require_project().hbc.literal_sites()
        if not spec.isdigit():
            raise ValueError(f"array:<N> requires a numeric index, got: {spec!r}")
        idx = int(spec)
        if idx >= len(sites):
            raise ValueError(f"Array index {idx} out of range (0–{len(sites) - 1})")
        return idx

    @staticmethod
    def _parse_listing_args(args: list[str], kind_flags: dict[str, str]):
        """(filter_text, kind_filter, func_arg) from `filter:<text>`, one of the
        `kind_flags` and `func:<...>`."""
        filter_text = kind_filter = func_arg = None
        for arg in args:
            if arg.startswith("filter:"):
                filter_text = arg[len("filter:"):]
            elif arg in kind_flags:
                kind_filter = kind_flags[arg]
            elif arg.startswith("func:"):
                func_arg = arg
        return filter_text, kind_filter, func_arg

    # Meta commands

    def cmd_help(self, args: list[str]) -> None:
        """help  - shows the available commands"""
        lines = []
        for cmd in _COMMANDS:
            handler = getattr(self, f"cmd_{cmd}", None)
            doc = (handler.__doc__ or "").strip().split("\n")[0] if handler else ""
            lines.append(f"  [bold cyan]{cmd:<12}[/] {doc}")
        self.console.print("\n".join(lines))

    def cmd_exit(self, args: list[str]) -> None:
        """exit  - exits the REPL"""
        raise SystemExit(0)

    def cmd_quit(self, args: list[str]) -> None:
        """quit  - exits the REPL"""
        self.cmd_exit(args)
