"""One-site rename of an Identifier (B3 M6).

From the rename prompt of `edit string:<idx>` (`O`, then a site): the new name
is added as an Identifier (or the existing one is reused), then the name operand
of that ONE instruction is re-pointed (`HermitProject.apply_rename_site`). When
the new index doesn't fit the operand, the opcode is widened (`GetByIdShort` ->
`GetById` -> `GetByIdLong`) and the body goes through `Module`, which relocates
the jumps. The re-pointed site gets property-cache index 0: the compiler gives
all the sites of one name the same cache slot, so a re-pointed site keeping it
would hit the old name's entry. Shared bytecode is refused.

Samples: `seeder/samples/identifiers.js` (compiled at test time) and variants
with more than 256 strings, so that a new string index needs a wider operand.
"""

import subprocess
from pathlib import Path

import pytest

import _libhbc
import cli.editor
from cli.editor import StringEditState
from cli.project import IDENTIFIER_OPCODES, HermitProject
from cli.repl import HermitREPL
from test_editor_add_identifier import proj, sample  # noqa: F401 (fixtures)
from test_identifier_add import SAMPLE
from testutil import CX, DOWN, ENTER, ESC, HERMES, drive_strings

ID = _libhbc.StringKind.Identifier
BS = "\x7f"
JUNK = "var junk = [" + ",".join(f'"s{i}"' for i in range(300)) + "];\n"
# A function with branches around the property reads, so that widening one
# instruction moves the jump targets.
BRANCHY = """
function pick(o, n) {
    var t = 0;
    for (var i = 0; i < n; i++) {
        if (o.alpha > i) { t += o.alpha; } else { t -= 1; }
    }
    return t;
}
print(pick({alpha: 2}, 4));
"""


