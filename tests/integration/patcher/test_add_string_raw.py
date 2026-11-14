"""`add_string_raw` - adding a string from raw, already-encoded bytes.

`add_string`/`add_identifier` are ASCII-only (`queueNewString`'s per-byte
check) and the pybind binding takes `std::string`, so pybind11's UTF-8 caster
rejects any Python `str` with unpaired surrogates before `Module::add_string`
even runs (fuzzing bug report, 2026-11-14). `add_string_raw` takes `bytes`
(no UTF-8/ASCII validation at all) plus an explicit `utf16` flag, so it can
build a malformed-UTF-16 string - unpaired surrogates included - which is
exactly the interesting case for a value-fuzzer exercising the VM's
string/rope code paths.
"""

import pytest

import _libhbc
from testutil import CORPUS, HERMES, run_hermes


def _load():
    path = CORPUS / "greeting_g0.hbc"
    if not path.exists():
        pytest.skip("missing corpus: greeting_g0.hbc")
    return _libhbc.HBCFile.open(str(path))


def test_adds_utf16_string_with_unpaired_surrogates():
    f = _load()
    m = _libhbc.Module.from_hbcfile(f)
    # 0xD800 LE, repeated: an unpaired high surrogate - not representable by
    # add_string (ASCII-only) nor by a Python str passed through pybind's UTF-8
    # caster (PyUnicode_AsUTF8AndSize fails on it).
    raw = b"\x00\xd8" * 2048
    idx = m.add_string_raw(raw, True)
    assert idx == f.header().string_count

    data = m.serialize()
    g = _libhbc.HBCFile.from_bytes(bytes(data))
    e = g.string_entries()[idx]
    assert e.is_utf16 is True
    assert e.char_length == 2048
    assert e.byte_length == 4096
    assert e.verdict == _libhbc.StringVerdict.InsideStorage


def test_adds_ascii_string_same_as_add_string():
    f = _load()
    m = _libhbc.Module.from_hbcfile(f)
    idx = m.add_string_raw(b"hello", False)
    data = m.serialize()
    g = _libhbc.HBCFile.from_bytes(bytes(data))
    assert g.strings()[idx] == "hello"
    assert g.string_entries()[idx].is_utf16 is False


def test_rejects_empty_bytes():
    f = _load()
    m = _libhbc.Module.from_hbcfile(f)
    with pytest.raises(ValueError, match="empty"):
        m.add_string_raw(b"", False)


def test_rejects_odd_length_utf16():
    f = _load()
    m = _libhbc.Module.from_hbcfile(f)
    with pytest.raises(ValueError, match="odd"):
        m.add_string_raw(b"\x00\xd8\x01", True)


def test_add_string_still_rejects_non_ascii_unaffected():
    """Regression guard: add_string/add_identifier keep their original
    ASCII-only behavior - add_string_raw is purely additive."""
    f = _load()
    m = _libhbc.Module.from_hbcfile(f)
    with pytest.raises(ValueError, match="non-ASCII"):
        m.add_string("café")
    with pytest.raises(ValueError, match="non-ASCII"):
        m.add_identifier("café")


def test_identifier_kind_hashes_utf16_bytes_not_ascii():
    """An Identifier added via add_string_raw must hash the actual UTF-16
    bytes (identifier_hash(..., isUtf16=True)), not fall back to the
    hardcoded isUtf16=False path add_identifier uses."""
    f = _load()
    m = _libhbc.Module.from_hbcfile(f)
    raw = "xé".encode("utf-16-le")  # "xé"
    idx = m.add_string_raw(raw, True, _libhbc.StringKind.Identifier)
    data = m.serialize()
    g = _libhbc.HBCFile.from_bytes(bytes(data))
    assert g.string_entries()[idx].kind == _libhbc.StringKind.Identifier
    expected = _libhbc.identifier_hash(raw, True)
    # The hash table position for this entry: it's the only identifier added,
    # appended at the end, so it's the last Identifier-kind entry's hash.
    ids = [i for i, e in enumerate(g.string_entries()) if e.kind == _libhbc.StringKind.Identifier]
    assert ids and ids[-1] == idx


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_file_with_raw_utf16_string_loads_on_the_real_vm(tmp_path):
    f = _load()
    m = _libhbc.Module.from_hbcfile(f)
    m.add_string_raw(b"\x00\xd8" * 2048, True)
    data = m.serialize()
    out = tmp_path / "utf16raw.hbc"
    out.write_bytes(bytes(data))
    r = run_hermes(out)
    assert r.returncode == 0, r.stderr
