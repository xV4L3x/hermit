"""`edit func:new` - adding a whole function.

Restricted scope (decided with the user, 2026-09-14): ADDITION ONLY. The
clone-on-edit of a shared body is out of scope. The new function's metadata is fixed (paramCount=1,
frameSize auto from the max register used, empty name, strict, prohibitInvoke
= None (2) - see the comment in `core/src/patcher/module.cpp`).

`edit func:new` opens the TRADITIONAL instruction editor (`EditorState`/
`run_editor`, the same as `edit func:N`) on a seed buffer
(`LoadConstUndefined r0` / `Ret r0`), not the single-field style of
`edit string:`. On commit, `HermitProject.apply_add_function` appends the
function; to reference it a `CreateClosure` must be manually inserted in
another function.
"""

import hashlib

import pytest

import _libhbc
from cli.editor import (
    EditorState,
    assemble_committed_function,
    new_function_seed_instrs,
    new_function_stub,
)
from testutil import (
    CORPUS,
    CX,
    ENTER,
    HERMES,
    RIGHT,
    drive_instructions,
    load_project,
    run_hermes,
)


def _new_func_state(proj):
    opcodes = _libhbc.opcode_table(proj.hbc.version())
    return EditorState(
        new_function_stub(), new_function_seed_instrs(opcodes),
        strings=proj.hbc.strings(), opcodes=opcodes, version=proj.hbc.version(),
    )


# --- edit func:new - headless editor ----------------------------------


def test_new_function_stub_and_seed_shape():
    """The stub + the seed buffer have exactly the attributes that
    EditorState/_build_lines read (index=-1 sentinel, frameSize=1,
    no EH/shared)."""
    proj = load_project("hello.hbc")
    opcodes = _libhbc.opcode_table(proj.hbc.version())
    stub = new_function_stub()
    assert stub.index == -1
    assert stub.frameSize == 1
    assert stub.hasExceptionHandler is False
    assert stub.shared is False

    seed = new_function_seed_instrs(opcodes)
    assert [i.mnemonic for i in seed] == ["LoadConstUndefined", "Ret"]

    state = _new_func_state(proj)
    assert len(state.nav_indices) == 2
    assert state.structural_change is False   # intact buffer: no insert/delete
    assert state.dirty_count == 0


def test_pristine_seed_exits_without_commit():
    """Ctrl+X on a buffer NEVER touched exits without a prompt (same quit-guard
    as the other editors) - commit False, no function created."""
    proj = load_project("hello.hbc")
    state = _new_func_state(proj)
    drive_instructions(state, CX)
    assert state.commit is False
    assert proj.is_patched is False


def test_insert_instruction_then_commit_creates_function():
    proj = load_project("hello.hbc")
    n0 = proj.hbc.header().function_count
    state = _new_func_state(proj)

    # insert "Mov r0,r0" below the first line (LoadConstUndefined) -> between the
    # seed's two instructions. Same key sequence as the insert test.
    keys = ("o" + "Mov" + ENTER + ENTER + "r0" + ENTER + RIGHT + ENTER + "r0" +
            ENTER + CX + "y")
    drive_instructions(state, keys)
    assert state.commit is True
    assert state.structural_change is True

    fidx, asm, frame, warnings, edit_all = assemble_committed_function(state)
    assert fidx == -1          # sentinel: stub, never used to index anything
    assert len(asm) == 3
    assert frame == 1
    assert warnings == []

    idx = proj.apply_add_function(asm, frame)
    assert idx == n0
    assert proj.is_patched is True
    assert proj.hbc.header().function_count == n0 + 1

    ins = proj.hbc.disasm(idx)
    assert [x.mnemonic for x in ins] == ["LoadConstUndefined", "Mov", "Ret"]


