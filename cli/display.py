"""rich formatting helpers for the REPL's output."""

import bisect
from typing import Optional

from rich.panel import Panel
from rich.table import Table
from rich.text import Text

import _libhbc
from cli.literal_preview import LiteralPreview


def _short_path(path: str, max_len: int = 60) -> str:
    """Truncate a path from the left if it exceeds max_len characters."""
    if len(path) <= max_len:
        return path
    return "…" + path[-(max_len - 1):]


def _func_name(funcs: list, idx: int) -> str:
    return funcs[idx].name if idx < len(funcs) else f"func_{idx}"


def escape_ws(s: str) -> str:
    return s.replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def ellipsize(s: str, max_len: int) -> str:
    return s if len(s) <= max_len else s[:max_len - 3] + "..."


def _function_flags(f, *, call_flags: bool = True, extra: bool = False) -> list[str]:
    """Flag names of a function. `call_flags`: no-call/no-ctor. `extra`:
    overflow, shared×N and show-source."""
    flags = []
    if f.isStrictMode:         flags.append("strict")
    if f.isAsync:              flags.append("async")
    if f.isGenerator:          flags.append("gen")
    if f.hasExceptionHandler:  flags.append("try/catch")
    if f.hasDebugInfo:         flags.append("debug")
    if call_flags:
        if f.prohibitCall:         flags.append("no-call")
        if f.prohibitConstruct:    flags.append("no-ctor")
    if extra:
        if f.overflowed:           flags.append("overflow")
        if f.shared:               flags.append(f"shared×{f.shareCount}")
        if f.hasPreservedSource and not f.sourceIsHidden:
            flags.append("show-source")
    return flags


def _source_annotations(instrs: list, src_locs: Optional[list]):
    """Yields (instruction, "L<line>:<col>" or None): the comment appears only
    where the source line changes."""
    addr_list: list[int] = []
    line_by_addr: dict[int, tuple[int, int]] = {}
    if src_locs:
        addr_list, line_by_addr = build_src_lookup(src_locs)

    prev_line: Optional[int] = None
    for instr in instrs:
        comment: Optional[str] = None
        if addr_list:
            loc = lookup_src_line(addr_list, line_by_addr, instr.offset)
            if loc is not None and loc[0] != prev_line:
                prev_line = loc[0]
                comment = f"L{loc[0]}:{loc[1]}"
        yield instr, comment


def format_operand(op: _libhbc.Operand) -> Text:
    """Format a single operand with semantic colors."""
    t = op.type
    T = _libhbc.OperandType

    if t in (T.Reg8, T.Reg32):
        return Text(f"r{op.u32}", style="cyan")

    if t == T.StringID:
        s = op.resolved_string
        # The disassembler appends " [identifier]" / " [string]" to the value:
        # split it off so the quotes hold only the value.
        kind_label = ""
        for suffix in (" [identifier]", " [string]"):
            if s.endswith(suffix):
                kind_label = suffix
                s = s[: -len(suffix)]
                break
        result = Text(f'"{s}"', style="green")
        if kind_label:
            result.append(kind_label, style="dim")
        result.append(f" [{op.u32}]", style="dim")
        return result

    if t == T.Double:
        return Text(str(op.f64), style="yellow")

    if t in (T.Addr8, T.Addr32):
        offset = op.i32
        sign = "+" if offset >= 0 else ""
        return Text(f"{sign}{offset}", style="magenta")

    if t == T.Imm32:
        return Text(str(op.i32), style="yellow")

    # UInt8 / UInt16 / UInt32
    return Text(str(op.u32), style="white")


def format_instruction_line(
    instr: _libhbc.Instruction,
    annotation: Optional[str] = None,
) -> Text:
    """Format one disassembly line: `0x0000  Mnemonic    operand, operand`."""
    line = Text()
    line.append(f"  {instr.offset:#06x}  ", style="dim")
    line.append(f"{instr.mnemonic:<20}", style="bold yellow")

    for i, op in enumerate(instr.operands):
        if i:
            line.append(", ")
        line.append_text(format_operand(op))

    if annotation:
        line.append(f"  ; {annotation}", style="dim italic green")

    return line


