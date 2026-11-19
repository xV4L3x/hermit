"""cfg_dot.py - CFG and Metro module graph -> DOT -> graph-easy terminal rendering."""

from __future__ import annotations

import os
import re
import shutil
import subprocess

import _libhbc
from _libhbc import EdgeKind


# ── ANSI colorizer ───────────────────────────────────────────────────────────

_R      = "\x1b[0m"
_GREEN  = "\x1b[32m"
_RED    = "\x1b[31m"
_CYAN   = "\x1b[1;36m"
_YELLOW = "\x1b[33m"
_PURPLE = "\x1b[35m"
_ORANGE = "\x1b[33;1m"
_DIM    = "\x1b[2m"
_BOLD   = "\x1b[1;37m"


def _colorize(text: str) -> str:
    """Add ANSI colors to graph-easy boxart output."""
    out = []
    for line in text.split("\n"):
        # Dim offset + yellow mnemonic, on ALL occurrences (multi-column
        # layouts put two instructions on one row)
        line = re.sub(
            r"(0x[0-9a-f]{4})  (\w+)",
            lambda m: f"{_DIM}{m.group(1)}{_R}  {_YELLOW}{m.group(2)}{_R}",
            line,
        )
        # String values in quotes -> green
        line = re.sub(r'"([^"]*)"', lambda m: f'{_GREEN}"{m.group(1)}"{_R}', line)
        # Kind annotations outside quotes ([identifier]) -> dim, except [entry]/[loop]
        line = re.sub(r"\[(?!entry\b|loop\b)(\w+)\]",
                      lambda m: f"{_DIM}[{m.group(1)}]{_R}", line)
        # Edge labels (mnemonics never contain a standalone T or F)
        line = re.sub(r"(?<![A-Za-z])T(?![A-Za-z])", f"{_GREEN}T{_R}", line)
        line = re.sub(r"(?<![A-Za-z])F(?![A-Za-z])", f"{_RED}F{_R}", line)
        line = re.sub(r"(?<![A-Za-z])def(?![A-Za-z])", f"{_YELLOW}def{_R}", line)
        line = re.sub(r"(?<![A-Za-z])exc(?![A-Za-z])", f"{_ORANGE}exc{_R}", line)
        line = re.sub(r"(?<![A-Za-z])resume(?![A-Za-z])", f"{_PURPLE}resume{_R}", line)
        # Block tags
        line = line.replace("[entry]", f"{_CYAN}[entry]{_R}")
        line = line.replace("[loop]",  f"{_YELLOW}[loop]{_R}")
        # Arrow heads
        line = re.sub(r"[◀▼▶▲]", lambda m: f"{_DIM}{m.group()}{_R}", line)
        # Block IDs
        line = re.sub(r"(?<![A-Za-z])(B\d+)(?![A-Za-z])",
                      lambda m: f"{_BOLD}{m.group(1)}{_R}", line)
        out.append(line)
    return "\n".join(out)


# ── DOT generation ──────────────────────────────────────────────────────────

_EDGE_STYLE: dict[EdgeKind, dict] = {
    EdgeKind.FallThrough:      {"color": "grey",   "label": "",    "style": ""},
    EdgeKind.Jump:             {"color": "cyan",   "label": "",    "style": ""},
    EdgeKind.CondTaken:        {"color": "green",  "label": "T",   "style": ""},
    EdgeKind.CondFallThru:     {"color": "red",    "label": "F",   "style": ""},
    EdgeKind.BackEdge:         {"color": "purple", "label": "",    "style": ""},
    EdgeKind.SwitchDefault:    {"color": "yellow", "label": "def", "style": ""},
    EdgeKind.SwitchCase:       {"color": "yellow", "label": "",    "style": ""},
    EdgeKind.ExceptionHandler: {"color": "orange", "label": "exc", "style": "dashed"},
    # generator `yield`: the next generator call resumes at the target
    EdgeKind.GeneratorResume:  {"color": "purple", "label": "resume", "style": "dotted"},
}


