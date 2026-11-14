"""`edit` command: the interactive editors (function body, string, identifier,
array/object literal) and the in-memory commit of each."""

import _libhbc
from cli.project import HermitProject


class EditCommand:
    def cmd_edit(self, args: list[str]) -> None:
        """edit func:<N|name|new> | edit string:<idx|value|new> | edit identifier:new | edit array:<N>  - interactive editor (Ctrl+X -> Y applies in memory; then `patch`)"""
        usage = ("Usage: edit func:<N|name>  |  edit func:new  |  "
                 "edit string:<idx|value>  |  edit string:new  |  "
                 "edit identifier:new  |  edit array:<N>")
        if len(args) != 1:
            self.console.print(usage)
            return
        proj = self._require_project()
        arg  = args[0]

        if arg.startswith("identifier:"):
            if arg != "identifier:new":
                self.console.print(usage)
                return
            self._edit_identifier_new(proj)
        elif arg.startswith("string:"):
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
        """edit string:<idx|value>. Empty and shared entries open: a shared one
        shows a banner and, if edited, detaches from its group. An Identifier
        opens with a rename banner (all its sites change) and always commits
        through the relocation, which rewrites its hash. `string:new` opens the
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

        # `O` in the rename prompt -> rename one site only.
        if getattr(state, "local_site", None) is not None:
            self._rename_identifier_site(proj, idx, state.value, state.local_site)
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
        # it in place would corrupt the others. An Identifier always relocates:
        # the relocation also rewrites its hash.
        is_id = kinds[idx] == _libhbc.StringKind.Identifier
        same_length = len(enc) == e.byte_length
        try:
            if is_id:
                proj.apply_string_relocation(idx, enc, state.value)
                n = sum(len(x.sites) for x in xrefs)
                self.console.print(
                    f"[green]identifier {idx} renamed in memory[/] "
                    f"({state.original!r} -> {state.value!r}, {n} site{'' if n == 1 else 's'}) "
                    f"- [bold]patch[/] to write the .hbc"
                )
            elif same_length and not shared_with:
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

    def _rename_identifier_site(self, proj: HermitProject, old_idx: int, value: str,
                                site) -> None:
        """One-site rename of Identifier `old_idx`: (1) the Identifier `value` is
        added, or reused if it exists; (2) the name operand of `site` is
        re-pointed to it (`apply_rename_site`, opcode widened if needed).
        Two commits: if (2) fails, the identifier added by (1) stays unused."""
        old = proj.hbc.strings()[old_idx]
        fi, off = site.function_index, site.instruction_offset
        f = proj.hbc.functions()[fi]
        if f.shared:
            self.console.print(
                f"[red]func[{fi}] has shared bytecode (×{f.shareCount})[/] - one site "
                "would change in every function of the group (cloning one function: bonus B12). "
                "Nothing changed.")
            return

        existing = next((i for i, (s, k) in enumerate(zip(proj.hbc.strings(),
                                                           proj.hbc.string_kinds()))
                         if k == _libhbc.StringKind.Identifier and str(s) == value), None)
        if existing is not None:
            new_idx = existing
            self.console.print(f"[dim]identifier {value!r} exists: reusing string:{new_idx}[/]")
        else:
            try:
                new_idx = proj.apply_add_identifier(value)
            except Exception as exc:
                self.console.print(f"[red]add_identifier rejected:[/] {exc} - nothing changed")
                return
            self.console.print(f"[green]new identifier [{new_idx}] {value!r}[/] added in memory")

        try:
            mnemonic = proj.apply_rename_site(fi, off, new_idx)
        except Exception as exc:
            self.console.print(f"[red]Re-point rejected:[/] {exc}")
            if existing is None:
                self.console.print(
                    f"[yellow]string:{new_idx} {value!r} stays in the image unused[/]")
            return
        widened = f" (widened {site.mnemonic} -> {mnemonic})" if mnemonic != site.mnemonic else ""
        self.console.print(
            f"[green]func[{fi}] @ {off:#06x} now names {value!r}[/]{widened}; "
            f"the other sites keep {old!r} - [bold]patch[/] to write the .hbc")

    def _edit_identifier_new(self, proj: HermitProject) -> None:
        """edit identifier:new: the string editor in "new string" mode, kind Identifier."""
        from cli.editor import run_string_editor

        state = run_string_editor(
            0, proj.hbc.strings(), proj.hbc.string_kinds(),
            proj.hbc.string_entries(), (), (), add=True,
            add_kind=_libhbc.StringKind.Identifier,
        )
        if getattr(state, "commit", False):
            self._finish_add_string(proj, state)

    def _finish_add_string(self, proj: HermitProject, state) -> None:
        """Commit of `edit string:new` / Ctrl+N / `edit identifier:new`."""
        is_id = getattr(state, "adds_identifier", False)
        what = "identifier" if is_id else "string"
        if not state.value:
            self.console.print(f"[dim]No {what} added.[/]")
            return
        try:
            idx = (proj.apply_add_identifier(state.value) if is_id
                   else proj.apply_add_string(state.value))
        except Exception as exc:
            self.console.print(f"[red]add_{what} rejected:[/] {exc}")
            return
        use = ("as the name of a GetById / PutById / ..." if is_id
               else "in a LoadConstString")
        self.console.print(
            f"[green]new {what} [{idx}] {state.value!r}[/] added in memory - "
            f"use it {use} with [bold]edit func:[/], then [bold]patch[/]"
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
                                 kinds=proj.hbc.string_kinds(),
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