def make_info_table(hbc: _libhbc.HBCFile) -> Table:
    """Summary table of the HBC header."""
    hdr = hbc.header()
    funcs = hbc.functions()
    global_idx = hdr.global_code_index

    table = Table(show_header=False, box=None, padding=(0, 2))
    table.add_column("Field", style="bold", min_width=24)
    table.add_column("Value")

    table.add_row("Version",          str(hdr.version))
    table.add_row("File size",        f"{hdr.file_length:,} bytes")
    table.add_row("Functions",        str(hdr.function_count))
    table.add_row("Strings",          str(hdr.string_count))
    table.add_row("  Identifiers",    str(hdr.identifier_count))
    table.add_row("  Overflow",       str(hdr.overflow_string_count))
    table.add_row("RegExps",          str(hdr.reg_exp_count))
    table.add_row("BigInts",          str(hdr.bigint_count))
    table.add_row("Segment ID",       str(hdr.segment_id))
    table.add_row("Function sources", str(hdr.function_source_count))
    table.add_row("Options",          f"{hdr.options:#04x}")
    table.add_row("Global function",  f"[{global_idx}] {funcs[global_idx].name}")
    table.add_row("Debug info offset", f"{hdr.debug_info_offset:#010x}")

    # header.cjsModuleCount > 0 only for a file compiled with Hermes's native
    # `-commonjs` mode -- never both this and Metro's `__d()` pattern on the
    # same file in practice, see doc/hbc-format/v96-format.md §9.
    try:
        cjs_mods = hbc.cjs_modules()
    except Exception:
        cjs_mods = []
    if cjs_mods:
        table.add_row("CJS modules", str(len(cjs_mods)))
    else:
        try:
            metro_mods = hbc.recover_modules()
        except Exception:
            metro_mods = []
        table.add_row("Metro modules", str(len(metro_mods)))

    src_files = hbc.source_files()
    if src_files:
        table.add_row("Source file(s)", src_files[0])
        for sf in src_files[1:]:
            table.add_row("", sf)

    table.add_row("Source hash",      hbc.header().source_hash.hex())

    return table


# ---------------------------------------------------------------------------
# `info sections | strings | functions`
# ---------------------------------------------------------------------------

def sections_info(hbc: _libhbc.HBCFile) -> dict:
    """Section map sorted by offset, with the uncovered gaps between sections.

    Returns {"rows": [(name, start, end, size, align, gap_before)],
             "file_length": int, "covered": int, "uncovered": int}.
    """
    secs = sorted(hbc.section_map(), key=lambda x: (x.offset, x.name))
    rows = []
    cursor = 0
    covered = 0
    for sec in secs:
        end = sec.offset + sec.size
        gap = max(0, sec.offset - cursor)
        rows.append((sec.name, sec.offset, end, sec.size, sec.align, gap))
        covered += sec.size
        cursor = max(cursor, end)
    file_length = hbc.header().file_length
    return {
        "rows": rows,
        "file_length": file_length,
        "covered": covered,
        "uncovered": file_length - covered,
    }


def make_info_sections_table(hbc: _libhbc.HBCFile) -> Table:
    info = sections_info(hbc)
    table = Table(box=None, padding=(0, 2))
    table.add_column("Section", style="bold")
    table.add_column("Start", justify="right")
    table.add_column("End", justify="right")
    table.add_column("Size", justify="right")
    table.add_column("Align", justify="right")
    table.add_column("Gap before", justify="right")
    for name, start, end, size, align, gap in info["rows"]:
        table.add_row(name, f"{start:#010x}", f"{end:#010x}", f"{size:,}",
                      str(align), f"{gap:,}" if gap else "")
    table.add_row("", "", "", "", "", "")
    table.add_row("[bold]File[/bold]", "", f"{info['file_length']:#010x}",
                  f"{info['file_length']:,}", "", "")
    table.add_row("[dim]Not in any section[/dim]", "", "",
                  f"{info['uncovered']:,}", "", "")
    return table


