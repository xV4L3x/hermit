"""cli/viewer.py - inline scrollable TUI list with real-time search."""

import re
import shutil

from prompt_toolkit import Application
from prompt_toolkit.filters import Condition
from prompt_toolkit.formatted_text import ANSI
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.layout import Layout
from prompt_toolkit.layout.containers import HSplit, Window
from prompt_toolkit.layout.controls import FormattedTextControl


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def _strip_ansi(s: str) -> str:
    return _ANSI_RE.sub("", s)


def _truncate_ansi(s: str, width: int) -> str:
    """Truncate `s` to `width` VISIBLE characters without splitting an ANSI
    sequence: a cut `\\x1b[..m` makes the `ANSI()` parser swallow the newline and
    merge two lines."""
    width = max(1, width)
    out: list[str] = []
    visible = 0
    i, n = 0, len(s)
    while i < n:
        m = _ANSI_RE.match(s, i)
        if m:                       # ANSI sequence: copy it whole, width 0
            out.append(m.group())
            i = m.end()
            continue
        if visible >= width:
            break
        out.append(s[i])
        visible += 1
        i += 1
    if visible >= width:
        out.append("\x1b[0m")       # close any style left open
    return "".join(out)


def show_list(
    rows: list[str],
    title: str = "",
    alt_rows: list[str] | None = None,
    alt_title: str = "",
    *,
    _input=None,
    _output=None,
) -> None:
    """Inline scrollable TUI sub-window with real-time search.

    Nav:    up down PgUp PgDn Home End  |  g/G top/bottom  |  q quit
    Search: /  enter search mode, typing filters the list in real time
            Esc  clears the query and returns to nav mode
            Backspace  deletes the last character
            Enter  confirms the search (returns to nav, keeps the filter)
    Toggle: s  switches between the main view and the alternate view (if given)

    `_input`/`_output`: headless tests only (piped input + `DummyOutput()`),
    same as `run_editor` in `cli/editor/`.
    """
    if not rows:
        return

    term = shutil.get_terminal_size()
    cols = term.columns
    view_height = max(5, min(20, term.lines // 2))

    mode    = ["nav"]    # "nav" | "search"
    query   = [""]
    offset  = [0]
    alt_on  = [False]    # True -> show alt_rows instead of rows

    has_alt = bool(alt_rows)

    # Helpers

    def _current_rows() -> list[str]:
        return (alt_rows or []) if alt_on[0] else rows

    def _current_title() -> str:
        return alt_title if alt_on[0] else title

    def _filtered() -> list[str]:
        q = query[0].lower()
        cr = _current_rows()
        return cr if not q else [r for r in cr if q in _strip_ansi(r).lower()]

    def _clamp_offset(filtered: list[str]) -> None:
        max_off = max(0, len(filtered) - view_height)
        if offset[0] > max_off:
            offset[0] = max_off

    # Window content

    def _main():
        filtered = _filtered()
        visible  = filtered[offset[0]: offset[0] + view_height]
        lines    = [_truncate_ansi(line, cols) for line in visible]
        while len(lines) < view_height:          # pad with empty lines
            lines.append("")
        return ANSI("\n".join(lines))

    def _status():
        filtered = _filtered()
        n        = len(filtered)
        end      = min(offset[0] + view_height, n)
        pct      = int(end / n * 100) if n else 0
        counts   = f"{offset[0]+1 if n else 0}–{end}/{n}"
        if query[0]:
            counts += f" (of {len(_current_rows())})"
        s_hint = "  s:view" if has_alt else ""
        return ANSI(
            f"\x1b[7m {_current_title()}  {counts} ({pct}%)  "
            f"↑↓ PgUp PgDn  /search  q quit{s_hint} \x1b[0m"
        )

    def _search_bar():
        if mode[0] == "search":
            return ANSI(f"\x1b[1m/\x1b[0m {query[0]}\x1b[7m \x1b[0m")
        elif query[0]:
            return ANSI(f"\x1b[2m/ {query[0]}  (Esc to clear)\x1b[0m")
        else:
            return ANSI("\x1b[2m/ to search\x1b[0m")

    # Key bindings

    kb        = KeyBindings()
    in_search = Condition(lambda: mode[0] == "search")
    in_nav    = Condition(lambda: mode[0] == "nav")
    can_alt   = Condition(lambda: has_alt and mode[0] == "nav")

    # quit
    @kb.add("q", filter=in_nav)
    @kb.add("c-c")
    def _quit(event):
        event.app.exit()

    # toggle between main view and alternate view
    @kb.add("s", filter=can_alt)
    def _toggle_alt(event):
        alt_on[0] = not alt_on[0]
        offset[0] = 0
        query[0]  = ""
        event.app.invalidate()

    # enter search mode
    @kb.add("/", filter=in_nav)
    def _enter_search(event):
        mode[0] = "search"
        event.app.invalidate()

    # Esc: clears the query and returns to nav mode
    @kb.add("escape")
    def _esc(event):
        mode[0] = "nav"
        if query[0]:
            query[0]  = ""
            offset[0] = 0
        event.app.invalidate()

    # Enter: confirms the search, returns to nav
    @kb.add("enter", filter=in_search)
    def _confirm(event):
        mode[0]  = "nav"
        offset[0] = 0
        event.app.invalidate()

    # type in search mode
    @kb.add("<any>", filter=in_search)
    def _type(event):
        if event.data and event.data.isprintable():
            query[0] += event.data
            offset[0] = 0
            _clamp_offset(_filtered())
            event.app.invalidate()

    @kb.add("backspace", filter=in_search)
    def _back(event):
        query[0]  = query[0][:-1]
        offset[0] = 0
        event.app.invalidate()

    # scroll (works in both modes)
    @kb.add("up")
    def _up(event):
        if offset[0] > 0:
            offset[0] -= 1
            event.app.invalidate()

    @kb.add("down")
    def _down(event):
        if offset[0] + view_height < len(_filtered()):
            offset[0] += 1
            event.app.invalidate()

    @kb.add("pageup")
    def _pgup(event):
        offset[0] = max(0, offset[0] - view_height)
        event.app.invalidate()

    @kb.add("pagedown")
    def _pgdn(event):
        filtered  = _filtered()
        offset[0] = min(max(0, len(filtered) - view_height), offset[0] + view_height)
        event.app.invalidate()

    @kb.add("home")
    @kb.add("g", filter=in_nav)
    def _home(event):
        offset[0] = 0
        event.app.invalidate()

    @kb.add("end")
    @kb.add("G", filter=in_nav)
    def _end(event):
        filtered  = _filtered()
        offset[0] = max(0, len(filtered) - view_height)
        event.app.invalidate()

    # Layout & run

    layout = Layout(
        HSplit([
            Window(FormattedTextControl(_main),       height=view_height),
            Window(FormattedTextControl(_status),     height=1),
            Window(FormattedTextControl(_search_bar), height=1),
        ])
    )

    kwargs = {}
    if _input is not None:
        kwargs["input"] = _input
    if _output is not None:
        kwargs["output"] = _output
    Application(layout=layout, key_bindings=kb, full_screen=False, **kwargs).run()
