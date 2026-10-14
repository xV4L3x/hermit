"""String encodings: ASCII, accented, supplementary and isolated-surrogate strings.

In the file an entry with `isUTF16 = 0` holds ASCII (one byte per character) and one
with `isUTF16 = 1` holds UTF-16LE (two bytes per code unit); `length` counts code
units. The Hermes compiler stores a string as ASCII when every character is ASCII and
as UTF-16 otherwise. These tests compile real strings with `hermes` and check:

* what the compiler chose (the premise of the parser) and what hermit reads, with
  `hermes -b -dump-bytecode` as the reference reader;
* which characters the patcher accepts on each kind of entry;
* an end-to-end edit of an accented string, run on the VM.
"""

import re
import subprocess
from types import SimpleNamespace

import pytest

import _libhbc
from cli.editor.strings import (
    _StrEditError,
    encode_new_string,
    encode_string_new_value,
    encode_string_value,
)
from testutil import HERMES

pytestmark = pytest.mark.skipif(HERMES is None, reason="hermes not available")

FFFD = "�"

# JS source -> (is_utf16, code units, string as hermit returns it).
CASES = {
    "abc": (False, 3, "abc"),
    "caff\\u00e8": (True, 5, "caffè"),
    "\\u00e0\\u00e8\\u00e9\\u00ec\\u00f2\\u00f9": (True, 6, "àèéìòù"),
    "x\\u{1F600}y": (True, 4, "x\U0001F600y"),
    "\\u{1D4B3}": (True, 2, "\U0001D4B3"),
    "\\uD800": (True, 1, FFFD),
    "\\uDC00": (True, 1, FFFD),
    "a\\uDC00b": (True, 3, "a" + FFFD + "b"),
    "\\uDC00\\uD800": (True, 2, FFFD + FFFD),
    "\\uD83D\\uDE00\\uD83D": (True, 3, "\U0001F600" + FFFD),
}

_DUMP_RE = re.compile(r"^s(\d+)\[(ASCII|UTF-16), (\d+)\.\.(\d+)\]")


def _compile(tmp_path, js, name="t"):
    src = tmp_path / f"{name}.js"
    src.write_text(js)
    out = tmp_path / f"{name}.hbc"
    subprocess.run([HERMES, "-emit-binary", "-out", str(out), str(src)], check=True)
    return out


def _hermes_string_ranges(path):
    """index -> (encoding label, byte length) as printed by Hermes' own reader."""
    r = subprocess.run([HERMES, "-b", "-dump-bytecode", str(path)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    out = {}
    for line in r.stdout.splitlines():
        m = _DUMP_RE.match(line)
        if m:
            out[int(m.group(1))] = (m.group(2), int(m.group(4)) - int(m.group(3)) + 1)
    return out


@pytest.fixture(scope="module")
def encodings_hbc(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("enc")
    js = "".join(f'var v{i} = "{s}";\n' for i, s in enumerate(CASES))
    js += "print(" + ", ".join(f"v{i}.length" for i in range(len(CASES))) + ");\n"
    return _compile(tmp, js)


def test_the_compiler_stores_ascii_or_utf16_and_hermit_reads_it(encodings_hbc):
    f = _libhbc.HBCFile.open(str(encodings_hbc))
    strings, entries = f.strings(), f.string_entries()
    ref = _hermes_string_ranges(encodings_hbc)

    for js, (utf16, units, expected) in CASES.items():
        idx = [i for i, s in enumerate(strings) if s == expected and entries[i].is_utf16 == utf16
               and entries[i].char_length == units]
        assert idx, f"{js!r}: no entry with is_utf16={utf16}, {units} code units, value {expected!r}"
        e = entries[idx[0]]
        assert e.byte_length == (units * 2 if utf16 else units)
        # Hermes' reader agrees on the kind of storage and on the size in bytes.
        label, nbytes = ref[idx[0]]
        assert (label, nbytes) == (("UTF-16" if utf16 else "ASCII"), e.byte_length)


def test_a_string_with_one_non_ascii_character_is_utf16_as_a_whole(encodings_hbc):
    f = _libhbc.HBCFile.open(str(encodings_hbc))
    caffe = [e for s, e in zip(f.strings(), f.string_entries()) if s == "caffè"][0]
    assert caffe.is_utf16 and caffe.char_length == 5      # the four ASCII letters too


# --- characters the patcher accepts ------------------------------------------------

ASCII_ENTRY = SimpleNamespace(is_utf16=False, byte_length=3)
UTF16_ENTRY = SimpleNamespace(is_utf16=True, byte_length=10)      # five code units


def test_ascii_entry_accepts_ascii_only():
    assert encode_string_value("xyz", ASCII_ENTRY) == b"xyz"
    assert encode_string_new_value("a longer value", ASCII_ENTRY) == b"a longer value"
    for bad in ("caffè", "x\U0001F600"):
        with pytest.raises(_StrEditError, match="non-ASCII"):
            encode_string_value(bad, ASCII_ENTRY)


def test_ascii_entry_same_length_edit_checks_bytes():
    with pytest.raises(_StrEditError, match="bytes"):
        encode_string_value("xy", ASCII_ENTRY)


def test_utf16_entry_accepts_accented_characters_two_bytes_each():
    enc = encode_string_value("caffù", UTF16_ENTRY)
    assert enc == b"c\x00a\x00f\x00f\x00\xf9\x00"
    assert len(enc) == UTF16_ENTRY.byte_length                     # 5 code units
    # ASCII on a UTF-16 entry is still two bytes per character.
    assert encode_string_new_value("abc", UTF16_ENTRY) == b"a\x00b\x00c\x00"


def test_utf16_entry_refuses_supplementary_characters_and_isolated_surrogates():
    with pytest.raises(_StrEditError, match="outside the BMP"):
        encode_string_new_value("x\U0001F600", UTF16_ENTRY)
    for lone in ("\ud800", "a\udc00b"):
        with pytest.raises(_StrEditError, match="isolated surrogate"):
            encode_string_new_value(lone, UTF16_ENTRY)


def test_new_strings_are_ascii_only():
    assert encode_new_string("hello") == b"hello"
    for bad in ("caffè", "\U0001F600", ""):
        with pytest.raises(_StrEditError):
            encode_new_string(bad)


# --- end to end ----------------------------------------------------------------------

def _run(path):
    r = subprocess.run([HERMES, str(path)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


@pytest.mark.parametrize("new_value", ["caffù", "più caffè con crema"])
def test_edited_accented_string_runs_on_the_vm(tmp_path, new_value):
    src = _compile(tmp_path, 'var s = "caff\\u00e8"; print(s);\n')
    assert _run(src) == "caffè"

    f = _libhbc.HBCFile.open(str(src))
    idx = [i for i, s in enumerate(f.strings()) if s == "caffè"][0]
    entry = f.string_entries()[idx]
    assert entry.is_utf16 and entry.char_length == 5

    m = _libhbc.Module.from_hbcfile(f)
    m.set_string_value(idx, encode_string_new_value(new_value, entry))
    out = tmp_path / "out.hbc"
    out.write_bytes(bytes(m.serialize()))

    g = _libhbc.HBCFile.open(str(out))
    assert g.strings()[idx] == new_value
    assert g.string_entries()[idx].char_length == len(new_value)    # BMP: units == characters
    assert _run(out) == new_value