def strings_stats(hbc: _libhbc.HBCFile) -> dict:
    """Aggregate statistics over the string table."""
    entries = hbc.string_entries()
    texts = hbc.strings()
    ident = _libhbc.StringKind.Identifier
    matrix = {(k, enc): 0 for k in ("string", "identifier") for enc in ("ascii", "utf16")}
    overflow = 0
    for e in entries:
        k = "identifier" if e.kind == ident else "string"
        matrix[(k, "utf16" if e.is_utf16 else "ascii")] += 1
        overflow += 1 if e.is_overflow else 0
    lengths = [e.char_length for e in entries]
    non_ascii = sum(1 for t in texts if any(ord(c) > 0x7F for c in t))
    return {
        "total": len(entries),
        "matrix": matrix,
        "overflow": overflow,
        "empty": sum(1 for n in lengths if n == 0),
        "unique": len(set(texts)),
        "duplicates": len(texts) - len(set(texts)),
        "non_ascii": non_ascii,
        "storage_bytes": hbc.header().string_storage_size,
        "len_min": min(lengths) if lengths else 0,
        "len_max": max(lengths) if lengths else 0,
        "len_avg": (sum(lengths) / len(lengths)) if lengths else 0.0,
    }


def make_info_strings_table(hbc: _libhbc.HBCFile) -> Table:
    st = strings_stats(hbc)
    m = st["matrix"]
    total = st["total"] or 1

    def pct(n: int) -> str:
        return f"{n:,}  ({100.0 * n / total:.1f}%)"

    table = Table(show_header=False, box=None, padding=(0, 2))
    table.add_column("Field", style="bold", min_width=24)
    table.add_column("Value")
    table.add_row("Total",                 f"{st['total']:,}")
    table.add_row("  String, ASCII",       pct(m[("string", "ascii")]))
    table.add_row("  String, UTF-16",      pct(m[("string", "utf16")]))
    table.add_row("  Identifier, ASCII",   pct(m[("identifier", "ascii")]))
    table.add_row("  Identifier, UTF-16",  pct(m[("identifier", "utf16")]))
    table.add_row("Overflow",              pct(st["overflow"]))
    table.add_row("Empty",                 f"{st['empty']:,}")
    table.add_row("Containing non-ASCII",  pct(st["non_ascii"]))
    table.add_row("Unique contents",       f"{st['unique']:,}")
    table.add_row("Duplicate contents",    f"{st['duplicates']:,}")
    table.add_row("Storage",               f"{st['storage_bytes']:,} bytes")
    table.add_row("Length (chars)",
                  f"min {st['len_min']:,}  max {st['len_max']:,}  avg {st['len_avg']:.1f}")
    return table


def functions_stats(hbc: _libhbc.HBCFile) -> dict:
    """Aggregate statistics over the function table."""
    funcs = hbc.functions()
    sizes = [f.bytecodeSizeInBytes for f in funcs]
    shared = [f for f in funcs if f.shared]
    # One body = one (offset, size) group; count groups of functions sharing it.
    groups = {(f.offset, f.bytecodeSizeInBytes) for f in shared}
    return {
        "total": len(funcs),
        "small": sum(1 for f in funcs if not f.overflowed),
        "overflow": sum(1 for f in funcs if f.overflowed),
        "strict": sum(1 for f in funcs if f.isStrictMode),
        "async": sum(1 for f in funcs if f.isAsync),
        "generator": sum(1 for f in funcs if f.isGenerator),
        "exception_handler": sum(1 for f in funcs if f.hasExceptionHandler),
        "debug_info": sum(1 for f in funcs if f.hasDebugInfo),
        "prohibit_call": sum(1 for f in funcs if f.prohibitCall),
        "prohibit_construct": sum(1 for f in funcs if f.prohibitConstruct),
        "preserved_source": sum(1 for f in funcs if f.hasPreservedSource),
        "shared_functions": len(shared),
        "shared_groups": len(groups),
        "size_total": sum(sizes),
        "size_min": min(sizes) if sizes else 0,
        "size_max": max(sizes) if sizes else 0,
        "size_avg": (sum(sizes) / len(sizes)) if sizes else 0.0,
        "frame_max": max((f.frameSize for f in funcs), default=0),
        "params_max": max((f.paramCount for f in funcs), default=0),
    }