def _fmt_instr(instr) -> str:
    ops = ", ".join(_fmt_op(o) for o in instr.operands)
    line = f"0x{instr.offset:04x}  {instr.mnemonic}"
    if ops:
        line += f"  {ops}"
    return line


def _fmt_op(op) -> str:
    t = op.type
    OT = _libhbc.OperandType
    if t in (OT.Reg8, OT.Reg32):
        return f"r{op.u32}"
    if t in (OT.Addr8, OT.Addr32):
        return f"{op.i32:+d}"
    if t == OT.StringID:
        s = op.resolved_string
        # Move kind annotation outside quotes: "name" [identifier]
        km = re.match(r'^(.*?)(\s+\[\w+\])$', s)
        if km:
            return f'"{km.group(1)}"{km.group(2)}'
        return f'"{s}"' if s else f"str:{op.u32}"
    return str(op.u32)


# Above this many instructions in the CFG, labels are truncated.
_COMPACT_THRESHOLD = 300
_COMPACT_HEAD      = 10    # instructions shown at the top of each block
_COMPACT_TAIL      = 5     # instructions shown at the bottom of each block


def cfg_to_dot(cfg, func, *, compact: bool = False) -> str:
    """CFGData + FunctionInfo as a DOT digraph. `compact` truncates the labels
    of long blocks, which otherwise exceed graph-easy's Perl regex recursion limit."""
    lines: list[str] = []
    lines.append(f'digraph "{func.name}" {{')
    lines.append("  graph [rankdir=TB fontname=Monospace];")
    lines.append("  node  [shape=box fontname=Monospace style=rounded];")
    lines.append("  edge  [fontname=Monospace fontsize=10];")
    lines.append("")

    for bb in cfg.blocks:
        tags = []
        if bb.id == cfg.entry_id:
            tags.append("entry")
        if bb.is_loop_header:
            tags.append("loop")
        header = f"B{bb.id}"
        if tags:
            header += f"  [{', '.join(tags)}]"

        instr_lines = [_fmt_instr(i) for i in bb.instructions]
        n = len(instr_lines)

        if compact and n > _COMPACT_HEAD + _COMPACT_TAIL + 1:
            hidden = n - _COMPACT_HEAD - _COMPACT_TAIL
            shown  = (
                instr_lines[:_COMPACT_HEAD]
                + [f"  ... {hidden} more ..."]
                + instr_lines[-_COMPACT_TAIL:]
            )
            header_with_count = f"{header}  ({n} instrs)"
        else:
            shown = instr_lines
            header_with_count = header

        all_lines = (
            [header_with_count,
             "─" * max(len(header_with_count), *(len(l) for l in shown), 1)]
            + shown
        )

        # DOT label: \l = left-aligned newline
        label = r"\l".join(all_lines) + r"\l"
        label = label.replace('"', r'\"')

        attrs = f'label="{label}"'
        if bb.id == cfg.entry_id:
            attrs += ' style="rounded,filled" fillcolor="#d0e8ff"'

        lines.append(f"  B{bb.id} [{attrs}];")

    lines.append("")

    for bb in cfg.blocks:
        for edge in bb.successors:
            es = _EDGE_STYLE.get(edge.kind, {"color": "black", "label": "", "style": ""})
            color, label, style = es["color"], es["label"], es["style"]
            attrs = f'color="{color}"'
            if label:
                attrs += f' label="{label}" fontcolor="{color}"'
            if style:
                attrs += f' style="{style}"'
            lines.append(f"  B{bb.id} -> B{edge.target_id} [{attrs}];")

    lines.append("}")
    return "\n".join(lines)


