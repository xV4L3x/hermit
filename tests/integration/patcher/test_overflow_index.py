"""Overflow strings are addressed by the index stored in their small entry.

A `SmallStringTableEntry` with `length == 255` carries in its `offset` field the
index of its entry in the Overflow String Table (Hermes'
`BytecodeDataProvider::getStringTableEntry` reads
`overflowStringTableEntries_[small.offset]`). These tests check what the VM-side
reader sees, using `hermes -b -dump-bytecode` as the oracle:

* `add_string` on a file that already has overflow strings stores the index of
  the new slot;
* `set_string_value` on an overflow entry finds the slot through that index, on
  a file whose slots are in any order;
* an overflow slot used by two entries is refused.
"""

import re
import struct
import subprocess

import pytest

import _libhbc
from testutil import CORPUS, HERMES

pytestmark = pytest.mark.skipif(HERMES is None, reason="hermes not available")

A, B = "A" * 300, "B" * 300

_DUMP_RE = re.compile(r"^s(\d+)\[(?:ASCII|UTF-16)[^\]]*\]: (.*)$")


def hermes_strings(path):
    """index -> text, as printed by Hermes' own reader of the string table."""
    r = subprocess.run([HERMES, "-b", "-dump-bytecode", str(path)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    out = {}
    for line in r.stdout.splitlines():
        m = _DUMP_RE.match(line)
        if m:
            out[int(m.group(1))] = m.group(2)
    return out


@pytest.fixture
def ovf_hbc(tmp_path):
    """HBC 96 compiled by hermes with two overflow strings (A*300, B*300)."""
    js = tmp_path / "ovf.js"
    js.write_text(f'var x = "{A}"; var y = "{B}"; print(x.length + y.length);\n')
    out = tmp_path / "ovf.hbc"
    subprocess.run([HERMES, "-emit-binary", "-out", str(out), str(js)], check=True)
    return out


def _align4(n):
    return (n + 3) & ~3


def _tables(f):
    """Absolute offsets of the string table and of the overflow table."""
    h = f.header()
    kind = _align4(128 + h.function_count * 16)
    ident = _align4(kind + h.string_kind_count * 4)
    stab = _align4(ident + h.identifier_count * 4)
    return stab, stab + h.string_count * 4


def _sentinels(raw, f):
    """Indices of the string table entries with length == 255."""
    stab, _ = _tables(f)
    return [i for i in range(f.header().string_count)
            if struct.unpack_from("<I", raw, stab + 4 * i)[0] >> 24 == 255]


def _permuted(path, out, shared=False):
    """Copy of `path` where the two overflow slots are swapped and the small
    entries updated to follow them: the strings seen by Hermes stay the same, the
    first sentinel names slot 1 and the second names slot 0. With `shared`, both
    small entries name slot 1."""
    f = _libhbc.HBCFile.open(str(path))
    raw = bytearray(path.read_bytes())
    stab, otab = _tables(f)
    s0, s1 = _sentinels(raw, f)[:2]
    if not shared:
        o0, o1 = raw[otab:otab + 8], raw[otab + 8:otab + 16]
        raw[otab:otab + 8], raw[otab + 8:otab + 16] = o1, o0
    for i, idx in ((s0, 1), (s1, 0 if not shared else 1)):
        w = struct.unpack_from("<I", raw, stab + 4 * i)[0]
        w = (w & ~(0x7FFFFF << 1)) | (idx << 1)
        struct.pack_into("<I", raw, stab + 4 * i, w)
    out.write_bytes(bytes(raw))
    return s0, s1


def _serialize(module, out):
    out.write_bytes(bytes(module.serialize()))
    return _libhbc.HBCFile.open(str(out))


def test_add_string_after_existing_overflow_uses_the_right_index(ovf_hbc, tmp_path):
    before = hermes_strings(ovf_hbc)
    f = _libhbc.HBCFile.open(str(ovf_hbc))
    assert f.header().overflow_string_count == 2

    m = _libhbc.Module.from_hbcfile(f)
    first = m.add_string("C" * 300)
    second = m.add_string("D" * 301)
    g = _serialize(m, tmp_path / "out.hbc")

    assert g.header().overflow_string_count == 4
    seen = hermes_strings(tmp_path / "out.hbc")
    assert seen[first] == "C" * 300          # what the VM reads, not slot 0
    assert seen[second] == "D" * 301
    assert all(seen[i] == before[i] for i in before)   # old strings untouched
    assert g.strings()[first] == "C" * 300 and g.strings()[second] == "D" * 301


def test_add_string_on_real_bundle_with_overflow(tmp_path):
    src = CORPUS / "hermitvault.hbc"
    if not src.exists():
        pytest.skip("missing corpus: hermitvault.hbc")
    before = hermes_strings(src)
    f = _libhbc.HBCFile.open(str(src))
    n_ovf = f.header().overflow_string_count
    assert n_ovf > 0

    m = _libhbc.Module.from_hbcfile(f)
    idx = m.add_string("Z" * 400)
    g = _serialize(m, tmp_path / "out.hbc")

    assert g.header().overflow_string_count == n_ovf + 1
    seen = hermes_strings(tmp_path / "out.hbc")
    assert seen[idx] == "Z" * 400
    assert all(seen[i] == before[i] for i in before)


def test_permuted_overflow_file_is_read_by_index(ovf_hbc, tmp_path):
    perm = tmp_path / "perm.hbc"
    _permuted(ovf_hbc, perm)
    # Hermes sees the same strings as in the original (the permutation is invisible to it).
    assert hermes_strings(perm) == hermes_strings(ovf_hbc)
    # hermit reads the same strings: A at index 0, B at index 1.
    f = _libhbc.HBCFile.open(str(perm))
    assert f.strings()[0] == A and f.strings()[1] == B


def test_set_string_value_on_overflow_entry_finds_the_slot_by_index(ovf_hbc, tmp_path):
    perm = tmp_path / "perm.hbc"
    s0, s1 = _permuted(ovf_hbc, perm)
    f = _libhbc.HBCFile.open(str(perm))
    assert f.strings()[s0] == A

    m = _libhbc.Module.from_hbcfile(f)
    m.set_string_value(s0, b"X" * 320)          # stays overflow whatever the length
    g = _serialize(m, tmp_path / "out.hbc")

    seen = hermes_strings(tmp_path / "out.hbc")
    assert seen[s0] == "X" * 320
    assert seen[s1] == B                         # the other overflow string is untouched
    assert g.strings()[s0] == "X" * 320 and g.strings()[s1] == B


def test_set_string_value_refuses_a_shared_overflow_slot(ovf_hbc, tmp_path):
    shared = tmp_path / "shared.hbc"
    s0, _ = _permuted(ovf_hbc, shared, shared=True)
    f = _libhbc.HBCFile.open(str(shared))
    assert f.strings()[0] == f.strings()[1] == B

    m = _libhbc.Module.from_hbcfile(f)
    with pytest.raises(RuntimeError, match="shared"):
        m.set_string_value(s0, b"X" * 320)