def make_info_functions_table(hbc: _libhbc.HBCFile) -> Table:
    st = functions_stats(hbc)
    total = st["total"] or 1

    def pct(n: int) -> str:
        return f"{n:,}  ({100.0 * n / total:.1f}%)"

    table = Table(show_header=False, box=None, padding=(0, 2))
    table.add_column("Field", style="bold", min_width=24)
    table.add_column("Value")
    table.add_row("Total",               f"{st['total']:,}")
    table.add_row("  Small header",      pct(st["small"]))
    table.add_row("  Overflow header",   pct(st["overflow"]))
    table.add_row("Strict mode",         pct(st["strict"]))
    table.add_row("Async",               pct(st["async"]))
    table.add_row("Generator",           pct(st["generator"]))
    table.add_row("Exception handlers",  pct(st["exception_handler"]))
    table.add_row("Debug info",          pct(st["debug_info"]))
    table.add_row("Prohibit call",       pct(st["prohibit_call"]))
    table.add_row("Prohibit construct",  pct(st["prohibit_construct"]))
    table.add_row("Preserved source",    pct(st["preserved_source"]))
    table.add_row("Shared bodies",
                  f"{st['shared_functions']:,} functions in {st['shared_groups']:,} groups")
    table.add_row("Bytecode size",
                  f"total {st['size_total']:,}  min {st['size_min']:,}  "
                  f"max {st['size_max']:,}  avg {st['size_avg']:.1f}")
    table.add_row("Max frame size",      f"{st['frame_max']:,}")
    table.add_row("Max param count",     f"{st['params_max']:,}")
    return table


def make_strings_table(
    strings: list[str],
    kinds: list,
    filter_text: Optional[str] = None,
) -> Table:
    """String table with optional filter."""
    table = Table(show_header=True, box=None, padding=(0, 1))
    table.add_column("idx", style="dim", justify="right", min_width=5)
    table.add_column("value", min_width=30)
    table.add_column("kind", min_width=12)

    for i, (s, k) in enumerate(zip(strings, kinds)):
        if filter_text and filter_text.lower() not in s.lower():
            continue
        kind_str = "identifier" if k == _libhbc.StringKind.Identifier else "string"
        kind_style = "cyan" if k == _libhbc.StringKind.Identifier else "green"
        table.add_row(
            f"[{i}]",
            repr(s) if any(c < ' ' for c in s) else s,
            Text(f"[{kind_str}]", style=kind_style),
        )

    return table


def make_functions_table(
    funcs: list,
    global_idx: int,
) -> Table:
    """Function table with js_params and flags."""
    table = Table(show_header=True, box=None, padding=(0, 1))
    table.add_column("idx",     style="dim",  justify="right", min_width=5)
    table.add_column("name",    min_width=28)
    table.add_column("offset",  min_width=10)
    table.add_column("size",    justify="right", min_width=6)
    table.add_column("params",  justify="right", min_width=6)
    table.add_column("regs",    justify="right", min_width=5)
    table.add_column("flags",   min_width=16)

    for f in funcs:
        js_params = f.paramCount - 1
        marker = "*" if f.index == global_idx else " "
        flag_str = ",".join(_function_flags(f, extra=True))

        table.add_row(
            f"[{f.index}]{marker}",
            f.name,
            f"{f.offset:#010x}",
            str(f.bytecodeSizeInBytes),
            str(js_params),
            str(f.frameSize),
            flag_str,
        )

    return table


def functions_to_ansi_rows(
    funcs: list,
    global_idx: int,
    module_ids: Optional[dict] = None,
    cjs_module_paths: Optional[dict] = None,
) -> list[str]:
    """Format functions as ANSI rows (fast, without rich.Table).

    `module_ids`: functionIndex -> Metro numeric module id (recover_modules(),
    `__d()` pattern-matching -- the case real React Native bundles use).
    `cjs_module_paths`: functionIndex -> source file path (cjs_modules(),
    header.cjsModuleCount -- Hermes's native `-commonjs` mode, never present
    on the same file as `module_ids` in practice; see
    doc/hbc-format/v96-format.md §9). Both tag only a module's own wrapper
    function, never functions nested inside it.
    """
    rows = []
    for f in funcs:
        marker = "*" if f.index == global_idx else " "
        js_params = f.paramCount - 1
        flag_str = ",".join(_function_flags(f, extra=True))
        mod_str = ""
        if module_ids and f.index in module_ids:
            mod_str = f"  \x1b[36mmodule #{module_ids[f.index]}\x1b[0m"
        elif cjs_module_paths and f.index in cjs_module_paths:
            mod_str = f"  \x1b[36mmodule {cjs_module_paths[f.index]!r}\x1b[0m"
        rows.append(
            f"\x1b[2m[{f.index:5d}]{marker}\x1b[0m "
            f"\x1b[1m{f.name:<32}\x1b[0m "
            f"\x1b[33m{f.offset:#010x}\x1b[0m "
            f"\x1b[36m{js_params:3d}p {f.frameSize:3d}r\x1b[0m "
            f"{flag_str:<14}"
            + mod_str
        )
    return rows


