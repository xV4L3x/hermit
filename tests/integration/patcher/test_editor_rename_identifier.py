"""Global rename of an Identifier from the REPL (B3 M5, `edit string:<idx>`).

The string editor opens on an Identifier with a rename banner and the site
count; the commit always goes through `HermitProject.apply_string_relocation`
(also at unchanged length), which rewrites the Identifier's hash. The prompt
warns when the new name equals another Identifier: the two become the same
property.

Sample: `seeder/samples/identifiers.js`, compiled at test time.
"""

import subprocess

import pytest

import _libhbc
import cli.editor
from cli.editor import StringEditState, _StrEditError, encode_string_commit
from cli.repl import HermitREPL
from test_editor_add_identifier import proj, sample  # noqa: F401 (fixtures)
from test_identifier_add import EXPECTED
from testutil import CX, HERMES, drive_strings

ID = _libhbc.StringKind.Identifier
BS = "\x7f"


def _idx(p, value):
    return list(p.hbc.strings()).index(value)


def _state(p, idx):
    xrefs = [x for x in p.hbc.string_xrefs() if x.string_index == idx]
    return StringEditState(idx, p.hbc.strings(), p.hbc.string_kinds(),
                           p.hbc.string_entries(), xrefs, p.hbc.functions())


def _repl_rename(sample_path, old, new, monkeypatch):
    """Runs `edit string:<old>` with the editor replaced by a committed state."""
    r = HermitREPL()
    r._open_file(str(sample_path))

    def fake_editor(idx, strings, kinds, entries, xrefs, funcs, **kw):
        st = StringEditState(idx, strings, kinds, entries, xrefs, funcs,
                             is_shared=kw.get("is_shared", False),
                             shared_with=kw.get("shared_with", ()))
        st.value = new
        st.commit = True
        return st

    monkeypatch.setattr(cli.editor, "run_string_editor", fake_editor)
    r._edit_string(r.project, f"string:{_idx(r.project, old)}")
    return r.project


# --- editor state ------------------------------------------------------------


def test_state_on_identifier(proj):
    st = _state(proj, _idx(proj, "alpha"))
    assert st.is_identifier is True
    assert st.adds_identifier is False
    # literal key + GetByIdShort in `read` + GetByIdShort in `main`
    assert st.site_count >= 3


def test_state_merges_with(proj):
    st = _state(proj, _idx(proj, "delta"))
    st.value = "gamma"
    assert st.merges_with() == _idx(proj, "gamma")
    st.value = "zeta"
    assert st.merges_with() is None


def test_state_on_string_is_not_identifier(proj):
    st = _state(proj, _idx(proj, "beta value"))
    assert st.is_identifier is False
    st.value = "alpha"
    assert st.merges_with() is None


def test_editor_rename_commits(proj):
    st = _state(proj, _idx(proj, "alpha"))
    drive_strings(st, BS * 5 + "zeta" + CX + "y")
    assert st.commit is True
    assert st.value == "zeta"


def test_same_length_encoder_refuses_identifier(proj):
    st = _state(proj, _idx(proj, "alpha"))
    st.value = "omega"
    with pytest.raises(_StrEditError, match="hash stale"):
        encode_string_commit(st)


# --- REPL commit ---------------------------------------------------------------


def test_repl_rename_rewrites_hash_same_length(sample, monkeypatch):
    """Same length ("alpha" -> "omega") still relocates: the hash changes."""
    p = _repl_rename(sample, "alpha", "omega", monkeypatch)
    assert p.is_patched is True
    idx = _idx(p, "omega")
    assert p.hbc.string_kinds()[idx] == ID
    data = p.image_bytes
    recomputed = [
        _libhbc.identifier_hash(data[e.storage_offset:e.storage_offset + e.byte_length],
                                e.is_utf16)
        for e in p.hbc.string_entries() if e.kind == ID
    ]
    assert list(p.hbc.identifier_hashes()) == recomputed
    assert _libhbc.identifier_hash(b"omega") in recomputed


@pytest.mark.parametrize("old,new,expected", [
    ("alpha", "zeta", EXPECTED),
    ("counter", "kount", EXPECTED),
    ("delta", "gamma", ["1", "1", "8", "7", "beta value"]),   # merges with gamma
])
def test_repl_rename_runs_on_vm(sample, monkeypatch, tmp_path, old, new, expected):
    p = _repl_rename(sample, old, new, monkeypatch)
    assert p.is_patched is True
    out = tmp_path / "renamed.hbc"
    p.write_patched(str(out))
    r = subprocess.run([HERMES, str(out)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines() == expected


def test_repl_rename_unchanged_value_is_noop(sample, monkeypatch):
    p = _repl_rename(sample, "alpha", "alpha", monkeypatch)
    assert p.is_patched is False


def test_editor_rename_to_existing_name_commits(proj):
    """The merge warning is shown in the prompt; Y still renames."""
    st = _state(proj, _idx(proj, "delta"))
    drive_strings(st, BS * 5 + "gamma" + CX + "y")
    assert st.commit is True
    assert st.merges_with() == _idx(proj, "gamma")