# ── Self-loop arrowhead fix ──────────────────────────────────────────────────
# graph-easy draws a self-loop (a block with a back-edge to itself) with the
# arrowhead at the BOTTOM of the block and the tail on top. For a CFG back-edge
# that is backwards: the arrowhead belongs at the loop header, i.e. the top.
# graph-easy offers no knob for this (`dir=back` is a no-op on a self-loop, ports
# are unsupported, `dir=both` double-heads), so we swap the arrowhead to the top
# of the bracket in its boxart output. Only blocks the CFG marks as self-looping
# are touched, and only when the bracket is located with confidence (both of its
# corners inside the block's box); otherwise the output is left unchanged, so
# there is no regression. Cross-block back-edges are normal edges and already
# point at their target, so they are not touched.
#
# This whole section is a graph-easy workaround; it is removed when B49 (a
# pure-Python renderer with instruction-precise edge ports) replaces graph-easy.

_VBAR = "│"                                   # │
_TOP_L, _BOT_L = "┌╭", "└╰"    # ┌╭ , └╰  (left-side bracket)
_TOP_R, _BOT_R = "┐╮", "┘╯"    # ┐╮ , ┘╯  (right-side bracket)
_ARROW_L, _ARROW_R = "▶", "◀"            # ▶ (points right, into box) , ◀
_HDASH = "─"                                  # ─
# characters the bracket is allowed to overwrite (its own glyphs + blanks)
_BRACKET_CHARS = set(" ─│┌└┐┘╭╮╰╯▶◀")


def _self_loop_ids(cfg) -> set:
    return {bb.id for bb in cfg.blocks for e in bb.successors
            if e.kind == EdgeKind.BackEdge and e.target_id == bb.id}


def _grid_char(lines: list, i: int, c: int) -> str:
    return lines[i][c] if 0 <= i < len(lines) and 0 <= c < len(lines[i]) else " "


def _box_rows(lines: list, r0: int, border_col: int) -> tuple:
    """Top and bottom row of the box whose side border sits at `border_col`."""
    top = r0
    while top - 1 >= 0 and _grid_char(lines, top - 1, border_col) in "│╭┌╮┐":
        top -= 1
        if _grid_char(lines, top, border_col) in "╭┌╮┐":
            break
    bot = r0
    while bot + 1 < len(lines) and _grid_char(lines, bot + 1, border_col) in "│╰└╯┘":
        bot += 1
        if _grid_char(lines, bot, border_col) in "╰└╯┘":
            break
    return top, bot