def strings_to_ansi_rows(
    strings: list,
    kinds: list,
    filter_text: Optional[str] = None,
    kind_filter: Optional[str] = None,
) -> list[str]:
    """Strings as ANSI rows (fast, no rich.Table). `kind_filter`: None,
    "identifier" or "string"."""
    rows = []
    for i, (s, k) in enumerate(zip(strings, kinds)):
        is_id = (k == _libhbc.StringKind.Identifier)
        if kind_filter == "identifier" and not is_id:
            continue
        if kind_filter == "string" and is_id:
            continue
        if filter_text and filter_text.lower() not in s.lower():
            continue
        kind_str = "\x1b[36m[identifier]\x1b[0m" if is_id else "\x1b[32m[string]    \x1b[0m"
        display_s = ellipsize(escape_ws(s), 64)
        rows.append(f"\x1b[2m[{i:6d}]\x1b[0m {display_s:<66} {kind_str}")
    return rows


def arrays_to_ansi_rows(
    sites: list,
    array_buf: bytes,
    obj_key_buf: bytes,
    obj_val_buf: bytes,
    strings: list,
    funcs: list,
    filter_text: Optional[str] = None,
    kind_filter: Optional[str] = None,
    func_filter: Optional[int] = None,
) -> list[str]:
    """Literal-buffer sites (NewArray/ObjectWithBuffer[Long]) as ANSI rows.
    `kind_filter`: None, "array" or "object". `func_filter`: a function index."""
    from cli.literals import decode_slp, slp_render

    rows = []
    for i, s in enumerate(sites):
        if kind_filter == "array" and s.is_object:
            continue
        if kind_filter == "object" and not s.is_object:
            continue
        if func_filter is not None and s.function_index != func_filter:
            continue

        try:
            if s.is_object:
                keys = decode_slp(obj_key_buf, s.key_buffer_idx, s.count)
                vals = decode_slp(obj_val_buf, s.buffer_idx, s.count)
                items = ", ".join(
                    f"{slp_render(k, strings)}: {slp_render(v, strings)}"
                    for k, v in zip(keys, vals)
                )
                literal = "{" + items + "}"
            else:
                vals = decode_slp(array_buf, s.buffer_idx, s.count)
                literal = "[" + ", ".join(slp_render(v, strings) for v in vals) + "]"
        except Exception:
            literal = "<decode error>"

        literal = ellipsize(literal, 60)

        fname = _func_name(funcs, s.function_index)
        if filter_text:
            ft = filter_text.lower()
            if ft not in literal.lower() and ft not in fname.lower():
                continue

        kindtxt = "\x1b[36m[object]\x1b[0m" if s.is_object else "\x1b[32m[array] \x1b[0m"
        rows.append(
            f"\x1b[2m[{i:6d}]\x1b[0m func[{s.function_index}] {fname}  "
            f"\x1b[2m@{s.instruction_offset:#06x}\x1b[0m  {kindtxt}  {literal}"
        )
    return rows


def _call_row(idx: int, fname: str, offset: int, mnemonic: str) -> str:
    return (
        f"\x1b[2m[{idx}]\x1b[0m "
        f"\x1b[1m{fname}\x1b[0m  "
        f"@ \x1b[33m{offset:#06x}\x1b[0m  "
        f"\x1b[2m({mnemonic})\x1b[0m"
    )


