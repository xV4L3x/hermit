"""Literal-buffer string XREFs against the Hermes oracle.

`hermes -b -dump-bytecode` prints the three literal buffers as flat element lists
(`[String 7]`, `[int 3]`, `[double 2.5]`, `true`, `false`, `null`), decoded by Hermes.
That list is the whole buffer read from byte 0, run after run. It is exact only for
a buffer written one literal after another. An optimized build (`-O`, every real app)
packs the literals with `ConsecutiveStringStorage` (`optimize` on): a literal can be
a substring of another, and the end of one can overlap the start of the next (a
literal starting with the bytes `75 00 00 00` overlaps a preceding `[117]`, whose
value bytes are those). A site's offset then points inside another literal's bytes,
the whole-buffer read stops matching the sites, and in the real bundles it does not
even end at the end of the buffer (hermitvault: 501 runs, 39191 bytes read from a
9108-byte buffer). Hermes notes this in `BytecodeDisassembler.cpp` ("the output might
not show the value of every literal"). The dump is therefore an oracle for
unoptimized files only.

So three kinds of check:

1. Unoptimized files: for every New{Array,Object}WithBuffer[Long] site, the strings
   `string_xrefs()` reports for the instruction are exactly the strings of dump
   elements [first, first + count).
2. The same source compiled at -O0 and at -O, with literals that are byte-for-byte
   substrings of each other: the array references (string, element, count) must be
   identical, and the -O build must really overlap them.
3. Real bundles (hermitvault, ridemovi): every site decodes, every string index is
   in range, and every object KEY reference is an Identifier string (the compiler
   writes keys with getIdentifierID).
"""

import re
import subprocess

import _libhbc
import pytest

from cli.literals import SLPTag, decode_slp
from testutil import HERMES, corpus_path

UNOPTIMIZED = ["xrefs_literals.hbc", "array_object_literals.hbc", "array_object_literals_long.hbc"]
REAL_BUNDLES = ["hermitvault.hbc", "ridemovi.hbc"]
OVERLAP_JS = """"use strict";
var a1 = [1, "ab", 2, "cd", 3, "ef", 4, "gh"];
var a2 = [2, "cd", 3, "ef"];
var a3 = [3, "ef", 4, "gh"];
var a4 = ["ef", 4, "gh"];
var b1 = {p: 1, q: "x1", r: 2, s: "x2"};
var b2 = {r: 2, s: "x2"};
var c1 = [true, "t1", false, "t2", null, "t3", 7, "t4"];
var c2 = [false, "t2", null, "t3"];
var e1 = [117];
var e2 = [0, 1, 2, 3, 4];
print(a1, a2, a3, a4, b1, b2, c1, c2, e1, e2);
"""
_W = {0: 0, 1: 0, 2: 0, 3: 8, 4: 4, 5: 2, 6: 1, 7: 4}
STRING_TAGS = (SLPTag.LONG_STRING, SLPTag.SHORT_STRING, SLPTag.BYTE_STRING)
HEADERS = {"Array Buffer:": "array", "Object Key Buffer:": "key", "Object Value Buffer:": "value"}
TOKEN = re.compile(r"^(?:\[String (\d+)\]|\[int (-?\d+)\]|\[double (\S+)\]|(true|false|null))$")


def _dump_buffers(path):
    """{'array'|'key'|'value': [token, ...]} where a token is ('s', idx), ('i', n),
    ('d', text) or ('k', 'true'|'false'|'null')."""
    out = subprocess.run([HERMES, "-b", "-dump-bytecode", str(path)],
                         capture_output=True, text=True, check=True).stdout
    bufs, cur = {"array": [], "key": [], "value": []}, None
    for line in out.splitlines():
        if line in HEADERS:
            cur = HEADERS[line]
            continue
        if cur is None:
            continue
        m = TOKEN.match(line)
        if not m:
            cur = None            # next section (Function<...>, blank line, ...)
            continue
        if m.group(1) is not None:   bufs[cur].append(("s", int(m.group(1))))
        elif m.group(2) is not None: bufs[cur].append(("i", int(m.group(2))))
        elif m.group(3) is not None: bufs[cur].append(("d", m.group(3)))
        else:                        bufs[cur].append(("k", m.group(4)))
    return bufs


