"""the structural patcher on HBC 94, 95 and 96.

`BytecodeFileFormat.h` is identical in v94/v95/v96 and `Module` only picks the
opcode table from the header version, so every patcher operation should work
unchanged on the three versions. These tests check it end to end: each sample
is compiled with the `hermes` of that version (`-g0`), patched through
`HermitProject` (same path as the REPL: `Module` -> `serialize` -> re-parse ->
invariants), written out and executed by the VM **of the same version**.

Compilers: `hermes` on PATH for 96; for 94/95 `$HERMES94` / `$HERMES95`, or
`~/.local/share/hermit/hermes/hermes9{4,5}` (built from the Hermes commits that
introduced each version - see `doc/hbc-format/bytecode-versions.md`). A missing
compiler skips its version.
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

import _libhbc
from cli.literals import encode_slp_sequence, decode_slp, reencode_value_bytes
from cli.project import HermitProject

SAMPLES = Path(__file__).resolve().parents[3] / "seeder" / "samples"
_LOCAL = Path.home() / ".local" / "share" / "hermit" / "hermes"


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


def _compile(version, sample, tmp_path):
    out = tmp_path / f"{Path(sample).stem}_v{version}.hbc"
    r = subprocess.run([_compiler(version), "-emit-binary", "-g0", "-out", str(out),
                        str(SAMPLES / sample)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert _libhbc.HBCFile.open(str(out)).version() == version
    return out


def _run(version, path):
    r = subprocess.run([_compiler(version), str(path)], capture_output=True, text=True,
                       timeout=30)
    assert r.returncode == 0, r.stderr
    return r.stdout.splitlines()


def _write(proj, tmp_path, name):
    out = tmp_path / name
    proj.write_patched(str(out))
    return out


def _func(proj, name):
    return next(f for f in proj.hbc.functions() if f.name == name)


def _operand_value(o):
    T = _libhbc.OperandType
    if o.type in (T.Imm32, T.Addr8, T.Addr32):
        return float(o.i32)
    if o.type == T.Double:
        return o.f64
    return float(o.u32)


def _ir(proj, fidx):
    """Disassembly -> `apply_insertion` tuples, jump targets as INDICES."""
    descs = {d.opcode: d for d in _libhbc.opcode_table(proj.hbc.version())}
    ins = proj.hbc.disasm(fidx)
    off2idx = {x.offset: k for k, x in enumerate(ins)}
    ir = []
    for x in ins:
        tgt, pos = -1, -1
        if descs[x.opcode].jump_kind in ("Unconditional", "Conditional"):
            pos = 0
            tgt = off2idx[x.offset + x.operands[0].i32]
        ir.append([x.opcode, [_operand_value(o) for o in x.operands], tgt, pos, x.offset])
    return ir


def _insert_nops(ir, at, count, opcode_mov):
    for t in ir:
        if t[2] >= at:
            t[2] += count
    for _ in range(count):
        ir.insert(at, [opcode_mov, [0.0, 0.0], -1, -1, -1])
    return [tuple(t) for t in ir]


def _mov(proj):
    return next(d.opcode for d in _libhbc.opcode_table(proj.hbc.version())
                if d.mnemonic == "Mov")


# --- identity ------------------------------------------------------------


@pytest.mark.parametrize("version", VERSIONS)
@pytest.mark.parametrize("sample", ["try_catch.js", "greeting.js", "literals.js",
                                    "control_flow.js", "invoke_kinds.js"])
def test_module_identity(version, sample, tmp_path):
    """No pending edit -> `Module.serialize()` is byte-identical to the input."""
    path = _compile(version, sample, tmp_path)
    f = _libhbc.HBCFile.open(str(path))
    m = _libhbc.Module.from_hbcfile(f)
    assert bytes(m.serialize()) == path.read_bytes()


# --- instructions: insertion, EH relocation, branch relaxation -----------


@pytest.mark.parametrize("version", VERSIONS)
def test_insertion_relocates_eh(version, tmp_path):
    path = _compile(version, "try_catch.js", tmp_path)
    assert _run(version, path) == ["10", "-1"]
    proj = HermitProject.from_hbc(str(path))
    risky = _func(proj, "risky")
    eh0 = [(h.start, h.end, h.target) for h in risky.exceptionHandlers]
    assert eh0

    ir = _insert_nops(_ir(proj, risky.index), 1, 1, _mov(proj))
    proj.apply_insertion(risky.index, ir, risky.frameSize)

    fi = proj.hbc.functions()[risky.index]
    assert fi.bytecodeSizeInBytes == risky.bytecodeSizeInBytes + 3
    moved = lambda v: v if v == 0 else v + 3
    assert [(h.start, h.end, h.target) for h in fi.exceptionHandlers] == \
        [(moved(s), moved(e), moved(t)) for s, e, t in eh0]
    assert _run(version, _write(proj, tmp_path, "ins.hbc")) == ["10", "-1"]


@pytest.mark.parametrize("version", VERSIONS)
def test_branch_relaxation(version, tmp_path):
    """60 no-ops right after the first short forward jump of `risky`: the
    jump no longer fits Addr8 and must be promoted to its Long variant."""
    path = _compile(version, "try_catch.js", tmp_path)
    proj = HermitProject.from_hbc(str(path))
    risky = _func(proj, "risky")
    ins = proj.hbc.disasm(risky.index)
    j = next(k for k, x in enumerate(ins)
             if x.mnemonic.startswith("J") and not x.mnemonic.endswith("Long")
             and x.operands[0].i32 > 0)

    ir = _insert_nops(_ir(proj, risky.index), j + 1, 60, _mov(proj))
    proj.apply_insertion(risky.index, ir, risky.frameSize)

    after = [x.mnemonic for x in proj.hbc.disasm(risky.index)]
    assert after[j] == ins[j].mnemonic + "Long"
    assert _run(version, _write(proj, tmp_path, "relax.hbc")) == ["10", "-1"]


# --- strings -------------------------------------------------------------


@pytest.mark.parametrize("version", VERSIONS)
def test_string_relocation(version, tmp_path):
    """Longer string: append to the storage, repoint the entry, re-layout of
    every following section."""
    path = _compile(version, "greeting.js", tmp_path)
    assert _run(version, path) == ["Hello, world"]
    proj = HermitProject.from_hbc(str(path))
    idx = list(proj.hbc.strings()).index("Hello, world")
    new = f"Hello from a patched HBC {version} file"
    proj.apply_string_relocation(idx, new.encode("ascii"), new)
    assert _run(version, _write(proj, tmp_path, "str.hbc")) == [new]


@pytest.mark.parametrize("version", VERSIONS)
def test_add_string(version, tmp_path):
    path = _compile(version, "greeting.js", tmp_path)
    proj = HermitProject.from_hbc(str(path))
    n0 = len(proj.hbc.strings())
    idx = proj.apply_add_string("added by hermit")
    assert idx == n0
    assert proj.hbc.strings()[idx] == "added by hermit"
    assert _run(version, _write(proj, tmp_path, "addstr.hbc")) == ["Hello, world"]


# --- literals ------------------------------------------------------------


def _array_site(proj):
    site = proj.hbc.literal_sites()[0]
    vals = decode_slp(proj.image_bytes, proj.hbc.array_buffer_offset() + site.buffer_idx,
                      site.count)
    return site, vals


@pytest.mark.parametrize("version", VERSIONS)
def test_literal_relocation(version, tmp_path):
    """a = [10, 20, 30]; print(a[1]) - a[1] becomes 99."""
    path = _compile(version, "literals.js", tmp_path)
    assert _run(version, path) == ["20"]
    proj = HermitProject.from_hbc(str(path))
    site, vals = _array_site(proj)
    items = [(v.tag, reencode_value_bytes(v.tag, 99 if k == 1 else v.value))
             for k, v in enumerate(vals)]
    proj.apply_array_relocation(site.function_index, site.instruction_offset, False,
                                encode_slp_sequence(items), 0, {1})
    assert _run(version, _write(proj, tmp_path, "lit.hbc")) == ["99"]


@pytest.mark.parametrize("version", VERSIONS)
def test_literal_resize(version, tmp_path):
    """a = [10, 20, 30]; print(a.length) - append 40."""
    path = _compile(version, "literals_resize.js", tmp_path)
    assert _run(version, path) == ["3"]
    proj = HermitProject.from_hbc(str(path))
    site, vals = _array_site(proj)
    items = [(v.tag, reencode_value_bytes(v.tag, v.value)) for v in vals]
    items.append((vals[-1].tag, reencode_value_bytes(vals[-1].tag, 40)))
    proj.apply_array_resize(site.function_index, site.instruction_offset, False, True,
                            encode_slp_sequence(items), b"", 0)
    assert _run(version, _write(proj, tmp_path, "resize.hbc")) == ["4"]


# --- add_function --------------------------------------------------------


@pytest.mark.parametrize("version", VERSIONS)
def test_add_function(version, tmp_path):
    """New function returning undefined; `global`'s CreateClosure for `g` is
    repointed to it (second commit): the program prints "undefined"."""
    path = _compile(version, "greeting.js", tmp_path)
    proj = HermitProject.from_hbc(str(path))
    op = {d.mnemonic: d.opcode for d in _libhbc.opcode_table(version)}
    new_idx = proj.apply_add_function(
        [(op["LoadConstUndefined"], [0.0], -1, -1, -1), (op["Ret"], [0.0], -1, -1, -1)], 1)

    g = proj.hbc.header().global_code_index
    ir = _ir(proj, g)
    cc = next(k for k, x in enumerate(proj.hbc.disasm(g)) if x.mnemonic == "CreateClosure")
    ir[cc][1][2] = float(new_idx)
    proj.apply_insertion(g, [tuple(t) for t in ir], proj.hbc.functions()[g].frameSize)
    assert _run(version, _write(proj, tmp_path, "addfn.hbc")) == ["undefined"]
