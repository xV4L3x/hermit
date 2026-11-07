"""Literal preview in the disassembly view (`cli/literal_preview.py`) and the
literal reference text in `xrefs` rows (`cli/display.py`).

Corpus: xrefs_literals.hbc (seeder/samples/xrefs_literals.js) and
array_object_literals.hbc. Rows are compared as plain text (ANSI stripped)."""

import re

import _libhbc
from cli import display
from cli.literal_preview import MAX_BODY, LiteralPreview
from testutil import corpus_path


def _plain(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", s)


def _open(name):
    return _libhbc.HBCFile.open(str(corpus_path(name)))


def _disasm_rows(f, func_index):
    func = f.functions()[func_index]
    rows = display.disasm_to_ansi_rows(func, f.disasm(func_index), [], LiteralPreview(f))
    return [_plain(r) for r in rows]


def _row(rows, mnemonic, offset=None):
    for r in rows:
        if mnemonic in r and (offset is None or f"{offset:#06x}" in r):
            return r
    raise AssertionError(f"no {mnemonic} row in {rows!r}")


def test_array_preview_with_strings_and_offset_id():
    f = _open("xrefs_literals.hbc")
    row = _row(_disasm_rows(f, 0), "NewArrayWithBuffer", 0x38)
    assert row.endswith('["loca", "lho", "st"] [array][0]')
    # raw operands are kept
    assert "r1, 3, 3, 0" in row


def test_mixed_array_preview_shows_every_tag():
    f = _open("xrefs_literals.hbc")
    row = _row(_disasm_rows(f, 0), "NewArrayWithBuffer", 0x54)
    assert row.endswith('["mx1", 7, "mx2", true, null, "mx3"] [array][8]')


def test_object_preview_uses_bare_keys_and_both_offsets():
    f = _open("xrefs_literals.hbc")
    row = _row(_disasm_rows(f, 0), "NewObjectWithBuffer")
    assert row.endswith('{kx: "vx", ky: "vy", kz: "vz"} [object][0, 0]')


def test_number_and_bool_elements():
    f = _open("array_object_literals.hbc")
    rows = _disasm_rows(f, 0)
    assert _row(rows, "NewArrayWithBuffer").endswith(
        '[1, 2.5, "three", true, false, null] [array][0]')
    assert _row(rows, "NewObjectWithBuffer").endswith('{a: 1, b: "two", c: false} [object][0, 0]')


def test_long_literal_is_cut_with_hidden_count():
    f = _open("xrefs_literals.hbc")
    # first(): ["alpha", "beta", "gamma", "delta", "eps", "zeta"] does not fit
    row = _row(_disasm_rows(f, 1), "NewArrayWithBuffer")
    assert row.endswith('["alpha", "beta", "gamma", … +3] [array][21]')


def test_body_never_exceeds_the_limit():
    f = _open("ridemovi.hbc") if corpus_path("ridemovi.hbc").exists() else _open("hermitvault.hbc")
    pv = LiteralPreview(f)
    checked = 0
    for func in f.functions()[:1500]:
        for instr in f.disasm(func.index):
            text = pv.render(instr)
            if text is None:
                continue
            body = _plain(text).rsplit(" [", 1)[0]
            first_item_only = body.count(", ") == 0
            assert first_item_only or len(body) <= MAX_BODY, body
            checked += 1
    assert checked > 0


def test_non_literal_instruction_has_no_preview():
    f = _open("xrefs_literals.hbc")
    pv = LiteralPreview(f)
    assert all(pv.render(i) is None for i in f.disasm(0) if "WithBuffer" not in i.mnemonic)


def test_without_previewer_the_row_is_unchanged():
    f = _open("xrefs_literals.hbc")
    func = f.functions()[0]
    rows = display.disasm_to_ansi_rows(func, f.disasm(0), [])
    assert "[array]" not in _plain("\n".join(rows))


def test_xrefs_rows_tell_where_the_string_sits_in_the_literal():
    f = _open("xrefs_literals.hbc")
    funcs = f.functions()
    xs = {x.string_value: x for x in f.string_xrefs()}

    def rows(value):
        return [_plain(r) for r in display.xrefs_detail_to_ansi_rows([xs[value]], funcs)]

    assert any("(NewArrayWithBuffer, element 1)" in r for r in rows("lho"))
    assert any("(NewArrayWithBuffer, element 0 x2)" in r for r in rows("dup"))
    assert any("(NewObjectWithBuffer, key 0)" in r for r in rows("kx"))
    assert any("(NewObjectWithBuffer, value 2)" in r for r in rows("vz"))
    # operand reference unchanged
    assert any("(LoadConstString)" in r for r in rows("beta"))
