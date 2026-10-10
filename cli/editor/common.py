"""Shared pieces of the three full-screen editors (instructions, strings,
array/object literals): styles, text buffer, prompts and the screen runner."""

from __future__ import annotations

import shutil

from prompt_toolkit import Application
from prompt_toolkit.filters import Condition
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.layout import Layout
from prompt_toolkit.layout.containers import HSplit, Window
from prompt_toolkit.layout.controls import FormattedTextControl

GUTTER_STYLE = "fg:ansibrightblack"
MNEMONIC_STYLE = "bold fg:ansiyellow"
INFO_STYLE = "fg:ansibrightblack"
CURSOR_STYLE = "reverse"
ERR_STYLE = "fg:ansired"
HINT_STYLE = "fg:ansibrightblack"
SEL_STYLE = "reverse"
EMPTY_STYLE = "bold fg:ansired"
DIRTY_MARK_STYLE = "bold fg:ansiyellow"      # modified line (not an error)
INCOMPLETE_MARK_STYLE = "bold fg:ansired"    # line with empty slots to fill in


def fmt_num(x: float) -> str:
    return str(int(x)) if float(x).is_integer() else repr(x)


def strip_kind_suffix(s: str) -> str:
    """Drops the " [identifier]" / " [string]" suffix of a resolved string."""
    for suffix in (" [identifier]", " [string]"):
        if s.endswith(suffix):
            return s[: -len(suffix)]
    return s


def view_height() -> int:
    return max(3, shutil.get_terminal_size().lines - 4)


class TextBufferMixin:
    """Single-line text buffer (`buf`, cursor `cur`) with the usual editing
    keys. Any edit clears `edit_err`."""

    buf: str
    cur: int
    edit_err: str

    def edit_insert(self, ch: str):
        self.buf = self.buf[:self.cur] + ch + self.buf[self.cur:]
        self.cur += len(ch)
        self.edit_err = ""

    def edit_backspace(self):
        if self.cur > 0:
            self.buf = self.buf[:self.cur - 1] + self.buf[self.cur:]
            self.cur -= 1
            self.edit_err = ""

    def edit_delete(self):
        if self.cur < len(self.buf):
            self.buf = self.buf[:self.cur] + self.buf[self.cur + 1:]
            self.edit_err = ""

    def edit_move(self, delta: int):
        self.cur = max(0, min(len(self.buf), self.cur + delta))

    def edit_home(self):
        self.cur = 0

    def edit_end(self):
        self.cur = len(self.buf)


def input_bar(label: str, buf: str, cur: int):
    """Fragments of a one-line input with a block cursor."""
    cur_ch = buf[cur:cur + 1] or " "
    return [(HINT_STYLE, label), ("", buf[:cur]), (CURSOR_STYLE, cur_ch), ("", buf[cur + 1:])]


def apply_discard_cancel_bar(prompt: str):
    """Fragments of the Y / N / Esc prompt shared by the string and array editors."""
    return [
        (DIRTY_MARK_STYLE, prompt),
        ("bold", " Y "), (HINT_STYLE, "apply  "),
        ("bold", " N "), (HINT_STYLE, "discard  "),
        ("bold", " Esc "), (HINT_STYLE, "cancel "),
    ]


def bind_text_input(kb: KeyBindings, state, active: Condition) -> None:
    """Left/Right/Home/End/Backspace/Delete and typing on `state` (a
    TextBufferMixin) while `active`."""
    def edit(method):
        def handler(event):
            getattr(state, method[0])(*method[1:])
            event.app.invalidate()
        return handler

    kb.add("left", filter=active)(edit(("edit_move", -1)))
    kb.add("right", filter=active)(edit(("edit_move", +1)))
    kb.add("home", filter=active)(edit(("edit_home",)))
    kb.add("end", filter=active)(edit(("edit_end",)))
    kb.add("backspace", filter=active)(edit(("edit_backspace",)))
    kb.add("delete", filter=active)(edit(("edit_delete",)))

    @kb.add("<any>", filter=active)
    def _typed(event):
        if event.data and event.data.isprintable():
            state.edit_insert(event.data)
            event.app.invalidate()


def bind_exit_prompt(kb: KeyBindings, state, in_ask: Condition, cancel_mode: str) -> None:
    """Y commits and exits, N discards and exits, Esc goes back to `cancel_mode`."""
    @kb.add("y", filter=in_ask)
    @kb.add("Y", filter=in_ask)
    def _yes(event):
        state.commit = True
        event.app.exit()

    @kb.add("n", filter=in_ask)
    @kb.add("N", filter=in_ask)
    def _no(event):
        state.commit = False
        event.app.exit()

    @kb.add("escape", filter=in_ask)
    def _cancel(event):
        state.mode = cancel_mode
        state.edit_err = ""
        event.app.invalidate()


def bind_abort(kb: KeyBindings, state) -> None:
    """Ctrl+C: always exits without committing."""
    @kb.add("c-c")
    def _cc(event):
        state.commit = False
        event.app.exit()


def run_screen(title, body, status, bar1, bar2, kb: KeyBindings, *, _input=None, _output=None) -> None:
    """Runs the full-screen layout shared by the editors: title, body, status
    line and two bars. `_input`/`_output` are for headless tests only."""
    layout = Layout(HSplit([
        Window(FormattedTextControl(title), height=1),
        Window(FormattedTextControl(body), wrap_lines=False),
        Window(FormattedTextControl(status), height=1),
        Window(FormattedTextControl(bar1), height=1),
        Window(FormattedTextControl(bar2), height=1),
    ]))
    kwargs = {"layout": layout, "key_bindings": kb, "full_screen": True}
    if _input is not None:
        kwargs["input"] = _input
    if _output is not None:
        kwargs["output"] = _output
    Application(**kwargs).run()
