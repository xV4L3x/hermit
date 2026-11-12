"""Global rename of an Identifier string (B3, `Module.set_string_value`).

An Identifier entry is referenced by index from every GetById/PutById/... and
object-literal key that names it, so rewriting it renames the property at all
those sites at once. `set_string_value` also rewrites its hash in the
Identifier Hash Table. The hash is what makes two entries with the same text the
same symbol: with a stale hash the VM registers a second, different symbol with
that text (`test_stale_hash_splits_the_symbol`).

Sample: `seeder/samples/identifiers.js`, run on the VM of each version.
"""

import struct

import pytest

import _libhbc
from test_identifier_add import EXPECTED, VERSIONS, _compile, _recomputed, _run


def _rename(path, old, new):
    f = _libhbc.HBCFile.open(str(path))
    m = _libhbc.Module.from_hbcfile(f)
    m.set_string_value(list(f.strings()).index(old), new.encode())
    return f, bytes(m.serialize())


def _write_run(version, tmp_path, name, data):
    out = tmp_path / name
    out.write_bytes(data)
    r = _run(version, out)
    assert r.returncode == 0, r.stderr
    return r.stdout.splitlines()


@pytest.mark.parametrize("version", VERSIONS)
@pytest.mark.parametrize("old,new", [
    ("alpha", "zeta"),          # object-literal key + GetByIdShort in two functions
    ("counter", "kount"),       # global var: DeclareGlobalVar + PutById + GetByIdShort
    ("delta", "a_much_longer_property_name"),
])
def test_global_rename_keeps_behaviour(version, tmp_path, old, new):
    path = _compile(version, tmp_path)
    f, data = _rename(path, old, new)
    g = _libhbc.HBCFile.from_bytes(data)

    idx = list(f.strings()).index(old)
    assert g.strings()[idx] == new
    assert g.string_kinds()[idx] == _libhbc.StringKind.Identifier
    assert g.header().identifier_count == f.header().identifier_count
    assert list(g.identifier_hashes()) == _recomputed(g, data)
    # Every site that named `old` now names `new`.
    sites = [x for x in g.string_xrefs() if x.string_index == idx]
    assert sites
    assert _write_run(version, tmp_path, "renamed.hbc", data) == EXPECTED


@pytest.mark.parametrize("version", VERSIONS)
def test_rename_to_existing_name_is_the_same_property(version, tmp_path):
    """`delta` -> `gamma`: two Identifier entries with the same text resolve to one
    symbol, so `o.gamma = 4` overwrites 3 and `o.gamma + o.gamma` prints 8."""
    path = _compile(version, tmp_path)
    _, data = _rename(path, "delta", "gamma")
    assert _write_run(version, tmp_path, "dup.hbc", data) == ["1", "1", "8", "7", "beta value"]


@pytest.mark.parametrize("version", VERSIONS)
def test_stale_hash_splits_the_symbol(version, tmp_path):
    """Same rename with the old hash put back: the VM makes a second `gamma` symbol,
    so the write and the read hit different properties (3 + 4 = 7, wrong)."""
    path = _compile(version, tmp_path)
    f, data = _rename(path, "delta", "gamma")
    g = _libhbc.HBCFile.from_bytes(data)
    kinds = list(g.string_kinds())
    ids = [i for i, k in enumerate(kinds) if k == _libhbc.StringKind.Identifier]
    delta = list(f.strings()).index("delta")
    table = next(s for s in g.section_map() if s.name == "identifier hash table").offset
    stale = bytearray(data)
    struct.pack_into("<I", stale, table + 4 * ids.index(delta), _libhbc.identifier_hash(b"delta"))
    assert _write_run(version, tmp_path, "stale.hbc", bytes(stale)) == \
        ["1", "1", "7", "7", "beta value"]