def _same(tok, e):
    kind, val = tok
    if kind == "s": return e.tag in STRING_TAGS and e.value == val
    if kind == "i": return e.tag == SLPTag.INTEGER and e.value == val
    if kind == "d": return e.tag == SLPTag.NUMBER
    return {"true": SLPTag.TRUE, "false": SLPTag.FALSE, "null": SLPTag.NULL}[val] == e.tag


def _walk_is_exact(buf):
    """True if reading `buf` run after run from byte 0 ends exactly at its end."""
    pos = 0
    while pos < len(buf):
        b = buf[pos]
        n, hl = (((b & 15) << 8) | buf[pos + 1], 2) if b >> 7 else (b & 15, 1)
        pos += hl + n * _W[(b >> 4) & 7]
    return pos == len(buf)


def _check_against_dump(path):
    f = _libhbc.HBCFile.open(str(path))
    tokens = _dump_buffers(path)
    raw = {"array": f.array_buffer(), "key": f.obj_key_buffer(), "value": f.obj_value_buffer()}
    assert all(_walk_is_exact(b) for b in raw.values()), "overlapping buffer: the dump is not a valid oracle"

    # 1. whole-buffer decode == dump
    decoded = {}
    for which, toks in tokens.items():
        elems = decode_slp(raw[which], 0, len(toks)) if toks else []
        assert len(elems) == len(toks)
        for i, (tok, e) in enumerate(zip(toks, elems)):
            assert _same(tok, e), f"{name} {which} element {i}: dump {tok}, decoded {e}"
        decoded[which] = {e.byte_off - (1 if False else 0): i for i, e in enumerate(elems)}
        # byte offset of an element -> its index; run headers sit between elements, so the
        # operand of a site is the offset of its first run header, resolved below.

    # offset operand -> index of the first element of the segment: the first element
    # whose own offset is >= operand (the run header precedes its elements).
    def first_element(which, offset):
        elems = decode_slp(raw[which], offset, 1)    # starts on a run header
        return decoded[which][elems[0].byte_off]

    expected = {}
    n_sites = 0
    for site in f.literal_sites():
        n_sites += 1
        if site.is_object:
            parts = [("ObjectKey", "key", site.key_buffer_idx),
                     ("ObjectValue", "value", site.buffer_idx)]
        else:
            parts = [("ArrayElement", "array", site.buffer_idx)]
        for kind, which, off in parts:
            first = first_element(which, off)
            seen = {}
            for i in range(first, first + site.count):
                tok = tokens[which][i]
                if tok[0] == "s":
                    pos, n = seen.get(tok[1], (i - first, 0))
                    seen[tok[1]] = (pos, n + 1)
            for s, v in seen.items():
                expected[(site.function_index, site.instruction_offset, kind, s)] = v

    actual = {}
    for xr in f.string_xrefs():
        for s in xr.sites:
            if s.kind.name != "Operand":
                actual[(s.function_index, s.instruction_offset, s.kind.name,
                        xr.string_index)] = (s.element_index, s.element_count)

    assert actual == expected
    print(f"\n{path.name}: {n_sites} sites, {len(actual)} literal references, "
          f"{len({k[3] for k in actual})} strings, "
          f"buffer elements array/key/value = "
          f"{len(tokens['array'])}/{len(tokens['key'])}/{len(tokens['value'])}")


@pytest.mark.skipif(HERMES is None, reason="hermes (HBC 96) not installed")
@pytest.mark.parametrize("name", UNOPTIMIZED)
def test_literal_xrefs_match_hermes_dump(name):
    path = corpus_path(name)
    if not path.exists():
        pytest.skip(f"{name} not in the corpus")
    _check_against_dump(path)


