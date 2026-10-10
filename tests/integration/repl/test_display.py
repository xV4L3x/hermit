"""CLI display formatting (`cli/display.py`), unit-level.

Pure functions (`functions_to_ansi_rows` et al.) don't need the REPL/viewer
scaffolding used by test_repl.py - call them directly on an opened HBCFile.

Covers: the "show-source" flag added to the `functions` command list when a
function carries Hermes' "show source" directive (Function Source Table,
see the function-table implementation notes).
"""

import _libhbc
from cli import display
from testutil import corpus_path


def _open(name: str) -> "_libhbc.HBCFile":
    return _libhbc.HBCFile.open(str(corpus_path(name)))


def _row_for(rows: list[str], name: str) -> str:
    for r in rows:
        if name in r:
            return r
    raise AssertionError(f"no row for function {name!r} in {rows!r}")


def test_show_source_flag_only_on_real_show_source_functions():
    hbc = _open("show_source.hbc")
    rows = display.functions_to_ansi_rows(hbc.functions(), hbc.header().global_code_index)

    # withComment and arrowShow both use "show source" -> flagged.
    assert "show-source" in _row_for(rows, "withComment")
    assert "show-source" in _row_for(rows, "arrowShow")

    # hide source / sensitive / no directive at all -> NOT flagged. The flag
    # means "real source recoverable", not "this function is in the table".
    assert "show-source" not in _row_for(rows, "hidden")
    assert "show-source" not in _row_for(rows, "sensitiveFn")
    assert "show-source" not in _row_for(rows, "normal")


def test_show_source_flag_absent_when_no_directives_used():
    # hello.hbc doesn't use the directive anywhere -> functionSourceCount == 0,
    # no row should ever carry "show-source".
    hbc = _open("hello.hbc")
    rows = display.functions_to_ansi_rows(hbc.functions(), hbc.header().global_code_index)
    assert all("show-source" not in r for r in rows)