def call_graph_to_ansi_rows(sites: list, funcs: list, mode: str) -> list[str]:
    """Call graph sites for show_list(). mode = 'callers' or 'callees'."""
    rows = []
    for cs in sites:
        if mode == "callers":
            rows.append(_call_row(cs.caller_index, _func_name(funcs, cs.caller_index),
                                  cs.offset, cs.mnemonic))
        elif cs.is_resolved:
            rows.append(_call_row(cs.callee_index, _func_name(funcs, cs.callee_index),
                                  cs.offset, cs.mnemonic))
        else:
            rows.append(
                f"\x1b[2m[unresolved]\x1b[0m  "
                f"@ \x1b[33m{cs.offset:#06x}\x1b[0m  "
                f"\x1b[2m({cs.mnemonic})\x1b[0m"
            )
    return rows


def xrefs_detail_to_ansi_rows(xrefs: list, funcs: list) -> list[str]:
    """Detailed XREFs for show_list(): a header per string, one row per site."""
    rows = []
    for xr in xrefs:
        s = ellipsize(escape_ws(xr.string_value), 52)
        n = len(xr.sites)
        rows.append(
            f"\x1b[2m[{xr.string_index}]\x1b[0m "
            f"\x1b[32m\"{s}\"\x1b[0m  "
            f"\x1b[33m{n} ref{'s' if n != 1 else ''}\x1b[0m"
        )
        for site in xr.sites:
            fname = _func_name(funcs, site.function_index)
            rows.append(
                f"  \x1b[2m->\x1b[0m "
                f"\x1b[2m[{site.function_index}]\x1b[0m "
                f"\x1b[1m{fname}\x1b[0m  "
                f"@ \x1b[33m{site.instruction_offset:#06x}\x1b[0m  "
                f"\x1b[2m({site.mnemonic}{_literal_ref_text(site)})\x1b[0m"
            )
    return rows


_LITERAL_ROLE = {"ArrayElement": "element", "ObjectKey": "key", "ObjectValue": "value"}


def _literal_ref_text(site) -> str:
    """`, element 1` / `, key 0` / `, value 2 x2`: where in the literal buffer a
    literal reference sits (empty for an instruction operand)."""
    role = _LITERAL_ROLE.get(site.kind.name)
    if role is None:
        return ""
    text = f", {role} {site.element_index}"
    if site.element_count > 1:
        text += f" x{site.element_count}"
    return text


def xrefs_summary_to_ansi_rows(xrefs: list) -> list[str]:
    """String XREF summary as ANSI rows for show_list()."""
    rows = []
    for xr in xrefs:
        s = ellipsize(escape_ws(xr.string_value), 52)
        n = len(xr.sites)
        rows.append(
            f"\x1b[2m[{xr.string_index:6d}]\x1b[0m "
            f"\x1b[32m{s:<54}\x1b[0m "
            f"\x1b[33m{n:4d} ref{'s' if n != 1 else ' '}\x1b[0m"
        )
    return rows


def hex_view_rows(
    data: bytes, base: int = 0, highlight: tuple[int, int] | None = None
) -> list[str]:
    """hexdump-style view, 16 bytes per row: `offset  HH … HH  |ascii|`.
    `base` is the ABSOLUTE offset of the first byte; `highlight` is a half-open
    range of ABSOLUTE offsets shown in reverse video. Non-printable bytes
    are `.`. Used by the string editor."""
    hl0, hl1 = highlight if highlight else (0, 0)
    rows: list[str] = []
    if not data:
        return [f"\x1b[2m{base:08x}\x1b[0m  (empty)"]
    for row_start in range(0, len(data), 16):
        chunk = data[row_start:row_start + 16]
        abs_row = base + row_start
        hexs, asciis = [], []
        for i, b in enumerate(chunk):
            off = abs_row + i
            hh = f"{b:02x}"
            ch = chr(b) if 32 <= b < 127 else "."
            if hl0 <= off < hl1:
                hexs.append(f"\x1b[7m{hh}\x1b[0m")
                asciis.append(f"\x1b[7m{ch}\x1b[0m")
            else:
                hexs.append(hh)
                asciis.append(ch)
        hex_str = " ".join(hexs) + "   " * (16 - len(chunk))
        rows.append(f"\x1b[2m{abs_row:08x}\x1b[0m  {hex_str}  |{''.join(asciis)}|")
    return rows


def build_src_lookup(src_locs: list) -> tuple[list, dict]:
    """(sorted address list, addr -> (line, col)) from source locations, using
    the first entry of each address."""
    addr_list: list[int] = []
    line_by_addr: dict[int, tuple[int, int]] = {}
    for loc in src_locs:
        if loc.address not in line_by_addr:
            line_by_addr[loc.address] = (loc.line, loc.column)
            addr_list.append(loc.address)
    return addr_list, line_by_addr


