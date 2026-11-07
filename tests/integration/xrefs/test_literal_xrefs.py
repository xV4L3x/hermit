"""Literal-buffer string XREFs: cross-check of the C++ decoder (core/src/util/slp.cpp)
against the Python one (cli/literals.py) on the whole corpus.

For every New{Array,Object}WithBuffer[Long] site, the strings that
`decode_slp` finds in the buffer must be exactly the literal references that
`string_xrefs()` reports for that instruction."""

from collections import defaultdict
from pathlib import Path

import _libhbc
import pytest

from cli.literals import SLPError, SLPTag, decode_slp

CORPUS = Path(__file__).resolve().parents[3] / "seeder" / "corpus"
FILES = [
    "xrefs_literals.hbc",
    "array_object_literals.hbc",
    "array_object_literals_long.hbc",
    "literals_g0.hbc",
    "literals_resize_g0.hbc",
    "hermitvault.hbc",
    "ridemovi.hbc",
]
STRING_TAGS = (SLPTag.LONG_STRING, SLPTag.SHORT_STRING, SLPTag.BYTE_STRING)
KIND_NAMES = {"ArrayElement", "ObjectKey", "ObjectValue"}


def _expected(f):
    """{(func, offset, kind, string): (first_element, element_count)} from the Python decoder."""
    arr, keys, vals = f.array_buffer(), f.obj_key_buffer(), f.obj_value_buffer()
    out = {}
    for site in f.literal_sites():
        if site.is_object:
            parts = [("ObjectKey", keys, site.key_buffer_idx),
                     ("ObjectValue", vals, site.buffer_idx)]
        else:
            parts = [("ArrayElement", arr, site.buffer_idx)]
        for kind, buf, off in parts:
            try:
                elems = decode_slp(buf, off, site.count)
            except SLPError:
                continue
            seen = {}
            for i, e in enumerate(elems):
                if e.tag in STRING_TAGS:
                    first, n = seen.get(e.value, (i, 0))
                    seen[e.value] = (first, n + 1)
            for s, v in seen.items():
                out[(site.function_index, site.instruction_offset, kind, s)] = v
    return out


def _actual(f):
    out = {}
    for xr in f.string_xrefs():
        for s in xr.sites:
            if s.kind.name in KIND_NAMES:
                key = (s.function_index, s.instruction_offset, s.kind.name, xr.string_index)
                assert key not in out, f"duplicate literal site {key}"
                out[key] = (s.element_index, s.element_count)
    return out


@pytest.mark.parametrize("name", FILES)
def test_cpp_literal_xrefs_match_python_decoder(name):
    path = CORPUS / name
    if not path.exists():
        pytest.skip(f"{name} not in the corpus")
    f = _libhbc.HBCFile.open(str(path))
    assert _actual(f) == _expected(f)


def test_xrefs_literals_sample_has_literal_references():
    path = CORPUS / "xrefs_literals.hbc"
    if not path.exists():
        pytest.skip("xrefs_literals.hbc not in the corpus")
    f = _libhbc.HBCFile.open(str(path))
    assert len(_actual(f)) >= 20   # 3 + 2 + 3 + 6 (x2 functions) + 6 object entries
