"""Helpers shared by the integration tests: corpus loading, the `hermes` VM,
terminal key codes and headless drivers for the three editors."""

import re
import shutil
import subprocess
from pathlib import Path

import pytest
from prompt_toolkit.input.defaults import create_pipe_input
from prompt_toolkit.output import DummyOutput

from cli.editor import run_array_editor, run_editor, run_string_editor
from cli.project import HermitProject

CORPUS = Path(__file__).resolve().parents[2] / "seeder" / "corpus"

# Keys as a terminal sends them.
UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
ENTER, ESC = "\r", "\x1b"
CC, CK, CN, CR, CT, CX, CZ = "\x03", "\x0b", "\x0e", "\x12", "\x14", "\x18", "\x1a"

# The Hermes VM (HBC 96) used to run patched files; None if not installed.
HERMES = shutil.which("hermes") or (
    "/usr/local/bin/hermes" if Path("/usr/local/bin/hermes").exists() else None
)

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def strip_ansi(s: str) -> str:
    return _ANSI_RE.sub("", s)


def corpus_path(name: str) -> Path:
    """`seeder/corpus/<name>`; skips the test if the corpus file isn't generated."""
    path = CORPUS / name
    if not path.exists():
        pytest.skip(f"missing corpus: {name} - regenerate it with `python -m seeder.compile`")
    return path


def load_project(name: str) -> HermitProject:
    return HermitProject.from_hbc(str(corpus_path(name)))


def run_hermes(path):
    """Runs a .hbc on the Hermes VM and returns the CompletedProcess."""
    return subprocess.run([HERMES, str(path)], capture_output=True, text=True, timeout=30)


def drive_instructions(state, keys):
    """Feeds `keys` (then Ctrl+C as a safety net) to the instruction editor."""
    with create_pipe_input() as inp:
        inp.send_text(keys + CC)
        run_editor(state.func, [], _input=inp, _output=DummyOutput(), _state=state)
    return state


def drive_strings(state, keys):
    with create_pipe_input() as inp:
        inp.send_text(keys + CC)
        run_string_editor(_state=state, _input=inp, _output=DummyOutput())
    return state


def drive_arrays(state, keys):
    with create_pipe_input() as inp:
        inp.send_text(keys + CC)
        run_array_editor(_state=state, _input=inp, _output=DummyOutput())
    return state
