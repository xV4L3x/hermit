"""HermitREPL - interactive REPL for analyzing HBC files."""

import os
import shlex
from typing import Optional

from prompt_toolkit import PromptSession
from prompt_toolkit.completion import WordCompleter
from prompt_toolkit.history import FileHistory
from prompt_toolkit.styles import Style
from rich.console import Console
from rich.syntax import Syntax

import _libhbc
from cli.project import HermitProject
from cli import display
from cli import cfg_dot
from cli.viewer import show_list


_COMMANDS = [
    "open", "info", "strings", "arrays", "functions",
    "disasm", "cfg", "src", "xrefs", "decompile", "edit", "patch", "help", "exit", "quit",
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


def _strip_banner_comments(code: str) -> str:
    """Drops decompile()'s leading `//` banner (func / paramCount / module):
    useful in the .js files of `decompile all`, noise in the interactive view."""
    lines = code.split("\n")
    i = 0
    while i < len(lines) and lines[i].startswith("//"):
        i += 1
    return "\n".join(lines[i:])


class HermitREPL:
    def __init__(self) -> None:
        self.project: Optional[HermitProject] = None
        self.console = Console()

        history_path = os.path.expanduser("~/.hermit_history")
        self.session: PromptSession = PromptSession(
            history=_SafeFileHistory(history_path),
            completer=WordCompleter(_COMMANDS, ignore_case=True),
            style=_PROMPT_STYLE,
        )

    # Main loop

    def run(self, initial_file: Optional[str] = None) -> None:
        if initial_file:
            self._open_file(initial_file)

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
        return f"[hbc v{ver} | {name}{mark}] > "

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

    # Internal helpers

    def _require_project(self) -> HermitProject:
        if self.project is None:
            raise RuntimeError("No file open. Use: open <file.hbc>")
        return self.project

    def _open_file(self, path: str) -> None:
        path = os.path.expanduser(os.path.abspath(path))
        if not os.path.exists(path):
            raise FileNotFoundError(f"File not found: {path}")

        ext = os.path.splitext(path)[1].lower()
        if ext != ".hbc":
            raise ValueError(f"Unknown extension {ext!r} - expected .hbc")
        proj = HermitProject.from_hbc(path)

        self.project = proj
        name = os.path.splitext(os.path.basename(proj.hbc_path))[0]
        ver  = proj.hbc.version()
        hdr  = proj.hbc.header()

        self.console.print(
            f"[green]Opened[/] [bold]{name}[/] "
            f"(HBC v{ver}, {hdr.function_count} functions, {hdr.string_count} strings)"
        )

    def _show_rows(
        self,
        rows: list[str],
        title: str,
        alt_rows: list[str] | None = None,
        alt_title: str = "",
    ) -> None:
        show_list(rows, title=title, alt_rows=alt_rows, alt_title=alt_title)

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

    # Commands

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

    def cmd_open(self, args: list[str]) -> None:
        """open <file.hbc>  - opens an HBC file"""
        if len(args) != 1:
            self.console.print("Usage: open <file.hbc>")
            return
        self._open_file(args[0])

    def cmd_info(self, args: list[str]) -> None:
        """info  - shows the HBC header summary"""
        proj = self._require_project()
        self.console.print(display.make_info_table(proj.hbc))

    def cmd_strings(self, args: list[str]) -> None:
        """strings [--identifiers|--strings] [filter:<text>]  - lists strings with kind"""
        proj = self._require_project()
        filter_text, kind_filter, _ = self._parse_listing_args(
            args, {"--identifiers": "identifier", "--strings": "string"})

        strings = proj.hbc.strings()
        kinds   = proj.hbc.string_kinds()
        rows    = display.strings_to_ansi_rows(strings, kinds, filter_text, kind_filter)
        title   = f"strings  {len(rows)}/{len(strings)}"
        if kind_filter:
            title += f"  [{kind_filter}s only]"
        if filter_text:
            title += f"  filter={filter_text!r}"
        self._show_rows(rows, title)

    def cmd_arrays(self, args: list[str]) -> None:
        """arrays [--arrays|--objects] [filter:<text>] [func:<N|name>]  - lists array/object literal sites"""
        proj = self._require_project()
        filter_text, kind_filter, func_arg = self._parse_listing_args(
            args, {"--arrays": "array", "--objects": "object"})

        func_filter = None
        if func_arg is not None:
            func_filter = self._resolve_func_arg(func_arg).index

        sites = proj.hbc.literal_sites()
        rows  = display.arrays_to_ansi_rows(
            sites, proj.hbc.array_buffer(), proj.hbc.obj_key_buffer(),
            proj.hbc.obj_value_buffer(), proj.hbc.strings(), proj.hbc.functions(),
            filter_text, kind_filter, func_filter,
        )
        title = f"arrays  {len(rows)}/{len(sites)}"
        if kind_filter:
            title += f"  [{kind_filter}s only]"
        if func_filter is not None:
            title += f"  func:{func_filter}"
        if filter_text:
            title += f"  filter={filter_text!r}"
        self._show_rows(rows, title)

    def cmd_functions(self, args: list[str]) -> None:
        """functions  - lists all functions with parameters and flags"""
        proj        = self._require_project()
        funcs       = proj.hbc.functions()
        global_idx  = proj.hbc.header().global_code_index

        module_ids: dict[int, int] = {}
        try:
            for mi in proj.hbc.recover_modules():
                module_ids[mi.function_index] = mi.module_id
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

        rows  = display.functions_to_ansi_rows(funcs, global_idx, module_ids, cjs_module_paths)
        title = f"functions ({len(rows)})"
        if module_ids:
            title += f"  {len(module_ids)} modules"
        if cjs_module_paths:
            title += f"  {len(cjs_module_paths)} CJS modules"
        title += "  * = global entry"
        self._show_rows(rows, title)

    def cmd_disasm(self, args: list[str]) -> None:
        """disasm func:<N|name>  - disassembles a function (s = debug source table)"""
        if len(args) != 1:
            self.console.print("Usage: disasm func:<N|name>")
            return
        proj        = self._require_project()
        func        = self._resolve_func_arg(args[0])
        instrs      = proj.hbc.disasm(func.index)
        src_locs    = proj.hbc.source_locations(func.index) if func.hasDebugInfo else []
        src_files   = proj.hbc.source_files()

        rows     = display.disasm_to_ansi_rows(func, instrs, src_locs)
        alt_rows = display.debug_source_table_rows(func, src_locs, src_files)
        title    = f"disasm  [{func.index}] {func.name}  ({len(instrs)} instrs)"
        alt_title = f"src table  [{func.index}] {func.name}"
        self._show_rows(rows, title, alt_rows=alt_rows, alt_title=alt_title)

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
            title     = f"src table  [{func.index}] {func.name}"
            self._show_rows(rows, title)
            return
        self.console.print("Usage: src  |  src func:<N|name>")

    def cmd_cfg(self, args: list[str]) -> None:
        """cfg func:<N|name> [--compact=true|false]  - shows the control flow graph"""
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
            self.console.print("Usage: cfg func:<N|name> [--compact=true|false]")
            return
        proj     = self._require_project()
        func     = self._resolve_func_arg(positional[0])
        cfg_data = proj.hbc.cfg(func.index)
        cfg_dot.show_cfg(cfg_data, func, force_compact=force_compact)

    def cmd_xrefs(self, args: list[str]) -> None:
        """xrefs [string:<value|N>] [func:<N|name> [--callers] [--callees]]"""
        proj = self._require_project()

        if not args:
            all_xrefs = proj.hbc.string_xrefs()
            rows  = display.xrefs_summary_to_ansi_rows(all_xrefs)
            title = f"xrefs  {len(rows)} referenced strings"
            self._show_rows(rows, title)
            return

        # --- func: branch ---
        func_arg = next((a for a in args if a.startswith("func:")), None)
        if func_arg is not None:
            func         = self._resolve_func_arg(func_arg)
            show_callers = "--callers" in args
            show_callees = "--callees" in args
            if not show_callers and not show_callees:
                show_callers = show_callees = True

            cg    = proj.hbc.call_graph()
            funcs = proj.hbc.functions()

            if show_callers:
                callers = [cs for cs in cg if cs.is_resolved and cs.callee_index == func.index]
                rows    = display.call_graph_to_ansi_rows(callers, funcs, mode="callers")
                title   = f"xrefs  [{func.index}] {func.name}  callers ({len(callers)})"
                self._show_rows(rows, title)

            if show_callees:
                callees = [cs for cs in cg if cs.caller_index == func.index]
                rows    = display.call_graph_to_ansi_rows(callees, funcs, mode="callees")
                title   = f"xrefs  [{func.index}] {func.name}  callees ({len(callees)})"
                self._show_rows(rows, title)
            return

        # --- string: branch ---
        spec = next((a[len("string:"):] for a in args if a.startswith("string:")), None)
        if spec is None:
            self.console.print("Usage: xrefs [string:<value|N>] [func:<N|name> [--callers] [--callees]]")
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

    def cmd_decompile(self, args: list[str]) -> None:
        """decompile func:<N|name> | decompile all [--output=<dir>]  - decompiles to pseudo-JS"""
        proj = self._require_project()
        if not args:
            self.console.print("Usage: decompile func:<N|name>  |  decompile all [--output=<dir>]")
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

        func = self._resolve_func_arg(args[0])
        try:
            code = proj.hbc.decompile(func.index)
        except Exception as exc:
            self.console.print(f"[red]Decompile failed:[/] {exc}")
            return

        code = _strip_banner_comments(code)

        # "show source" (Function Source Table): a standalone alert, not a `//`
        # comment inside the highlighted body.
        if func.hasPreservedSource and not func.sourceIsHidden:
            self.console.print("[bold yellow]ORIGINAL SOURCE - NOT decompiled[/]")
            self.console.print(Syntax(func.preservedSource, "javascript", theme="ansi_dark", line_numbers=False))
            return

        self.console.print(Syntax(code, "javascript", theme="ansi_dark", line_numbers=False))

    def cmd_edit(self, args: list[str]) -> None:
        """edit func:<N|name|new> | edit string:<idx|value|new> | edit array:<N>  - interactive editor (Ctrl+X -> Y applies in memory; then `patch`)"""
        if len(args) != 1:
            self.console.print(
                "Usage: edit func:<N|name>  |  edit func:new  |  "
                "edit string:<idx|value>  |  edit string:new  |  edit array:<N>")
            return
        proj = self._require_project()
        arg  = args[0]

        if arg.startswith("string:"):
            self._edit_string(proj, arg)
        elif arg.startswith("array:"):
            self._edit_array(proj, arg)
        elif arg == "func:new":
            self._edit_func_new(proj)
        else:
            self._edit_func(proj, arg)

    def _edit_func(self, proj: HermitProject, arg: str) -> None:
        func   = self._resolve_func_arg(arg)
        instrs = proj.hbc.disasm(func.index)
        from cli.editor import (run_editor, encode_committed_lines,
                                assemble_committed_function)
        state = run_editor(
            func, instrs,
            strings=proj.hbc.strings(),
            opcodes=_libhbc.opcode_table(proj.hbc.version()),
            version=proj.hbc.version(),
        )
        if not getattr(state, "commit", False):
            return

        # Structural path: the buffer changed shape, or the function has shared
        # bytecode (which goes through Module for the editAll logic).
        if state.structural_change or getattr(state, "shared", False):
            if getattr(state, "shared", False) and not getattr(state, "edit_all", False):
                self.console.print(
                    "[yellow]Edit not applied:[/] the function has shared "
                    "bytecode and you did not choose «apply to all»."
                )
                return
            try:
                fidx, asm, frame, warnings, edit_all = assemble_committed_function(state)
                warnings = list(warnings) + list(
                    proj.apply_insertion(fidx, asm, frame, edit_all=edit_all))
            except Exception as exc:
                self.console.print(f"[red]Structural commit rejected:[/] {exc}")
                return
            for w in warnings:
                self.console.print(f"[yellow]note:[/] {w}")
            n_old = getattr(state, "_orig_nav_count", len(asm))
            self.console.print(
                f"[green]body of func {fidx} reassembled[/] "
                f"({len(asm)} instructions, was {n_old}) - [bold]patch[/] to write the .hbc"
            )
            return

        # Same-size path.
        try:
            patches = encode_committed_lines(state)
        except Exception as exc:
            self.console.print(f"[red]Commit cancelled:[/] {exc}")
            return
        if not patches:
            self.console.print("[dim]No lines changed.[/]")
            return
        try:
            proj.apply_patch(func.index, patches)
        except Exception as exc:
            self.console.print(f"[red]Patch rejected (integrity check):[/] {exc}")
            return
        n = len(patches)
        self.console.print(
            f"[green]{n} instruction{'s' if n != 1 else ''} patched "
            f"in memory[/] (func {func.index}) - [bold]patch[/] to write the .hbc"
        )

    def _edit_func_new(self, proj: HermitProject) -> None:
        """edit func:new: the usual instruction editor on a seed buffer
        (`LoadConstUndefined r0` / `Ret r0`) to build a NEW function. Fixed
        metadata: paramCount 1, frameSize from the highest register used, empty
        name, strict. To use it, insert a `CreateClosure rDst, rEnv, <idx>` in
        another function (`edit func:<other>`, `o`/`O`)."""
        from cli.editor import run_editor, new_function_stub, new_function_seed_instrs

        opcodes = _libhbc.opcode_table(proj.hbc.version())
        state = run_editor(
            new_function_stub(), new_function_seed_instrs(opcodes),
            strings=proj.hbc.strings(),
            opcodes=opcodes,
            version=proj.hbc.version(),
        )
        if not getattr(state, "commit", False):
            return
        self._finish_add_function(proj, state)

    def _finish_add_function(self, proj: HermitProject, state) -> None:
        """Assembles a committed `edit func:new` buffer and appends it. Separate
        from `_edit_func_new` so tests can drive it without `run_editor`."""
        from cli.editor import assemble_committed_function
        try:
            _fidx, asm, frame, warnings, _edit_all = assemble_committed_function(state)
            idx = proj.apply_add_function(asm, frame)
        except Exception as exc:
            self.console.print(f"[red]Commit rejected:[/] {exc}")
            return
        for w in warnings:
            self.console.print(f"[yellow]note:[/] {w}")
        self.console.print(
            f"[green]new function [{idx}][/] created in memory "
            f"({len(asm)} instructions) - reference it with "
            f"[bold]CreateClosure rDst, rEnv, {idx}[/] in edit func:<other>, "
            f"then [bold]patch[/]"
        )

    def _edit_string(self, proj: HermitProject, arg: str) -> None:
        """edit string:<idx|value>. Only Identifiers are refused (they need
        Hermes's hash). Empty and shared entries open: a shared one shows a
        banner and, if edited, detaches from its group. `string:new` opens the
        editor in "new string" mode."""
        from cli.editor import run_string_editor, encode_string_relocation_commit, _StrEditError

        spec = arg[len("string:"):] if arg.startswith("string:") else arg
        if spec == "new":
            state = run_string_editor(
                0, proj.hbc.strings(), proj.hbc.string_kinds(),
                proj.hbc.string_entries(), (), (), add=True,
            )
            if getattr(state, "commit", False):
                self._finish_add_string(proj, state)
            return

        idx     = self._resolve_string_arg(arg)
        entries = proj.hbc.string_entries()
        kinds   = proj.hbc.string_kinds()
        strings = proj.hbc.strings()
        e       = entries[idx]

        if kinds[idx] == _libhbc.StringKind.Identifier:
            self.console.print(
                f"[red]string:{idx} is an Identifier[/] - overwriting it desyncs "
                "the Identifier Hash Table (editing Identifiers is not yet supported)."
            )
            return

        # Entries sharing bytes with this one (informational). An empty entry
        # shares nothing: without the guard, a0 == a1 inside another entry's
        # range would be a false positive.
        a0, a1 = e.storage_offset, e.storage_offset + e.byte_length
        shared_with = [
            j for j, other in enumerate(entries)
            if j != idx and other.byte_length > 0
            and a0 < other.storage_offset + other.byte_length
            and other.storage_offset < a1
        ] if e.byte_length > 0 else []

        xrefs = [x for x in proj.hbc.string_xrefs() if x.string_index == idx]
        funcs = proj.hbc.functions()
        off   = e.storage_offset
        img   = proj.image_bytes
        lo    = max(0, off - 16)
        ctx   = img[lo:off + e.byte_length + 16]

        state = run_string_editor(
            idx, strings, kinds, entries, xrefs, funcs,
            context_bytes=ctx, context_base=lo,
            is_shared=bool(shared_with), shared_with=shared_with,
        )
        if not getattr(state, "commit", False):
            return

        # `Ctrl+N` inside the session -> the user switched to "new string".
        if getattr(state, "is_add", False):
            self._finish_add_string(proj, state)
            return

        try:
            enc = encode_string_relocation_commit(state)
        except _StrEditError as exc:
            self.console.print(f"[red]Commit cancelled:[/] {exc}")
            return
        if enc is None:
            self.console.print("[dim]No changes.[/]")
            return

        # A shared entry always detaches, even at unchanged length: overwriting
        # it in place would corrupt the others.
        same_length = len(enc) == e.byte_length
        try:
            if same_length and not shared_with:
                proj.apply_string_patch(idx, enc, state.value)
                self.console.print(
                    f"[green]string {idx} rewritten in memory[/] "
                    f"({e.byte_length} B unchanged) - [bold]patch[/] to write the .hbc"
                )
            else:
                proj.apply_string_relocation(idx, enc, state.value)
                verb = ("detached from the shared group and rebuilt" if shared_with
                        else "rebuilt")
                self.console.print(
                    f"[green]string {idx} {verb} in memory[/] "
                    f"({e.byte_length}->{len(enc)} B) - [bold]patch[/] to write the .hbc"
                )
        except Exception as exc:
            self.console.print(f"[red]Patch rejected (integrity check):[/] {exc}")

    def _finish_add_string(self, proj: HermitProject, state) -> None:
        """Commit of `edit string:new` / Ctrl+N."""
        if not state.value:
            self.console.print("[dim]No string added.[/]")
            return
        try:
            idx = proj.apply_add_string(state.value)
        except Exception as exc:
            self.console.print(f"[red]add_string rejected:[/] {exc}")
            return
        self.console.print(
            f"[green]new string [{idx}] {state.value!r}[/] added in memory - "
            f"use it in a LoadConstString with [bold]edit func:[/], then [bold]patch[/]"
        )

    def _edit_array(self, proj: HermitProject, arg: str) -> None:
        """edit array:<N>: editor for an array/object literal site. Refused only
        if the site's function has shared bytecode (the operands live inside the
        instruction, so an edit would touch the whole group). A buffer shared
        with another site opens with a banner and the commit detaches via
        relocation. An empty site opens too: `a` adds the first element."""
        from cli.editor import run_array_editor
        from cli.literals import SLPError, decode_slp, encode_slp_sequence, reencode_value_bytes

        idx     = self._resolve_array_arg(arg)
        sites   = proj.hbc.literal_sites()
        site    = sites[idx]
        strings = proj.hbc.strings()
        funcs   = proj.hbc.functions()
        image   = proj.image_bytes

        if funcs[site.function_index].shared:
            self.console.print(
                f"[red]array:{idx} is in a function with shared bytecode[/] "
                f"(func[{site.function_index}], ×{funcs[site.function_index].shareCount}) - "
                "editing its bufferIdx operand would touch every function in the group "
                "(private clone not supported)."
            )
            return

        try:
            if site.is_object:
                key_off = proj.hbc.obj_key_buffer_offset()
                val_off = proj.hbc.obj_value_buffer_offset()
                keys = decode_slp(image, key_off + site.key_buffer_idx, site.count)
                vals = decode_slp(image, val_off + site.buffer_idx, site.count)
            else:
                arr_off = proj.hbc.array_buffer_offset()
                vals = decode_slp(image, arr_off + site.buffer_idx, site.count)
                keys = None
        except SLPError as exc:
            self.console.print(f"[red]Site decode failed:[/] {exc}")
            return

        shared_with = proj.sites_sharing_buffer(idx)

        if vals:
            lo  = max(0, vals[0].byte_off - 16)
            hi  = vals[-1].byte_off + vals[-1].byte_len + 16
            ctx = image[lo:hi]
        else:
            lo, ctx = 0, b""

        state = run_array_editor(idx, site, vals, keys, strings, funcs,
                                 context_bytes=ctx, context_base=lo,
                                 is_shared=bool(shared_with), shared_with=shared_with)
        if not getattr(state, "commit", False):
            return

        # add/remove sessions are never combined with element edits.
        if state.is_resize:
            new_val_buf = encode_slp_sequence(
                [(v.tag, reencode_value_bytes(v.tag, v.value)) for v in state.values])
            new_key_buf = encode_slp_sequence(
                [(k.tag, reencode_value_bytes(k.tag, k.value)) for k in state.keys]
            ) if site.is_object else b""
            try:
                proj.apply_array_resize(
                    site.function_index, site.instruction_offset, site.is_object,
                    state.pending_add, new_val_buf, new_key_buf, idx)
            except Exception as exc:
                self.console.print(f"[red]Patch rejected (integrity check):[/] {exc}")
                return
            verb = "added" if state.pending_add else "removed"
            what = "key-value pair" if site.is_object else "element"
            self.console.print(
                f"[green]1 {what} {verb} in memory[/] (array:{idx}) - "
                f"[bold]patch[/] to write the .hbc"
            )
            return

        n = len(state.changed_indices)
        if n == 0:
            self.console.print("[dim]No changes.[/]")
            return

        buffer_name = "objvalue" if site.is_object else "array"
        try:
            if not state.needs_relocation:
                for abs_off, enc in state.patches.items():
                    proj.apply_array_patch(buffer_name, abs_off, enc, idx)
                verb = "modified"
            else:
                new_buffer_bytes = encode_slp_sequence(
                    [(v.tag, reencode_value_bytes(v.tag, v.value)) for v in state.values])
                proj.apply_array_relocation(
                    site.function_index, site.instruction_offset, site.is_object,
                    new_buffer_bytes, idx, state.changed_indices)
                verb = ("detached from the shared group and rebuilt" if shared_with
                        else "rebuilt")
        except Exception as exc:
            self.console.print(f"[red]Patch rejected (integrity check):[/] {exc}")
            return

        self.console.print(
            f"[green]{n} element{'s' if n != 1 else ''} {verb} in memory[/] "
            f"(array:{idx}) - [bold]patch[/] to write the .hbc"
        )

    def cmd_patch(self, args: list[str]) -> None:
        """patch [file.hbc]  - writes the patched image (from `edit` + Ctrl+X->Y) to a new .hbc"""
        proj = self._require_project()
        if not proj.is_patched:
            self.console.print(
                "No changes applied. Use [bold]edit func:<N>[/], edit, "
                "then [bold]Ctrl+X[/] -> [bold]Y[/]."
            )
            return

        stem = os.path.splitext(os.path.basename(proj.hbc_path))[0]
        default_out = os.path.join(os.path.dirname(proj.hbc_path), f"{stem}_patched.hbc")

        if args:
            out_path = os.path.abspath(os.path.expanduser(args[0]))
        else:
            try:
                raw = self.session.prompt("File output: ", default=default_out)
            except (EOFError, KeyboardInterrupt):
                self.console.print("[yellow]Cancelled.[/]")
                return
            out_path = os.path.abspath(os.path.expanduser(raw.strip() or default_out))

        if out_path == os.path.abspath(proj.hbc_path):
            self.console.print(
                "[red]Refusing to overwrite the original file.[/] Choose another name."
            )
            return

        try:
            n_bytes = proj.write_patched(out_path)
        except Exception as exc:
            self.console.print(f"[red]patch failed:[/] {exc}")
            return

        # sanity check: the written file must reopen
        try:
            _libhbc.HBCFile.open(out_path)
            check = "reopened OK"
        except Exception as exc:
            check = f"[yellow]WARN: reopen failed ({exc})[/]"
        self.console.print(
            f"[green]Written[/] -> [bold]{out_path}[/]  "
            f"({n_bytes} bytes, SHA-1 footer updated) - {check}"
        )

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