def _flip_block_self_loop(lines: list, bid: int, start_off: int) -> None:
    hdr = re.compile(rf"(?<![0-9A-Za-z])B{bid}(?![0-9A-Za-z])")
    r0 = col = None
    for i, row in enumerate(lines):
        m = hdr.search("".join(row))
        if m and _VBAR in row:
            r0, col = i, m.start()
            break
    if r0 is None:
        return
    bars = [j for j, ch in enumerate(lines[r0]) if ch == _VBAR]
    left = [b for b in bars if b < col]
    right = [b for b in bars if b > col]
    if not left or not right:
        return
    lb, rb = max(left), min(right)
    top, bot = _box_rows(lines, r0, lb)
    entry_tag = f"0x{start_off:04x}"

    # Try a bracket on the left margin, then the right margin.
    for arrow, top_c, bot_c, cols in (
        (_ARROW_L, _TOP_L, _BOT_L, range(lb - 1, max(-1, lb - 10), -1)),
        (_ARROW_R, _TOP_R, _BOT_R, range(rb + 1, rb + 10)),
    ):
        # 1) find the arrowhead inside the block's rows
        ar = ac = None
        for i in range(top, bot + 1):
            for c in cols:
                if _grid_char(lines, i, c) == arrow:
                    ar, ac = i, c
                    break
            if ar is not None:
                break
        if ar is None:
            continue
        # 2) the corner on the arrowhead's row, then its matching corner up/down
        cc = next((c for c in cols if _grid_char(lines, ar, c) in top_c + bot_c), None)
        if cc is None:
            continue
        step = -1 if _grid_char(lines, ar, cc) in bot_c else 1
        i = ar + step
        while 0 <= i < len(lines) and _grid_char(lines, i, cc) == _VBAR:
            i += step
        if not (0 <= i < len(lines) and _grid_char(lines, i, cc) in top_c + bot_c):
            continue
        other = i
        # 3) both corners inside the box => a genuine self-loop (not an edge
        #    arriving from another block, whose far corner is outside the box)
        if not (top <= other <= bot):
            continue
        tr, br = min(ar, other), max(ar, other)

        # 4) the arrowhead belongs at the jump target = the block's entry
        #    instruction (start_off). Use that row when it is at/above the
        #    bracket top with a clear span to it; else fall back to the top.
        lo, hi = min(ac, cc), max(ac, cc)
        target = tr
        er = next((r for r in range(top, bot + 1)
                   if entry_tag in "".join(lines[r])), None)
        if er is not None and er < tr and all(
            _grid_char(lines, r, c) in _BRACKET_CHARS
            for r in range(er, tr) for c in range(lo, hi + 1)
        ):
            target = er
        if not all(_grid_char(lines, r, c) in _BRACKET_CHARS
                   for r in range(target, br + 1) for c in range(lo, hi + 1)):
            continue   # unexpected content in the bracket span: leave untouched

        # 5) redraw: corner + arrowhead at `target`, vertical run down to `br`,
        #    plain corner (no arrowhead) at the bottom.
        top_corner, bot_corner = top_c[0], bot_c[0]
        for r in range(target, br + 1):
            if len(lines[r]) < hi + 1:   # right-margin rows can be shorter
                lines[r].extend(" " * (hi + 1 - len(lines[r])))
            for c in range(lo, hi + 1):
                if r == target:
                    lines[r][c] = top_corner if c == cc else (arrow if c == ac else _HDASH)
                elif r == br:
                    lines[r][c] = bot_corner if c == cc else _HDASH
                else:
                    lines[r][c] = _VBAR if c == cc else " "
        return


def _flip_self_loop_arrows(text: str, cfg) -> str:
    """Rewrite graph-easy boxart so a self-loop arrowhead points at the block's
    entry (the jump target / loop header) instead of the latch at the bottom.
    A no-op when there are no self-loops or a bracket cannot be located."""
    ids = _self_loop_ids(cfg)
    if not ids:
        return text
    starts = {bb.id: bb.start_offset for bb in cfg.blocks}
    lines = [list(l) for l in text.split("\n")]
    for bid in ids:
        try:
            _flip_block_self_loop(lines, bid, starts[bid])
        except Exception:
            pass   # never let a rendering tweak break the output
    return "\n".join("".join(r) for r in lines)


# ── Rendering via graph-easy ─────────────────────────────────────────────────

def _find_graph_easy() -> tuple[str, dict] | None:
    """Return (path, env) for graph-easy, or None if not found."""
    candidates = [
        shutil.which("graph-easy"),
        os.path.expanduser("~/perl5/bin/graph-easy"),
    ]
    for path in candidates:
        if path and os.path.isfile(path):
            env = os.environ.copy()
            perl5 = os.path.expanduser("~/perl5/lib/perl5")
            existing = env.get("PERL5LIB", "")
            env["PERL5LIB"] = f"{perl5}:{existing}" if existing else perl5
            return path, env
    return None