def _compile(src: str, tmp_path: Path, name: str) -> Path:
    if HERMES is None:
        pytest.skip("hermes not available")
    js = tmp_path / f"{name}.js"
    js.write_text(src)
    out = tmp_path / f"{name}.hbc"
    r = subprocess.run([HERMES, "-emit-binary", "-g0", "-out", str(out), str(js)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    return out


def _run(p: HermitProject, tmp_path: Path, name="out.hbc"):
    out = tmp_path / name
    p.write_patched(str(out))
    r = subprocess.run([HERMES, str(out)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    return r.stdout.splitlines()


def _sites(p, func_name, mnemonic, name):
    f = next(f for f in p.hbc.functions() if f.name == func_name)
    idx = list(p.hbc.strings()).index(name)
    mask = {d.mnemonic: d.string_ref_mask for d in _libhbc.opcode_table(p.hbc.version())}
    out = []
    for ins in p.hbc.disasm(f.index):
        if ins.mnemonic != mnemonic:
            continue
        pos = next(i for i in range(len(ins.operands)) if (mask[ins.mnemonic] >> i) & 1)
        if ins.operands[pos].u32 == idx:
            out.append(ins)
    return f, out


# --- apply_rename_site ---------------------------------------------------------


def test_identifier_opcodes_cover_the_by_id_family():
    table = {d.mnemonic for d in _libhbc.opcode_table(96)}
    assert IDENTIFIER_OPCODES <= table
    assert "LoadConstString" not in IDENTIFIER_OPCODES
    assert "CreateRegExp" not in IDENTIFIER_OPCODES


def test_rename_site_same_width(proj, tmp_path):
    """`o.alpha` in main reads `o.zeta`; `read(o)` still reads `o.alpha`."""
    new_idx = proj.apply_add_identifier("zeta")
    f, (site,) = _sites(proj, "main", "GetByIdShort", "alpha")
    assert proj.apply_rename_site(f.index, site.offset, new_idx) == "GetByIdShort"
    after = next(i for i in proj.hbc.disasm(f.index) if i.offset == site.offset)
    assert after.operands[3].u32 == new_idx
    assert after.operands[2].u32 == 0                    # property cache disabled
    assert _run(proj, tmp_path) == ["1", "undefined", "7", "7", "beta value"]
    _, kept = _sites(proj, "read", "GetByIdShort", "alpha")
    assert len(kept) == 1


def test_rename_site_widens_short_to_base(tmp_path):
    path = _compile(SAMPLE.read_text().replace('"use strict";\n', '"use strict";\n' + JUNK),
                    tmp_path, "big")
    p = HermitProject.from_hbc(str(path))
    new_idx = p.apply_add_identifier("zeta")
    assert new_idx > 255
    f, (site,) = _sites(p, "main", "GetByIdShort", "alpha")
    size0 = f.bytecodeSizeInBytes
    assert p.apply_rename_site(f.index, site.offset, new_idx) == "GetById"
    f1 = p.hbc.functions()[f.index]
    assert f1.bytecodeSizeInBytes == size0 + 1          # UInt8 -> UInt16
    assert _run(p, tmp_path) == ["1", "undefined", "7", "7", "beta value"]


def test_rename_site_widening_relocates_jumps(tmp_path):
    path = _compile(JUNK + BRANCHY, tmp_path, "branchy")
    p = HermitProject.from_hbc(str(path))
    assert _run(p, tmp_path, "orig.hbc") == ["2"]          # 2 + 2 - 1 - 1
    new_idx = p.apply_add_identifier("zeta")
    f, sites = _sites(p, "pick", "GetByIdShort", "alpha")
    assert len(sites) == 2
    # The read inside the then-branch: `t += o.zeta` adds undefined -> NaN. Both
    # reads share cache slot 1: with the slot kept, the second read would hit the
    # entry filled by the first `o.alpha` and still add 2.
    assert sites[0].operands[2].u32 == sites[1].operands[2].u32 != 0
    p.apply_rename_site(f.index, sites[1].offset, new_idx)
    assert _run(p, tmp_path) == ["NaN"]
    _, kept = _sites(p, "pick", "GetByIdShort", "alpha")
    assert len(kept) == 1


def test_rename_site_refuses_string_kind_target(proj):
    beta = list(proj.hbc.strings()).index("beta value")
    f, (site,) = _sites(proj, "main", "GetByIdShort", "alpha")
    with pytest.raises(ValueError, match="not an Identifier"):
        proj.apply_rename_site(f.index, site.offset, beta)
    assert proj.is_patched is False


def test_rename_site_refuses_non_name_operand(proj):
    beta = list(proj.hbc.strings()).index("beta value")
    f, (site,) = _sites(proj, "main", "LoadConstString", "beta value")
    gamma = list(proj.hbc.strings()).index("gamma")
    with pytest.raises(ValueError, match="does not name a property"):
        proj.apply_rename_site(f.index, site.offset, gamma)
    assert beta >= 0


def test_rename_site_refuses_shared_bytecode():
    path = Path(__file__).resolve().parents[3] / "seeder" / "corpus" / "ridemovi.hbc"
    if not path.exists():
        pytest.skip("ridemovi.hbc not in the corpus")
    p = HermitProject.from_hbc(str(path))
    kinds = p.hbc.string_kinds()
    any_id = next(i for i, k in enumerate(kinds) if k == ID)
    shared = {f.index for f in p.hbc.functions() if f.shared}
    for x in p.hbc.string_xrefs():
        for st in x.sites:
            if (st.kind == _libhbc.XRefKind.Operand and st.mnemonic in IDENTIFIER_OPCODES
                    and st.function_index in shared):
                with pytest.raises(ValueError, match="shared bytecode"):
                    p.apply_rename_site(st.function_index, st.instruction_offset, any_id)
                assert p.is_patched is False
                return
    pytest.skip("no identifier site in a shared function")


# --- editor: O -> pick a site ----------------------------------------------------


def _state(p, idx):
    xrefs = [x for x in p.hbc.string_xrefs() if x.string_index == idx]
    return StringEditState(idx, p.hbc.strings(), p.hbc.string_kinds(),
                           p.hbc.string_entries(), xrefs, p.hbc.functions())


def test_editor_pick_one_site(proj):
    st = _state(proj, list(proj.hbc.strings()).index("alpha"))
    sites = st.operand_sites
    assert len(sites) == 2                       # read + main; the literal key is not listed
    assert st.site_count == 3
    drive_strings(st, BS * 5 + "zeta" + CX + "o" + DOWN + ENTER)
    assert st.commit is True
    assert st.local_site is not None
    assert (st.local_site.function_index, st.local_site.instruction_offset) == \
        (sites[1].function_index, sites[1].instruction_offset)


def test_editor_pick_esc_goes_back_to_prompt(proj):
    st = _state(proj, list(proj.hbc.strings()).index("alpha"))
    drive_strings(st, BS * 5 + "zeta" + CX + "o" + ESC)
    assert st.mode == "exit_ask"
    assert st.local_site is None


def test_editor_pick_without_instruction_sites(proj):
    idx = list(proj.hbc.strings()).index("alpha")
    st = StringEditState(idx, proj.hbc.strings(), proj.hbc.string_kinds(),
                         proj.hbc.string_entries(), [], proj.hbc.functions())
    assert st.start_site_pick() is False
    assert "global rename" in st.edit_err


# --- REPL: both commits ----------------------------------------------------------


def _repl_local(sample_path, old, new, pick, monkeypatch):
    r = HermitREPL()
    r._open_file(str(sample_path))

    def fake_editor(idx, strings, kinds, entries, xrefs, funcs, **kw):
        st = StringEditState(idx, strings, kinds, entries, xrefs, funcs)
        st.value = new
        st.local_site = st.operand_sites[pick]
        st.commit = True
        return st

    monkeypatch.setattr(cli.editor, "run_string_editor", fake_editor)
    r._edit_string(r.project, f"string:{list(r.project.hbc.strings()).index(old)}")
    return r.project


def test_repl_local_rename_adds_identifier(sample, monkeypatch, tmp_path):
    p = _repl_local(sample, "alpha", "zeta", 1, monkeypatch)
    assert p.hbc.strings()[-1] == "zeta"
    assert p.hbc.string_kinds()[-1] == ID
    assert "alpha" in list(p.hbc.strings())
    assert _run(p, tmp_path) == ["1", "undefined", "7", "7", "beta value"]


def test_repl_local_rename_reuses_existing_identifier(sample, monkeypatch, tmp_path):
    """`o.delta` (the read in main) -> `o.gamma`: no new string, sum is 3 + 3."""
    n0 = _libhbc.HBCFile.open(str(sample)).header().string_count
    p = _repl_local(sample, "delta", "gamma", 1, monkeypatch)
    assert p.hbc.header().string_count == n0
    assert _run(p, tmp_path) == ["1", "1", "6", "7", "beta value"]
