"""Identifier strings (B3): the hash table and `Module.add_identifier`.

The VM creates a symbol at load time only for `Identifier` entries, with the hash
read from the Identifier Hash Table (`RuntimeModule::importStringIDMapMayAllocate`).
`GetById`/`PutById`/... read that symbol with `getSymbolIDMustExist`, which is
only an assert, so the release VM has no check. The tests run on the VM of each
version (94, 95, 96):

- `identifier_hash` recomputed over every Identifier equals the stored table;
- a `PutById` whose name is a `String` entry crashes the VM, the same name added
  with `add_identifier` works.

Sample: `seeder/samples/identifiers.js`, compiled at test time (`-g0`).
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

import _libhbc

SAMPLE = Path(__file__).resolve().parents[3] / "seeder" / "samples" / "identifiers.js"
_LOCAL = Path.home() / ".local" / "share" / "hermit" / "hermes"
EXPECTED = ["1", "1", "7", "7", "beta value"]


def _compiler(version):
    if version == 96:
        return shutil.which("hermes")
    env = os.environ.get(f"HERMES{version}")
    if env and Path(env).exists():
        return env
    p = _LOCAL / f"hermes{version}"
    return str(p) if p.exists() else None


VERSIONS = [
    pytest.param(v, marks=pytest.mark.skipif(_compiler(v) is None,
                                             reason=f"no hermes for HBC {v}"))
    for v in (94, 95, 96)
]


def _compile(version, tmp_path):
    out = tmp_path / f"identifiers_v{version}.hbc"
    r = subprocess.run([_compiler(version), "-emit-binary", "-g0", "-out", str(out),
                        str(SAMPLE)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    return out


def _run(version, path):
    return subprocess.run([_compiler(version), str(path)], capture_output=True,
                          text=True, timeout=30)


def _recomputed(f, data):
    return [
        _libhbc.identifier_hash(data[e.storage_offset:e.storage_offset + e.byte_length],
                                e.is_utf16)
        for e in f.string_entries() if e.kind == _libhbc.StringKind.Identifier
    ]


def _put_delta_site(f):
    """Absolute file position of the string operand of `PutById ..., "delta"` in main
    (UInt16, last two bytes of the instruction)."""
    main = next(fn for fn in f.functions() if fn.name == "main")
    delta = f.strings().index("delta")
    for ins in f.disasm(main.index):
        if ins.mnemonic == "PutById" and ins.operands[3].u32 == delta:
            return main.offset + ins.offset + 4
    raise AssertionError("PutById 'delta' not found")


def _redirect_put(data, f, new_idx):
    pos = _put_delta_site(f)
    out = bytearray(data)
    out[pos:pos + 2] = new_idx.to_bytes(2, "little")
    return bytes(out)


# --- hash oracle ----------------------------------------------------------


@pytest.mark.parametrize("version", VERSIONS)
def test_hash_matches_stored_table(version, tmp_path):
    path = _compile(version, tmp_path)
    f = _libhbc.HBCFile.open(str(path))
    data = path.read_bytes()
    assert f.version() == version
    assert list(f.identifier_hashes()) == _recomputed(f, data)
    assert len(f.identifier_hashes()) == f.header().identifier_count


def test_hash_utf16_identifiers_real_bundle():
    path = Path(__file__).resolve().parents[3] / "seeder" / "corpus" / "ridemovi.hbc"
    if not path.exists():
        pytest.skip("ridemovi.hbc not in the corpus")
    f = _libhbc.HBCFile.open(str(path))
    assert any(e.is_utf16 and e.kind == _libhbc.StringKind.Identifier
               for e in f.string_entries())
    assert list(f.identifier_hashes()) == _recomputed(f, path.read_bytes())


def test_hash_binding_values():
    assert _libhbc.identifier_hash(b"") == 0
    assert _libhbc.identifier_hash(b"a\x00b\x00", True) == _libhbc.identifier_hash(b"ab")
    with pytest.raises(ValueError):
        _libhbc.identifier_hash(b"a\x00b", True)


# --- why the kind matters, and add_identifier end to end ------------------


@pytest.mark.parametrize("version", VERSIONS)
def test_original_sample_runs(version, tmp_path):
    r = _run(version, _compile(version, tmp_path))
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines() == EXPECTED


@pytest.mark.parametrize("version", VERSIONS)
def test_putbyid_on_string_kind_crashes_vm(version, tmp_path):
    """Name of a PutById redirected to a String entry: no symbol exists for it."""
    path = _compile(version, tmp_path)
    f = _libhbc.HBCFile.open(str(path))
    beta = f.strings().index("beta value")
    assert f.string_kinds()[beta] == _libhbc.StringKind.String
    bad = tmp_path / "bad.hbc"
    bad.write_bytes(_redirect_put(path.read_bytes(), f, beta))
    assert _run(version, bad).returncode != 0


@pytest.mark.parametrize("version", VERSIONS)
def test_add_identifier_used_by_putbyid(version, tmp_path):
    """`o.delta = 4` rewritten as `o.epsilon = 4`: the read of `o.delta` gives
    undefined, so `o.gamma + o.delta` prints NaN, and the VM does not crash."""
    path = _compile(version, tmp_path)
    f = _libhbc.HBCFile.open(str(path))
    m = _libhbc.Module.from_hbcfile(f)
    idx = m.add_identifier("epsilon")
    assert idx == f.header().string_count
    added = bytes(m.serialize())

    g = _libhbc.HBCFile.from_bytes(added)
    assert g.strings()[idx] == "epsilon"
    assert g.string_kinds()[idx] == _libhbc.StringKind.Identifier
    assert g.header().identifier_count == f.header().identifier_count + 1
    assert list(g.identifier_hashes()) == _recomputed(g, added)

    unchanged = tmp_path / "added.hbc"
    unchanged.write_bytes(added)
    assert _run(version, unchanged).stdout.splitlines() == EXPECTED

    patched = tmp_path / "patched.hbc"
    patched.write_bytes(_redirect_put(added, g, idx))
    r = _run(version, patched)
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines() == ["1", "1", "NaN", "7", "beta value"]


@pytest.mark.parametrize("version", VERSIONS)
def test_add_string_same_name_still_crashes(version, tmp_path):
    """The same name added as a String (add_string) is not a symbol: crash."""
    path = _compile(version, tmp_path)
    f = _libhbc.HBCFile.open(str(path))
    m = _libhbc.Module.from_hbcfile(f)
    idx = m.add_string("epsilon")
    added = bytes(m.serialize())
    g = _libhbc.HBCFile.from_bytes(added)
    bad = tmp_path / "bad.hbc"
    bad.write_bytes(_redirect_put(added, g, idx))
    assert _run(version, bad).returncode != 0