def lookup_src_line(
    addr_list: list[int],
    line_by_addr: dict[int, tuple[int, int]],
    instr_offset: int,
) -> Optional[tuple[int, int]]:
    """(line, col) of the nearest entry with addr <= instr_offset, or None."""
    if not addr_list:
        return None
    idx = bisect.bisect_right(addr_list, instr_offset) - 1
    if idx < 0:
        return None
    return line_by_addr[addr_list[idx]]


def make_disasm_panel(
    func,
    instrs: list,
    src_locs: Optional[list] = None,
) -> Panel:
    """rich Panel with header + disassembly body for one function."""
    js_params = func.paramCount - 1
    flags_str = "  ".join(_function_flags(func))

    header_text = Text()
    header_text.append(func.name, style="bold")
    header_text.append(f"  params={js_params}  regs={func.frameSize}", style="dim")
    if flags_str:
        header_text.append(f"  {flags_str}", style="italic yellow")

    body = Text()
    for instr, src_comment in _source_annotations(instrs, src_locs):
        body.append_text(format_instruction_line(instr, annotation=src_comment))
        body.append("\n")

    if func.hasExceptionHandler and func.exceptionHandlers:
        body.append("\n")
        body.append("  Exception Handlers:\n", style="dim")
        for i, eh in enumerate(func.exceptionHandlers):
            body.append(f"    [{i}]  try ", style="dim")
            body.append(f"{eh.start:#06x}", style="dim")
            body.append("..", style="dim")
            body.append(f"{eh.end:#06x}", style="dim")
            body.append("  ->  catch @ ", style="dim")
            body.append(f"{eh.target:#06x}", style="cyan")
            body.append("\n")

    content = Text()
    content.append_text(header_text)
    content.append("\n")
    content.append_text(body)

    return Panel(content, title=f"func {func.index}", border_style="blue")


def _fmt_instr_ansi(instr, src_comment: Optional[str] = None,
                    previewer: Optional[LiteralPreview] = None) -> str:
    """An instruction as an ANSI string for show_list(). With a `previewer`, a
    New{Array,Object}WithBuffer[Long] instruction gets the literal it builds after
    its operands."""
    ops = []
    for op in instr.operands:
        t = op.type
        T = _libhbc.OperandType
        if t in (T.Reg8, T.Reg32):
            ops.append(f"\x1b[36mr{op.u32}\x1b[0m")
        elif t == T.StringID:
            s = op.resolved_string
            for suffix in (" [identifier]", " [string]"):
                if s.endswith(suffix):
                    s = f'"{s[:-len(suffix)]}"' + f"\x1b[2m{suffix}[{op.u32}]\x1b[0m"
                    break
            else:
                s = f'"{s}"'
            ops.append(f"\x1b[32m{s}\x1b[0m")
        elif t == T.Double:
            ops.append(f"\x1b[33m{op.f64}\x1b[0m")
        elif t in (T.Addr8, T.Addr32):
            sign = "+" if op.i32 >= 0 else ""
            ops.append(f"\x1b[35m{sign}{op.i32}\x1b[0m")
        elif t == T.Imm32:
            ops.append(f"\x1b[33m{op.i32}\x1b[0m")
        else:
            ops.append(str(op.u32))
    ops_str = ", ".join(ops)
    if previewer is not None:
        pv = previewer.render(instr)
        if pv:
            ops_str += f"   {pv}"
    cmt = f"  \x1b[2m\x1b[32m; {src_comment}\x1b[0m" if src_comment else ""
    return (
        f"  \x1b[2m{instr.offset:#06x}\x1b[0m  "
        f"\x1b[1m\x1b[33m{instr.mnemonic:<20}\x1b[0m"
        f"{ops_str}{cmt}"
    )