def _compile(tmp_path, opt):
    src = tmp_path / "overlap.js"
    src.write_text(OVERLAP_JS)
    out = tmp_path / f"overlap{opt}.hbc"
    subprocess.run([HERMES, opt, "-g0", "-emit-binary", "-out", str(out), str(src)], check=True)
    return out


def _array_refs(f):
    """{(string, first element, element count)} over the array-literal references."""
    strings = f.strings()
    return sorted((strings[xr.string_index], s.element_index, s.element_count)
                  for xr in f.string_xrefs() for s in xr.sites
                  if s.kind.name == "ArrayElement")


@pytest.mark.skipif(HERMES is None, reason="hermes (HBC 96) not installed")
def test_unoptimized_stress_sample_matches_hermes_dump(tmp_path):
    _check_against_dump(_compile(tmp_path, "-O0"))


@pytest.mark.skipif(HERMES is None, reason="hermes (HBC 96) not installed")
def test_overlapping_literals_give_the_same_references_as_unoptimized(tmp_path):
    o0 = _libhbc.HBCFile.open(str(_compile(tmp_path, "-O0")))
    o1 = _libhbc.HBCFile.open(str(_compile(tmp_path, "-O")))

    # -O really overlaps: the buffer is smaller and some site starts before the
    # previous one ends.
    buf = o1.array_buffer()
    segs = sorted({(s.buffer_idx, s.count) for s in o1.literal_sites() if not s.is_object})
    ends = []
    for off, count in segs:
        last = decode_slp(buf, off, count)[-1]
        ends.append(last.byte_off + last.byte_len)
    assert len(buf) < len(o0.array_buffer())
    assert any(segs[i + 1][0] < ends[i] for i in range(len(segs) - 1))

    assert _array_refs(o1) == _array_refs(o0)
    assert _array_refs(o1)     # non-empty


@pytest.mark.skipif(HERMES is None, reason="hermes (HBC 96) not installed")
def test_overlapping_objects_share_key_and_value_segments(tmp_path):
    f = _libhbc.HBCFile.open(str(_compile(tmp_path, "-O")))
    strings = f.strings()
    refs = {(strings[xr.string_index], s.kind.name, s.element_index, s.element_count)
            for xr in f.string_xrefs() for s in xr.sites
            if s.kind.name in ("ObjectKey", "ObjectValue")}
    # b1 = {p: 1, q: "x1", r: 2, s: "x2"}, b2 = {r: 2, s: "x2"} (bytes inside b1's)
    assert ("p", "ObjectKey", 0, 1) in refs and ("q", "ObjectKey", 1, 1) in refs
    assert ("r", "ObjectKey", 2, 1) in refs and ("r", "ObjectKey", 0, 1) in refs
    assert ("x1", "ObjectValue", 1, 1) in refs
    assert ("x2", "ObjectValue", 3, 1) in refs and ("x2", "ObjectValue", 1, 1) in refs


@pytest.mark.parametrize("name", REAL_BUNDLES)
def test_real_bundle_literal_references_are_consistent(name):
    path = corpus_path(name)
    if not path.exists():
        pytest.skip(f"{name} not in the corpus")
    f = _libhbc.HBCFile.open(str(path))
    kinds, n = f.string_kinds(), len(f.strings())
    bufs = {"ArrayElement": f.array_buffer(), "ObjectKey": f.obj_key_buffer(),
            "ObjectValue": f.obj_value_buffer()}

    # every site decodes, and the C++ decoder agrees with the Python one (test_literal_xrefs.py)
    for site in f.literal_sites():
        parts = ([("ObjectKey", site.key_buffer_idx), ("ObjectValue", site.buffer_idx)]
                 if site.is_object else [("ArrayElement", site.buffer_idx)])
        for kind, off in parts:
            decode_slp(bufs[kind], off, site.count)       # raises SLPError on a bad site

    key_refs = 0
    for xr in f.string_xrefs():
        assert xr.string_index < n
        for s in xr.sites:
            if s.kind.name == "ObjectKey":
                key_refs += 1
                assert kinds[xr.string_index].name == "Identifier", \
                    f"object key {xr.string_value!r} is not an Identifier string"
    assert key_refs > 0