def render_cfg(cfg, func, *, force_compact: bool | None = None) -> str:
    """graph-easy ASCII art of the CFG. `force_compact`: None = compact above
    _COMPACT_THRESHOLD instructions, True = always, False = never (can crash
    graph-easy). Raises RuntimeError if graph-easy isn't installed
    (`cpanm Graph::Easy`)."""
    found = _find_graph_easy()
    if found is None:
        raise RuntimeError(
            "graph-easy not found - the `cfg` command requires it to render "
            "the control flow graph. Install with: cpanm Graph::Easy "
            "(a Perl module)."
        )

    total_instrs = sum(len(bb.instructions) for bb in cfg.blocks)
    if force_compact is None:
        compact = total_instrs > _COMPACT_THRESHOLD
    else:
        compact = force_compact

    dot = cfg_to_dot(cfg, func, compact=compact)

    compact_note = ""
    if compact:
        compact_note = (
            f"\x1b[33m[compact view - {total_instrs} instrs in {len(cfg.blocks)} blocks,"
            f" showing first {_COMPACT_HEAD}+last {_COMPACT_TAIL} per block]\x1b[0m\n"
        )

    ge, env = found
    try:
        result = subprocess.run(
            [ge, "--from=graphviz", "--as=boxart"],
            input=dot,
            capture_output=True,
            text=True,
            timeout=15,
            env=env,
        )
        if result.returncode != 0 or not result.stdout.strip():
            err = result.stderr.strip()
            return f"{compact_note}\x1b[31m[graph-easy error]\x1b[0m {err}\n\n{dot}"
        fixed = _flip_self_loop_arrows(result.stdout, cfg)
        return compact_note + _colorize(fixed)
    except subprocess.TimeoutExpired:
        return f"{compact_note}\x1b[31m[graph-easy timed out]\x1b[0m\n\n{dot}"
    except Exception as exc:
        return f"{compact_note}\x1b[31m[graph-easy failed: {exc}]\x1b[0m\n\n{dot}"


def show_cfg(cfg, func, *, force_compact: bool | None = None) -> None:
    """Render CFG via graph-easy and display in pager."""
    header = (
        f"\x1b[1;37mCFG\x1b[0m  \x1b[1;36m{func.name}\x1b[0m"
        f"  \x1b[2m(func {func.index})\x1b[0m"
    )
    if cfg.has_loops:
        header += "  \x1b[33m[loops]\x1b[0m"
    header += "\n"

    body = render_cfg(cfg, func, force_compact=force_compact)
    _page(header + "\n" + body)


def _page(text: str) -> None:
    """Shows `text` in `less -R -S` (plain print if less is missing)."""
    try:
        pager = subprocess.Popen(
            ["less", "-R", "-S", "--quit-if-one-screen"],
            stdin=subprocess.PIPE,
            text=True,
        )
        pager.communicate(text)
    except (BrokenPipeError, FileNotFoundError):
        print(text)


# ── Metro module graph (cfg module:<id>) ────────────────────────────────────

# Above this many direct dependencies the graph shows the first ones and a
# "+N more" node (graph-easy gets slow on very wide graphs; ridemovi has a
# module with 287 dependencies).
_MODULE_DEPS_THRESHOLD = 40