def disasm_to_ansi_rows(
    func,
    instrs: list,
    src_locs: Optional[list] = None,
    previewer: Optional[LiteralPreview] = None,
) -> list[str]:
    """ANSI rows of the main disasm view: a header, then one row per
    instruction (`; L<line>:<col>` where the source line changes) with the jump
    table under each SwitchImm."""
    js_params = func.paramCount - 1
    flags = _function_flags(func, call_flags=False)
    hdr = (
        f"\x1b[1m{func.name}\x1b[0m  "
        f"\x1b[2mparams={js_params} regs={func.frameSize}"
        + (f"  {' '.join(flags)}" if flags else "")
        + "\x1b[0m"
    )

    rows: list[str] = [hdr, ""]

    for instr, src_comment in _source_annotations(instrs, src_locs):
        rows.append(_fmt_instr_ansi(instr, src_comment, previewer))
        if instr.mnemonic == "SwitchImm" and instr.switch_targets:
            min_val = instr.operands[3].u32
            default_target = instr.offset + instr.operands[2].i32
            for i, target in enumerate(instr.switch_targets):
                rows.append(f"  \x1b[2mcase {min_val + i:<4}\x1b[0m\x1b[36m-> {target:#06x}\x1b[0m")
            rows.append(f"  \x1b[2mdefault   \x1b[0m\x1b[36m-> {default_target:#06x}\x1b[0m")

    if func.hasExceptionHandler and func.exceptionHandlers:
        rows.append("")
        rows.append("  \x1b[2mException Handlers:\x1b[0m")
        for i, eh in enumerate(func.exceptionHandlers):
            rows.append(
                f"  \x1b[2m[{i}]  try {eh.start:#06x}..{eh.end:#06x}"
                f"  ->  catch @ \x1b[0m\x1b[36m{eh.target:#06x}\x1b[0m"
            )
    return rows


def debug_source_table_rows(
    func,
    src_locs: Optional[list],
    src_files: Optional[list[str]] = None,
) -> list[str]:
    """The 's' view of the disasm TUI: the function's debug offset and its
    source location entries, `bc <addr>  L<line>:<col>` (hermes --dump-bytecode style)."""
    rows: list[str] = []
    rows.append(
        f"\x1b[1mDebug source table\x1b[0m  -  "
        f"func[{func.index}] \x1b[1m{func.name}\x1b[0m  "
        f"\x1b[2m(SL offset {func.debugOffset:#06x})\x1b[0m"
    )

    if src_files:
        rows.append(f"  \x1b[2msource file: {_short_path(src_files[0])}\x1b[0m")

    rows.append("")

    if not func.hasDebugInfo:
        rows.append("  \x1b[2m(no debug info)\x1b[0m")
        return rows

    if not src_locs:
        rows.append("  \x1b[2m(no source location entries)\x1b[0m")
        return rows

    for loc in src_locs:
        rows.append(
            f"    \x1b[2mbc\x1b[0m \x1b[33m{loc.address:4d}\x1b[0m  "
            f"\x1b[32mL{loc.line}:{loc.column}\x1b[0m"
        )

    rows.append("")
    rows.append(f"  \x1b[2m{len(src_locs)} entr{'y' if len(src_locs)==1 else 'ies'}\x1b[0m")
    return rows


def all_debug_source_table_rows(hbc) -> list[str]:
    """Debug source table of every function (hermes --dump-bytecode style)."""
    src_files = hbc.source_files()
    funcs = hbc.functions()
    rows: list[str] = []
    rows.append("\x1b[1mDebug source table\x1b[0m")
    if src_files:
        rows.append(f"  \x1b[2mfilename: {_short_path(src_files[0])}\x1b[0m")
    rows.append("")

    for func in funcs:
        if not func.hasDebugInfo:
            continue
        locs = hbc.source_locations(func.index)
        rows.append(
            f"  \x1b[2m{func.debugOffset:#06x}\x1b[0m  "
            f"function idx \x1b[33m{func.index}\x1b[0m  "
            f"\x1b[1m{func.name}\x1b[0m  "
            f"\x1b[2mparams={func.paramCount-1}\x1b[0m"
        )
        for loc in locs:
            rows.append(
                f"    \x1b[2mbc\x1b[0m \x1b[33m{loc.address:4d}\x1b[0m  "
                f"\x1b[32mL{loc.line}:{loc.column}\x1b[0m"
            )
        if not locs:
            rows.append("    \x1b[2m(none)\x1b[0m")
    rows.append("")
    rows.append("  \x1b[2mend of debug source table\x1b[0m")
    return rows
