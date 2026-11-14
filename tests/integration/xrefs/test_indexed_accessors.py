"""Indexed string_xrefs(string_index=N) and bulk function_modules() agree with
the full-list / per-function forms they are shortcuts for."""

from pathlib import Path

import _libhbc

CORPUS = Path(__file__).resolve().parents[3] / "seeder" / "corpus"


def _open(name):
    return _libhbc.HBCFile.open(str(CORPUS / name))


def test_string_xref_indexed_matches_full_list():
    f = _open("hermitvault.hbc")
    full = f.string_xrefs()
    by_index = {x.string_index: x for x in full}

    for x in full:
        one = f.string_xrefs(string_index=x.string_index)
        assert one is not None
        assert one.string_index == x.string_index
        assert [(s.function_index, s.instruction_offset) for s in one.sites] == \
               [(s.function_index, s.instruction_offset) for s in x.sites]

    # A string with no references, and an out-of-range index, both give None.
    unreferenced = next(i for i in range(len(f.strings())) if i not in by_index)
    assert f.string_xrefs(string_index=unreferenced) is None
    assert f.string_xrefs(string_index=len(f.strings()) + 1000) is None


def test_function_modules_bulk_matches_per_function():
    f = _open("hermitvault.hbc")
    bulk = f.function_modules()
    assert len(bulk) == len(f.functions())
    for i in range(len(bulk)):
        assert bulk[i] == f.function_module(i)
