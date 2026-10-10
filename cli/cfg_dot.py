"""cfg_dot.py - CFG -> DOT -> graph-easy terminal rendering."""

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
        return compact_note + _colorize(result.stdout)
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
    text = header + "\n" + body

    try:
        pager = subprocess.Popen(
            ["less", "-R", "-S", "--quit-if-one-screen"],
            stdin=subprocess.PIPE,
            text=True,
        )
        pager.communicate(text)
    except (BrokenPipeError, FileNotFoundError):
        print(text)
