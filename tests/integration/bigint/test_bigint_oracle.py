"""BigInt Table against the Hermes oracles and Python's int.

Oracles: (1) `hermes -b -dump-bytecode` prints `Global BigInt Table:` with each
entry's byte range and its decimal value (cut at 16 digits, then `... (N decimal
digits)`); (2) the VM prints every literal of bigint.js in full; (3) Python's
`int.from_bytes(bytes, "little", signed=True)` on the entry bytes.
Also the disassembly preview (`[bigint][id]`).
"""

import re
import subprocess

import _libhbc
import pytest

from cli import display
from cli.literal_preview import MAX_DIGITS, LiteralPreview
from testutil import HERMES, corpus_path

NAME = "bigint.hbc"


def _open():
    path = corpus_path(NAME)
    if not path.exists():
        pytest.skip(f"{NAME} not in the corpus")
    return path, _libhbc.HBCFile.open(str(path))


def _plain(s):
    return re.sub(r"\x1b\[[0-9;]*m", "", s)


def test_decimal_matches_python_int():
    _, f = _open()
    assert f.bigints()
    for b in f.bigints():
        assert b.ok
        assert int(b.decimal) == int.from_bytes(b.bytes, "little", signed=True)
        assert b.is_negative == (int(b.decimal) < 0)


@pytest.mark.skipif(HERMES is None, reason="hermes (HBC 96) not installed")
def test_decimal_matches_vm_output():
    path, f = _open()
    out = subprocess.run([HERMES, str(path)], capture_output=True, text=True, check=True).stdout.split()
    # one printed line per literal; the table holds each distinct value once
    assert sorted(set(out), key=out.index) == [b.decimal for b in f.bigints()]


@pytest.mark.skipif(HERMES is None, reason="hermes (HBC 96) not installed")
def test_entries_match_hermes_dump():
    path, f = _open()
    dump = subprocess.run([HERMES, "-b", "-dump-bytecode", str(path)],
                          capture_output=True, text=True, check=True).stdout
    lines = dump.split("Global BigInt Table:\n", 1)[1].split("\n\n", 1)[0].splitlines()
    assert len(lines) == len(f.bigints())
    for line, b in zip(lines, f.bigints()):
        if "[empty]" in line:
            assert b.length == 0
            continue
        m = re.match(r" (\d+)\[ (\d+)\[(\d+)\.\.(\d+)\]: (.*)$", line)
        assert m, line
        idx, _, end, start, text = int(m[1]), m[2], int(m[3]), int(m[4]), m[5]
        assert idx == b.index and start == b.offset and end == b.offset + b.length - 1
        cut = re.match(r"(\d{16})\.\.\. \((\d+) decimal digits\)$", text)
        if cut:
            assert b.decimal.startswith(cut[1]) and len(b.decimal) == int(cut[2])
        else:
            assert text == b.decimal


def test_disasm_shows_value_and_table_index():
    _, f = _open()
    pv = LiteralPreview(f)
    func = f.functions()[f.header().global_code_index]
    rows = [_plain(r) for r in display.disasm_to_ansi_rows(func, f.disasm(func.index), [], pv)]
    bigint_rows = [r for r in rows if "LoadConstBigInt" in r]
    assert any(r.endswith("127n [bigint][2]") for r in bigint_rows)
    assert any(r.endswith("65535n [bigint][6]") for r in bigint_rows)
    # 30 digits: cut after MAX_DIGITS with the hidden count, no trailing n
    cut = f"{'123456789012345678901234567890'[:MAX_DIGITS]}… +{30 - MAX_DIGITS} digits [bigint][10]"
    assert any(r.endswith(cut) for r in bigint_rows)
    # 0n is an empty entry
    assert any(r.endswith("0n [bigint][0]") for r in bigint_rows)


def test_other_instructions_have_no_bigint_preview():
    _, f = _open()
    pv = LiteralPreview(f)
    assert all(pv.render(i) is None for i in f.disasm(0) if "BigInt" not in i.mnemonic)


def _compiler(version):
    import os
    import shutil
    from pathlib import Path
    if version == 96:
        return shutil.which("hermes")
    env = os.environ.get(f"HERMES{version}")
    if env and Path(env).exists():
        return env
    p = Path.home() / ".local" / "share" / "hermit" / "hermes" / f"hermes{version}"
    return str(p) if p.exists() else None


@pytest.mark.parametrize("version,opt", [(94, "-O0"), (95, "-O0"), (96, "-O0"), (96, "-O")])
def test_same_table_on_every_version_and_optimization(tmp_path, version, opt):
    """The encoding is the same on HBC 94, 95 and 96, optimized or not."""
    cc = _compiler(version)
    if cc is None:
        pytest.skip(f"no compiler for HBC {version}")
    src = corpus_path(NAME).parents[1] / "samples" / "bigint.js"
    out = tmp_path / "b.hbc"
    subprocess.run([cc, opt, "-emit-binary", "-out", str(out), str(src)], check=True)
    f = _libhbc.HBCFile.open(str(out))
    assert f.header().version == version
    _, ref = _open()
    assert [b.decimal for b in f.bigints()] == [b.decimal for b in ref.bigints()]
    assert all(b.ok for b in f.bigints())
