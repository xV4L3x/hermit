"""Identifiers used by array and object literals (B3).

The VM turns each object-literal key into a symbol with `ID()`
(`getHiddenClassForBuffer` in `Interpreter.cpp`), and symbols exist only for
Identifier entries: a key that names a String-kind entry crashes the VM. A key
repeated in one literal gives a hidden class with fewer properties than values;
the values are stored by position, so they shift. Hermes stores each text once,
so a property name and a string literal with the same text (`"alpha"` in an
array) share the Identifier entry: a global rename changes both.

Guards: the array editor and `apply_array_resize` accept a new key only if it is
an Identifier not already in the literal; `apply_string_relocation` refuses a
rename that would repeat a key.

Sample: `OBJ` below, compiled at test time for each version.
"""

import subprocess

import pytest

import _libhbc
import cli.editor
from cli.editor import ArrayEditState, StringEditState, _SLP_TAG_ORDER
from cli.literals import SLPTag, decode_slp, encode_slp_sequence, reencode_value_bytes
from cli.project import HermitProject
from cli.repl import HermitREPL
from test_identifier_add import VERSIONS, _compiler

ID = _libhbc.StringKind.Identifier

OBJ = """"use strict";
function main() {
    var o = { alpha: 1, gamma: 3, zz: 5 };
    var a = ["alpha", "plain text", 7, true];
    print(JSON.stringify(o));
    print(JSON.stringify(a));
    print(o.alpha + o.gamma);
}
main();
"""
ORIG = ['{"alpha":1,"gamma":3,"zz":5}', '["alpha","plain text",7,true]', "4"]