def _dot_str(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', r'\"')


def _module_label(mod_id: int, mod, funcs: list) -> str:
    if mod is None:
        return f"#{mod_id}\\lnot defined\\l"
    name = funcs[mod.function_index].name if mod.function_index < len(funcs) else ""
    return _dot_str(f"#{mod_id}") + r"\l" + _dot_str(f"[{mod.function_index}] {name}") + r"\l"


def module_graph_to_dot(mod, mods: list, funcs: list, entries: list, *,
                        compact: bool = False) -> str:
    """DOT digraph of `mod` and its direct dependencies (one edge per
    dependencyMap entry, labelled with its index). `null` entries become a
    dashed "null" node, ids not defined in the bundle a dashed red node."""
    by_id = {m.module_id: m for m in mods}
    deps = list(enumerate(mod.dependencies))
    hidden = 0
    if compact and len(deps) > _MODULE_DEPS_THRESHOLD:
        hidden = len(deps) - _MODULE_DEPS_THRESHOLD
        deps = deps[:_MODULE_DEPS_THRESHOLD]

    center = f"M{mod.module_id}"
    lines = [
        f'digraph "module_{mod.module_id}" {{',
        "  graph [rankdir=LR fontname=Monospace];",
        "  node  [shape=box fontname=Monospace style=rounded];",
        "  edge  [fontname=Monospace fontsize=10];",
        "",
    ]
    tag = "  [entry]" if mod.module_id in entries else ""
    label = _module_label(mod.module_id, mod, funcs).replace(r"\l", tag + r"\l", 1)
    lines.append(f'  {center} [label="{label}" style="rounded,filled" fillcolor="#d0e8ff"];')

    seen: set[str] = {center}
    for i, d in deps:
        if d is None:
            node = f"N{i}"
            lines.append(f'  {node} [label="null" style=dashed];')
        else:
            node = f"M{d}"
            if node not in seen:
                target = by_id.get(d)
                attrs = f'label="{_module_label(d, target, funcs)}"'
                if target is None:
                    attrs += ' style=dashed color="red"'
                lines.append(f"  {node} [{attrs}];")
        seen.add(node)
        lines.append(f'  {center} -> {node} [label="{i}"];')
    if hidden:
        lines.append(f'  MORE [label="+{hidden} more\\l(modules module:{mod.module_id})\\l" style=dashed];')
        lines.append(f"  {center} -> MORE [style=dashed];")
    lines.append("}")
    return "\n".join(lines)


def show_module_graph(mod, mods: list, funcs: list, entries: list, *,
                      force_compact: bool | None = None) -> None:
    """`cfg module:<id>`: the module and its direct dependencies, via graph-easy
    in the pager. `force_compact` as in render_cfg()."""
    found = _find_graph_easy()
    if found is None:
        raise RuntimeError(
            "graph-easy not found - `cfg module:` requires it to render the "
            "module graph. Install with: cpanm Graph::Easy (a Perl module)."
        )
    n = len(mod.dependencies)
    compact = n > _MODULE_DEPS_THRESHOLD if force_compact is None else force_compact
    dot = module_graph_to_dot(mod, mods, funcs, entries, compact=compact)

    header = (f"\x1b[1;37mMODULE\x1b[0m  \x1b[1;36m#{mod.module_id}\x1b[0m"
              f"  \x1b[2m(factory func {mod.function_index}, {n} dependencies)\x1b[0m\n")
    if compact and n > _MODULE_DEPS_THRESHOLD:
        header += (f"\x1b[33m[compact view - showing {_MODULE_DEPS_THRESHOLD} of {n} "
                   f"dependencies, --compact=false for all]\x1b[0m\n")
    if not mod.dependencies_known:
        header += "\x1b[31m[dependencyMap is not an array literal: dependencies unknown]\x1b[0m\n"

    ge, env = found
    try:
        result = subprocess.run([ge, "--from=graphviz", "--as=boxart"], input=dot,
                                capture_output=True, text=True, timeout=30, env=env)
        if result.returncode != 0 or not result.stdout.strip():
            body = f"\x1b[31m[graph-easy error]\x1b[0m {result.stderr.strip()}\n\n{dot}"
        else:
            body = _colorize_modules(result.stdout)
    except subprocess.TimeoutExpired:
        body = f"\x1b[31m[graph-easy timed out]\x1b[0m\n\n{dot}"
    _page(header + "\n" + body)


def _colorize_modules(text: str) -> str:
    out = []
    for line in text.split("\n"):
        line = re.sub(r"#\d+", lambda m: f"{_BOLD}{m.group()}{_R}", line)
        line = re.sub(r"\[\d+\]", lambda m: f"{_DIM}{m.group()}{_R}", line)
        line = line.replace("[entry]", f"{_CYAN}[entry]{_R}")
        line = re.sub(r"[◀▼▶▲]", lambda m: f"{_DIM}{m.group()}{_R}", line)
        out.append(line)
    return "\n".join(out)
