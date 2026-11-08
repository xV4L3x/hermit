"""String entries whose byte range leaves the string storage.

The Hermes runtime reads [offset, offset + length * unit) from the string storage
without checking it. hermit checks it: Strict mode (default) raises
InvalidStringEntryError; Permissive mode opens the file for analysis and reports a
verdict per entry (InsideStorage / InsideFile / OutsideFile / BadOverflowIndex).

Fixture `string_range_outside_file.hbc` (from a fuzzer): overflow string 0 is
524288 UTF-16 units at storage offset 555, past the end of the 738-byte file
(it used to segfault `strings()`).
"""

import struct
from pathlib import Path

import _libhbc
import pytest

from cli import display
from cli.project import HermitProject
from cli.repl import HermitREPL
from testutil import corpus_path

FIXTURE = Path(__file__).parent / "data" / "string_range_outside_file.hbc"
STRING_STORAGE_SIZE_OFFSET = 0x3C  # HBCFileHeader::stringStorageSize

V = _libhbc.StringVerdict
PERMISSIVE = _libhbc.ValidationMode.Permissive


def _hello_with_storage_size(size: int, mode=_libhbc.ValidationMode.Strict):
    data = bytearray(corpus_path("hello.hbc").read_bytes())
    struct.pack_into("<I", data, STRING_STORAGE_SIZE_OFFSET, size)
    return _libhbc.HBCFile.from_bytes(bytes(data), mode)


def test_intact_file_decodes_in_both_modes():
    path = str(corpus_path("hello.hbc"))
    strict = _libhbc.HBCFile.open(path)
    perm = _libhbc.HBCFile.open(path, PERMISSIVE)
    assert strict.strings() == perm.strings()
    assert all(e.verdict == V.InsideStorage and not e.misaligned for e in strict.string_entries())


@pytest.mark.parametrize("call", ["strings", "string_entries"])
def test_strict_raises_with_index(call):
    hbc = _libhbc.HBCFile.open(str(FIXTURE))
    with pytest.raises(_libhbc.InvalidStringEntryError, match="string 0 .*string storage"):
        getattr(hbc, call)()


def test_strict_error_is_a_runtime_error():
    hbc = _hello_with_storage_size(1)
    with pytest.raises(RuntimeError, match="string storage"):
        hbc.strings()


def test_permissive_fixture_outside_file():
    hbc = _libhbc.HBCFile.open(str(FIXTURE), PERMISSIVE)
    entries = hbc.string_entries()
    assert entries[0].verdict == V.OutsideFile
    assert entries[0].is_overflow and entries[0].is_utf16 and entries[0].misaligned
    assert entries[0].char_length == 524288
    assert [e.verdict for e in entries[1:]] == [V.InsideStorage] * 3
    assert hbc.strings() == ["", "global", "print", "s"]
    # the rest of the file is analysable
    assert [f.name for f in hbc.functions()] == ["global"]


def test_permissive_inside_file_is_decoded():
    # storage shrunk to 1 byte: every entry now lies outside it but inside the file
    hbc = _hello_with_storage_size(1, PERMISSIVE)
    assert any(e.verdict == V.InsideFile for e in hbc.string_entries())
    good = _libhbc.HBCFile.open(str(corpus_path("hello.hbc"))).strings()
    assert hbc.strings() == good  # same bytes, only the verdict differs


def test_permissive_info_strings_reports_invalid_entries():
    hbc = _libhbc.HBCFile.open(str(FIXTURE), PERMISSIVE)
    st = display.strings_stats(hbc)
    assert st["verdicts"][V.OutsideFile] == 1
    assert st["verdicts"][V.InsideStorage] == 3
    assert st["misaligned"] == 1


def test_project_permissive_is_read_only():
    proj = HermitProject.from_hbc(str(FIXTURE), permissive=True)
    assert proj.is_permissive
    with pytest.raises(ValueError, match="read-only"):
        proj._commit(bytearray(proj.image_bytes), proj.hbc)
    assert not proj.is_patched


def test_repl_open_permissive():
    r = HermitREPL()
    with pytest.raises(ValueError, match="open --permissive"):   # strict: refused with a hint
        r._dispatch(f"open {FIXTURE}")
    assert r.project is None
    r._dispatch(f"open --permissive {FIXTURE}")
    assert r.project is not None and r.project.is_permissive
    assert " ro " in r._prompt_str()
    r._dispatch("info strings")
    r._dispatch("info all")