def test_apply_add_function_frame_size_auto_from_max_register():
    """frameSize isn't passed by the user: assemble_committed_function
    computes it from the highest register used in the buffer (risk #2 of the
    plan, already closed - pure reuse, zero new code)."""
    proj = load_project("hello.hbc")
    state = _new_func_state(proj)
    # r5 in a new Mov -> expected frameSize 6, not 1.
    keys = ("o" + "Mov" + ENTER + ENTER + "r5" + ENTER + RIGHT + ENTER + "r0" +
            ENTER + CX + "y")
    drive_instructions(state, keys)
    _fidx, asm, frame, _w, _ea = assemble_committed_function(state)
    assert frame == 6
    idx = proj.apply_add_function(asm, frame)
    assert proj.hbc.functions()[idx].frameSize == 6


# --- REPL glue ---------------------------------------------------------


def test_repl_finish_add_function_via_state():
    """Mirror of test_edit_string_new_via_repl: drives the state without
    going through the real interactive run_editor inside _edit_func_new."""
    from cli.repl import HermitREPL

    proj_path = CORPUS / "hello.hbc"
    if not proj_path.exists():
        pytest.skip("missing corpus: hello.hbc")
    r = HermitREPL()
    r._open_file(str(proj_path))
    proj = r.project
    n0 = proj.hbc.header().function_count

    state = _new_func_state(proj)
    keys = ("o" + "Mov" + ENTER + ENTER + "r0" + ENTER + RIGHT + ENTER + "r0" +
            ENTER + CX + "y")
    drive_instructions(state, keys)
    assert state.commit is True

    r._finish_add_function(proj, state)
    assert proj.is_patched is True
    assert proj.hbc.header().function_count == n0 + 1


def test_cmd_edit_func_new_routes_to_finish_add_function(monkeypatch):
    """`edit func:new` routes to _edit_func_new -> _finish_add_function,
    without going through the real interactive run_editor (monkeypatch, mirror
    of the style used to isolate the REPL logic from the real terminal)."""
    from cli.repl import HermitREPL

    proj_path = CORPUS / "hello.hbc"
    if not proj_path.exists():
        pytest.skip("missing corpus: hello.hbc")
    r = HermitREPL()
    r._open_file(str(proj_path))
    proj = r.project
    n0 = proj.hbc.header().function_count

    committed_state = _new_func_state(proj)
    keys = ("o" + "Mov" + ENTER + ENTER + "r0" + ENTER + RIGHT + ENTER + "r0" +
            ENTER + CX + "y")
    drive_instructions(committed_state, keys)
    assert committed_state.commit is True

    import cli.editor as editor_mod
    monkeypatch.setattr(editor_mod, "run_editor", lambda *a, **kw: committed_state)

    r.cmd_edit(["func:new"])
    assert proj.is_patched is True
    assert proj.hbc.header().function_count == n0 + 1


# --- rejections ------------------------------------------------------------


def test_apply_add_function_rejects_frame_size_over_127():
    proj = load_project("hello.hbc")
    state = _new_func_state(proj)
    drive_instructions(state, CX)   # pristine, doesn't even need a commit for this test
    _fidx, asm, _frame, _w, _ea = assemble_committed_function(state)
    with pytest.raises(ValueError, match="127"):
        proj.apply_add_function(asm, 128)
    assert proj.is_patched is False


def test_apply_add_function_rejects_switchimm():
    proj = load_project("hello.hbc")
    switch_op = next(d for d in _libhbc.opcode_table(proj.hbc.version())
                     if d.mnemonic == "SwitchImm")
    with pytest.raises(ValueError):
        proj.apply_add_function([(switch_op.opcode, [], -1, -1, -1)], 1)
    assert proj.is_patched is False


# --- real bundle -----------------------------------------------------


def test_add_function_on_real_bundle():
    proj = load_project("ridemovi.hbc")
    n0 = proj.hbc.header().function_count
    g0 = proj.hbc.header().global_code_index

    opcodes = _libhbc.opcode_table(proj.hbc.version())
    seed_asm = [(i.opcode, [float(i.operands[0].u32)], -1, -1, i.offset)
               for i in new_function_seed_instrs(opcodes)]
    idx = proj.apply_add_function(seed_asm, 1)

    assert idx == n0
    assert proj.hbc.header().function_count == n0 + 1
    assert proj.hbc.header().global_code_index == g0
    assert [x.mnemonic for x in proj.hbc.disasm(idx)] == ["LoadConstUndefined", "Ret"]


