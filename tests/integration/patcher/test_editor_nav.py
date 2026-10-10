"""read-only bytecode editor, token-level cursor.

Headless test: drives the real prompt_toolkit Application with a pipe input
(key sequence ending with `q`) and a DummyOutput, then inspects the
`EditorState` returned by run_editor().
"""


import pytest
from prompt_toolkit.input.defaults import create_pipe_input
from prompt_toolkit.output import DummyOutput

import _libhbc
from cli.editor import EditorState, run_editor
from testutil import CORPUS, DOWN, LEFT, RIGHT, UP


# VT100 escape sequences
PGUP = "\x1b[5~"
PGDN = "\x1b[6~"


def _load(name, func_pred=None):
    path = CORPUS / name
    if not path.exists():
        pytest.skip(f"missing corpus: {name} (regenerate with `python -m seeder.compile`)")
    hbc = _libhbc.HBCFile.open(str(path))
    funcs = hbc.functions()
    idx = 0
    if func_pred:
        idx = next((f.index for f in funcs if func_pred(f)), 0)
    func = funcs[idx]
    return func, hbc.disasm(func.index)


def _run_keys(func, instrs, keys):
    """Runs the headless editor with the `keys` + `q` sequence, returns the state."""
    state = EditorState(func, instrs)
    with create_pipe_input() as inp:
        inp.send_text(keys + "q")
        run_editor(func, instrs, _input=inp, _output=DummyOutput(), _state=state)
    return state


# ---------------------------------------------------------------------------


def test_state_token_layout():
    """Every nav line has tokens = [mnemonic] + operands; info lines are not nav."""
    func, instrs = _load("exceptions.hbc", lambda f: f.hasExceptionHandler)
    st = EditorState(func, instrs)
    nav_lines = [st.lines[i] for i in st.nav_indices]
    assert len(nav_lines) == len(instrs)
    for line, ins in zip(nav_lines, instrs):
        assert line.mnemonic == ins.mnemonic
        assert line.n_tokens == 1 + len(ins.operands)
    # there are informational lines (EH header) that the cursor skips
    assert len(st.nav_indices) < len(st.lines)


def test_right_advances_then_clamps():
    func, instrs = _load("control_flow.hbc", lambda f: f.name == "max")
    # line 0 = "LoadParam r0, 1" -> 3 tokens (idx 0,1,2)
    st = _run_keys(func, instrs, RIGHT * 5)
    assert st.ci == 0
    assert st.tok == 2  # clamped at the line's last token


def test_left_clamps_at_zero():
    func, instrs = _load("control_flow.hbc", lambda f: f.name == "max")
    st = _run_keys(func, instrs, RIGHT + LEFT + LEFT + LEFT)
    assert st.tok == 0


def test_down_moves_line_and_resets_token():
    func, instrs = _load("control_flow.hbc", lambda f: f.name == "max")
    st = _run_keys(func, instrs, RIGHT + RIGHT + DOWN)
    assert st.ci == 1
    assert st.tok == 0  # lands on the new line's mnemonic


def test_up_clamps_at_top():
    func, instrs = _load("control_flow.hbc", lambda f: f.name == "max")
    st = _run_keys(func, instrs, UP * 4)
    assert st.ci == 0


def test_g_and_G_jump_to_ends():
    func, instrs = _load("control_flow.hbc", lambda f: f.name == "max")
    last = len(EditorState(func, instrs).nav_indices) - 1
    assert _run_keys(func, instrs, "G").ci == last
    assert _run_keys(func, instrs, "Gg").ci == 0


def test_pagedown_pageup():
    func, instrs = _load("ridemovi.hbc") if (CORPUS / "ridemovi.hbc").exists() \
        else _load("exceptions.hbc", lambda f: f.hasExceptionHandler)
    n = len(EditorState(func, instrs).nav_indices)
    st = _run_keys(func, instrs, PGDN)
    assert st.ci > 0 or n == 1
    st2 = _run_keys(func, instrs, PGDN + PGUP)
    assert st2.ci == 0


def test_exception_lines_never_selected():
    func, instrs = _load("exceptions.hbc", lambda f: f.hasExceptionHandler)
    st = _run_keys(func, instrs, DOWN * 200)  # well past the end
    assert st.ci == len(st.nav_indices) - 1
    assert st.cur_line.nav is True  # never landed on an info line


def test_switchimm_walks_clean():
    """SwitchImm: 5 operands, attached jump table already skipped by the disasm."""
    from cli.editor.instructions import _line_fragments
    func, instrs = _load("switch.hbc", lambda f: f.index != 0 and not f.name.startswith("anon"))
    st = EditorState(func, instrs)
    assert len(st.nav_indices) == len(instrs)
    sw = [ln for ln in st.lines if ln.nav and ln.mnemonic == "SwitchImm"]
    if sw:
        assert sw[0].n_tokens == 1 + 5
    # no exception while walking every token
    for i in st.nav_indices:
        ln = st.lines[i]
        for tk in range(ln.n_tokens):
            st.tok = tk
            _line_fragments(st, ln, True)


def test_hello_minimal():
    func, instrs = _load("hello.hbc")
    st = _run_keys(func, instrs, DOWN + DOWN + RIGHT)
    assert st.ci == 2
    assert st.tok == 1