def _compile(tmp_path, version=96):
    if _compiler(version) is None:
        pytest.skip(f"no hermes for HBC {version}")
    js = tmp_path / "objkeys.js"
    js.write_text(OBJ)
    out = tmp_path / f"objkeys_v{version}.hbc"
    r = subprocess.run([_compiler(version), "-emit-binary", "-g0", "-out", str(out), str(js)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    return out


def _run(path, version=96):
    return subprocess.run([_compiler(version), str(path)], capture_output=True,
                          text=True, timeout=30)


def _run_proj(p, tmp_path, version=96, name="out.hbc"):
    out = tmp_path / name
    p.write_patched(str(out))
    r = _run(out, version)
    assert r.returncode == 0, r.stderr
    return r.stdout.splitlines()


@pytest.fixture
def path(tmp_path):
    return _compile(tmp_path)


@pytest.fixture
def proj(path):
    return HermitProject.from_hbc(str(path))


def _idx(p, value):
    return list(p.hbc.strings()).index(value)


def _object_site(p):
    return next((i, s) for i, s in enumerate(p.hbc.literal_sites()) if s.is_object)


def _buffers_with_pair(p, key_idx, value=9):
    """Key and value buffers of the object literal with one more pair."""
    i, site = _object_site(p)
    img = p.image_bytes
    keys = decode_slp(img, p.hbc.obj_key_buffer_offset() + site.key_buffer_idx, site.count)
    vals = decode_slp(img, p.hbc.obj_value_buffer_offset() + site.buffer_idx, site.count)
    kt = SLPTag.SHORT_STRING if key_idx < 0x10000 else SLPTag.LONG_STRING
    kb = encode_slp_sequence([(k.tag, reencode_value_bytes(k.tag, k.value)) for k in keys]
                             + [(kt, reencode_value_bytes(kt, key_idx))])
    vb = encode_slp_sequence([(v.tag, reencode_value_bytes(v.tag, v.value)) for v in vals]
                             + [(SLPTag.INTEGER, reencode_value_bytes(SLPTag.INTEGER, value))])
    return i, site, vb, kb


def _add_pair(p, key_idx, value=9):
    i, site, vb, kb = _buffers_with_pair(p, key_idx, value)
    p.apply_array_resize(site.function_index, site.instruction_offset, True, True, vb, kb, i)


# --- why: the VM -------------------------------------------------------------


def test_sample_runs(path):
    assert _run(path).stdout.splitlines() == ORIG


def test_string_kind_key_crashes_the_vm(path, tmp_path):
    """The evidence for the guard: the same add through `Module` directly."""
    p = HermitProject.from_hbc(str(path))
    _, site, vb, kb = _buffers_with_pair(p, _idx(p, "plain text"))
    assert p.hbc.string_kinds()[_idx(p, "plain text")] != ID
    m = _libhbc.Module.from_hbcfile(_libhbc.HBCFile.open(str(path)))
    m.resize_literal(site.function_index, site.instruction_offset, True, True, vb, kb)
    out = tmp_path / "bad.hbc"
    out.write_bytes(bytes(m.serialize()))
    assert _run(out).returncode != 0


# --- add a key ---------------------------------------------------------------


@pytest.mark.parametrize("version", VERSIONS)
def test_add_pair_with_new_identifier_key(version, tmp_path):
    p = HermitProject.from_hbc(str(_compile(tmp_path, version)))
    _add_pair(p, p.apply_add_identifier("epsilon"))
    assert _run_proj(p, tmp_path, version) == [
        '{"alpha":1,"gamma":3,"zz":5,"epsilon":9}', ORIG[1], "4"]


def test_add_pair_with_existing_identifier_key(proj, tmp_path):
    _add_pair(proj, _idx(proj, "stringify"))
    assert _run_proj(proj, tmp_path)[0] == '{"alpha":1,"gamma":3,"zz":5,"stringify":9}'


def test_apply_array_resize_refuses_string_key(proj):
    with pytest.raises(ValueError, match="not an Identifier"):
        _add_pair(proj, _idx(proj, "plain text"))
    assert proj.is_patched is False


def test_apply_array_resize_refuses_repeated_key(proj):
    with pytest.raises(ValueError, match='already has the key "gamma"'):
        _add_pair(proj, _idx(proj, "gamma"))
    assert proj.is_patched is False


def test_editor_refuses_string_key(proj):
    i, site = _object_site(proj)
    img = proj.image_bytes
    keys = decode_slp(img, proj.hbc.obj_key_buffer_offset() + site.key_buffer_idx, site.count)
    vals = decode_slp(img, proj.hbc.obj_value_buffer_offset() + site.buffer_idx, site.count)
    st = ArrayEditState(i, site, vals, keys, proj.hbc.strings(), proj.hbc.functions(),
                        kinds=proj.hbc.string_kinds())
    st.begin_add()
    st.type_pick_sel = _SLP_TAG_ORDER.index(SLPTag.SHORT_STRING)
    st.add_type_pick_confirm()
    st.buf = str(_idx(proj, "plain text"))
    st.commit_add_field()
    assert st.mode == "add_edit"
    assert st.add_stage == "key"
    assert "not an Identifier" in st.edit_err
    st.buf = str(_idx(proj, "stringify"))
    st.commit_add_field()
    assert st.add_stage == "value"


# --- rename a key ------------------------------------------------------------


@pytest.mark.parametrize("version", VERSIONS)
def test_global_rename_of_a_key_renames_the_array_value_too(version, tmp_path):
    """`alpha` is a key, a GetById name and an array element: one entry."""
    p = HermitProject.from_hbc(str(_compile(tmp_path, version)))
    p.apply_string_relocation(_idx(p, "alpha"), b"zeta", "zeta")
    assert _run_proj(p, tmp_path, version) == [
        '{"zeta":1,"gamma":3,"zz":5}', '["zeta","plain text",7,true]', "4"]


def test_rename_to_a_key_of_the_same_literal_is_refused(proj):
    """Without the guard: `{"alpha":1,"zz":3}` (values shifted) and 2."""
    assert proj.object_key_clashes(_idx(proj, "gamma"), "alpha") == [_object_site(proj)[0]]
    with pytest.raises(ValueError, match="key twice"):
        proj.apply_string_relocation(_idx(proj, "gamma"), b"alpha", "alpha")
    assert proj.is_patched is False


def test_rename_to_a_name_outside_the_literal_is_allowed(proj, tmp_path):
    """`zz` -> `stringify`: another Identifier, not a key of this literal."""
    proj.apply_string_relocation(_idx(proj, "zz"), b"stringify", "stringify")
    assert _run_proj(proj, tmp_path)[0] == '{"alpha":1,"gamma":3,"stringify":5}'


def test_one_site_rename_leaves_the_key(proj, tmp_path):
    """Only `o.alpha` is re-pointed: the key and the array value keep `alpha`."""
    main = next(f for f in proj.hbc.functions() if f.name == "main")
    alpha = _idx(proj, "alpha")
    site = next(x for x in proj.hbc.string_xrefs() if x.string_index == alpha)
    get = next(st for st in site.sites if st.kind == _libhbc.XRefKind.Operand)
    assert get.function_index == main.index
    proj.apply_rename_site(main.index, get.instruction_offset, proj.apply_add_identifier("zeta"))
    assert _run_proj(proj, tmp_path) == [ORIG[0], ORIG[1], "NaN"]


# --- editor and REPL ---------------------------------------------------------


def _state(p, idx):
    xrefs = [x for x in p.hbc.string_xrefs() if x.string_index == idx]
    return StringEditState(idx, p.hbc.strings(), p.hbc.string_kinds(),
                           p.hbc.string_entries(), xrefs, p.hbc.functions())


def test_editor_counts_value_sites(proj):
    st = _state(proj, _idx(proj, "alpha"))
    assert st.site_count == 3                      # key + array element + GetById
    assert [s.kind for s in st.value_sites] == [_libhbc.XRefKind.ArrayElement]
    assert [s.mnemonic for s in st.operand_sites] == ["GetByIdShort"]


def test_repl_rename_clash_is_rejected(path, monkeypatch, capsys):
    r = HermitREPL()
    r._open_file(str(path))

    def fake_editor(idx, strings, kinds, entries, xrefs, funcs, **kw):
        st = StringEditState(idx, strings, kinds, entries, xrefs, funcs)
        st.value = "alpha"
        st.commit = True
        return st

    monkeypatch.setattr(cli.editor, "run_string_editor", fake_editor)
    r._edit_string(r.project, f"string:{_idx(r.project, 'gamma')}")
    assert r.project.is_patched is False
    assert "key twice" in capsys.readouterr().out