# --- hermes oracle -------------------------------------------------------


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
def test_add_function_hermes_oracle(tmp_path):
    """add_function (creates a new idx) + set_function_body on `global` to
    re-point the existing CreateClosure (which created `g`, returns
    "Hello, world") toward the new function (returns `undefined`) - two
    separate commits (mutually exclusive with set_function_body in the
    same serialize). `hermes` must print "undefined", no longer
    "Hello, world"."""
    proj = load_project("greeting_g0.hbc")
    r0 = run_hermes(proj.hbc_path)
    if r0.returncode != 0 or "Hello, world" not in r0.stdout:
        pytest.skip("greeting_g0.hbc: unexpected oracle behavior")

    opcodes = _libhbc.opcode_table(proj.hbc.version())
    seed_asm = [(i.opcode, [float(i.operands[0].u32)], -1, -1, i.offset)
               for i in new_function_seed_instrs(opcodes)]
    new_idx = proj.apply_add_function(seed_asm, 1)

    # Separate step: find CreateClosure in `global` (idx 0) again and re-point
    # its funcIdx operand to the new function.
    instrs = proj.hbc.disasm(0)
    create_closure = next((i for i in instrs if i.mnemonic == "CreateClosure"), None)
    if create_closure is None:
        pytest.skip("greeting_g0.hbc: no CreateClosure in global (unexpected layout)")

    def to_asm(ins):
        vals = [float(o.u32) for o in ins.operands]
        if ins.offset == create_closure.offset:
            vals[2] = float(new_idx)
        return (ins.opcode, vals, -1, -1, ins.offset)

    new_global_ir = [to_asm(i) for i in instrs]
    frame0 = proj.hbc.functions()[0].frameSize
    proj.apply_insertion(0, new_global_ir, frame0)

    out = tmp_path / "add_function_oracle.hbc"
    proj.write_patched(str(out))
    r1 = run_hermes(out)
    assert r1.returncode == 0
    assert "undefined" in r1.stdout
    assert "Hello, world" not in r1.stdout


# --- ProhibitInvoke: flags bits 0-1 are a 2-bit enum ------------------


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
@pytest.mark.parametrize("value, call_out, new_out", [
    (0, "call ERR: Class constructor invoked without new", "new ok: 2"),   # ProhibitCall
    (1, "call ok: 2", "new ERR: Function is not a constructor"),          # ProhibitConstruct
    (2, "call ok: 2", "new ok: 2"),                                       # ProhibitNone
    (3, "call ok: 2", "new ok: 2"),                                       # == isConstructCall never true
])
def test_prohibit_invoke_hermes_oracle(tmp_path, value, call_out, new_out):
    """Explains why add_function must not leave flags bits 0-1 at 0: they
    are Hermes' 2-bit ProhibitInvoke enum (0 = Call prohibited, 1 = Construct
    prohibited, 2 = None), not two independent prohibitCall/prohibitConstruct
    bits. Forces each value on `plainDecl` and checks the real VM's behaviour
    on both `plainDecl(1)` and `new plainDecl(2)`, plus hermit's decoding."""
    proj = load_project("invoke_kinds.hbc")
    idx = next(f.index for f in proj.hbc.functions() if f.name == "plainDecl")
    image = bytearray(proj.image_bytes)
    pos = 128 + idx * 16 + 15                       # SmallFuncHeader.flags
    image[pos] = (image[pos] & ~0x3) | value
    image[-20:] = hashlib.sha1(bytes(image[:-20])).digest()
    out = tmp_path / f"invoke_{value}.hbc"
    out.write_bytes(bytes(image))

    fi = _libhbc.HBCFile.from_bytes(bytes(image)).functions()[idx]
    assert fi.prohibitCall == (value == 0)
    assert fi.prohibitConstruct == (value == 1)

    r = run_hermes(out)
    assert r.returncode == 0, r.stderr
    lines = r.stdout.splitlines()
    assert lines[0] == call_out
    assert lines[1] == new_out
