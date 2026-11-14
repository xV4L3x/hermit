"""`frameSize` is a MINIMUM, not an arbitrary value (fuzzing bug report, 2026-11-14).

`set_function_body`/`add_function` used to trust the caller's `frameSize`
verbatim, checking it only against the 7-bit cap and the exception-handler
growth guard. A body that inserts a `Call*`/`Construct*` needs extra registers
reserved at the TOP of the frame for the callee's call-setup window
(`StackFrameLayout::CallerExtraRegistersAtEnd` + `numArgsIncludingThis`, exactly
how `hermesc` computes a function's `frameSize` - see
`core/src/patcher/module.cpp::computeMinSafeFrameSize`); an undersized
`frameSize` let the VM overlap that window with the caller's own live
registers - undefined reads or SIGSEGV, non-monotonically depending on the
exact value. `set_function_body`/`add_function` now raise `frameSize` to the
safe minimum instead of trusting it, and report the bump via `warnings()`.
"""

import pytest

import _libhbc
from testutil import CORPUS, HERMES, run_hermes


def _opcode_table():
    return {d.mnemonic: d.opcode for d in _libhbc.opcode_table(96)}


def _load(name):
    path = CORPUS / name
    if not path.exists():
        pytest.skip(f"missing corpus: {name}")
    return _libhbc.HBCFile.open(str(path))


def _call2_ir(op, target_func_idx, string_idx=0):
    """CreateEnvironment r0; CreateClosure r1,r0,target; LoadConstStringLongIndex
    r2,string_idx; LoadConstUndefined r3; Call2 r4,r1,r3,r2; Ret r3. Max register
    used = r4; one Call2 (1 arg excl. `this`). Exactly the bug report's repro
    shape (Shopify Inbox harness), on a function we control (greeting_g0.hbc)."""
    return [
        (op("CreateEnvironment"), [0], -1, -1, -1),
        (op("CreateClosure"), [1, 0, target_func_idx], -1, -1, -1),
        (op("LoadConstStringLongIndex"), [2, string_idx], -1, -1, -1),
        (op("LoadConstUndefined"), [3], -1, -1, -1),
        (op("Call2"), [4, 1, 3, 2], -1, -1, -1),
        (op("Ret"), [3], -1, -1, -1),
    ]


# The real formula: max register used (r4 -> 5) + CallerExtraRegistersAtEnd (6)
# + numArgsIncludingThis (2 for Call2: `this` + 1 arg) = 13.
_SAFE_MIN = 13


class TestSetFunctionBody:

    @pytest.mark.parametrize("requested", [1, 5, 6, 7, 8, 9, 10, 12])
    def test_undersized_frame_size_is_bumped_and_warned(self, requested):
        f = _load("greeting_g0.hbc")
        m = _libhbc.Module.from_hbcfile(f)
        op = _opcode_table().__getitem__
        m.set_function_body(0, _call2_ir(op, target_func_idx=1), requested)
        m.serialize()
        assert m.warnings() == [
            f"function 0: frameSize raised from {requested} to {_SAFE_MIN} - the "
            "requested value did not cover the registers/call-argument window "
            "used by the new body"
        ]

    @pytest.mark.parametrize("requested", [_SAFE_MIN, 19, 127])
    def test_already_sufficient_frame_size_is_unchanged(self, requested):
        f = _load("greeting_g0.hbc")
        m = _libhbc.Module.from_hbcfile(f)
        op = _opcode_table().__getitem__
        m.set_function_body(0, _call2_ir(op, target_func_idx=1), requested)
        m.serialize()
        assert m.warnings() == []

    def test_bump_past_127_still_rejected(self):
        """The safe minimum can itself exceed the 7-bit cap - the cap check
        applies to the bumped value, not the (possibly tiny) requested one."""
        f = _load("greeting_g0.hbc")
        m = _libhbc.Module.from_hbcfile(f)
        op = _opcode_table().__getitem__
        ir = _call2_ir(op, target_func_idx=1)
        # Force the max register above 127 - overhead so the safe minimum itself
        # overflows the 7-bit field, even though `requested` is tiny.
        ir.append((op("Mov"), [120, 0], -1, -1, -1))
        with pytest.raises(RuntimeError, match="127"):
            m.set_function_body(0, ir, 1)


@pytest.mark.skipif(HERMES is None, reason="hermes not available")
class TestHermesOracle:
    """The exact repro from the bug report: Call2, max register r4, frame_size
    below the real minimum used to produce `undefined is not a function`
    (register corruption) or SIGSEGV, depending on the exact value. After the
    fix every one of these frame_size values round-trips through `hermes`
    without error, because the Module auto-bumps to the safe minimum."""

    @pytest.mark.parametrize("requested", [1, 5, 6, 7, 8, 9, 10, 19, 127])
    def test_patched_module_runs_clean_on_real_vm(self, tmp_path, requested):
        f = _load("greeting_g0.hbc")
        m = _libhbc.Module.from_hbcfile(f)
        op = _opcode_table().__getitem__
        m.set_function_body(0, _call2_ir(op, target_func_idx=1), requested)
        data = m.serialize()

        out = tmp_path / f"frametest_{requested}.hbc"
        out.write_bytes(bytes(data))
        r = run_hermes(out)
        assert r.returncode == 0, (requested, r.stdout, r.stderr)
        assert "TypeError" not in r.stderr
        assert "is not a function" not in r.stderr


class TestAddFunction:

    def test_undersized_frame_size_is_bumped_and_warned(self):
        f = _load("greeting_g0.hbc")
        m = _libhbc.Module.from_hbcfile(f)
        op = _opcode_table().__getitem__
        m.add_function(_call2_ir(op, target_func_idx=1), 1)
        m.serialize()
        assert m.warnings() == [
            f"add_function: frameSize raised from 1 to {_SAFE_MIN} - the "
            "requested value did not cover the registers/call-argument window "
            "used by the new body"
        ]

    def test_already_sufficient_frame_size_is_unchanged(self):
        f = _load("greeting_g0.hbc")
        m = _libhbc.Module.from_hbcfile(f)
        op = _opcode_table().__getitem__
        m.add_function(_call2_ir(op, target_func_idx=1), _SAFE_MIN)
        m.serialize()
        assert m.warnings() == []

    def test_no_registers_no_calls_keeps_requested_value(self):
        """Regression guard: a body with no registers/calls must not have its
        frameSize forced to a nonzero floor (computeMinSafeFrameSize returns 0,
        never lowering an otherwise-valid value)."""
        f = _load("greeting_g0.hbc")
        m = _libhbc.Module.from_hbcfile(f)
        op = _opcode_table().__getitem__
        ir = [(op("LoadConstUndefined"), [0], -1, -1, -1), (op("Ret"), [0], -1, -1, -1)]
        idx = m.add_function(ir, 1)
        m.serialize()
        assert m.warnings() == []
        assert idx == f.header().function_count
