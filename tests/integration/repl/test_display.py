"""CLI display formatting (`cli/display.py`), unit-level.

Pure functions (`functions_to_ansi_rows` et al.) don't need the REPL/viewer
scaffolding used by test_repl.py - call them directly on an opened HBCFile.

Covers: the "show-source" flag added to the `functions` command list when a
function carries Hermes' "show source" directive (Function Source Table,
see the function-table implementation notes).
"""

import _libhbc
from rich.console import Console
from cli import display
from testutil import corpus_path


def _render(table) -> str:
    """Render a rich Table to plain text at a width wide enough that no
    column wraps, so cell content can be asserted on directly."""
    console = Console(width=300, legacy_windows=False)
    with console.capture() as capture:
        console.print(table)
    return capture.get()


def _open(name: str) -> "_libhbc.HBCFile":
    return _libhbc.HBCFile.open(str(corpus_path(name)))


def _row_for(rows: list[str], name: str) -> str:
    for r in rows:
        if name in r:
            return r
    raise AssertionError(f"no row for function {name!r} in {rows!r}")


def _row_for_index(rows: list[str], index: int) -> str:
    needle = f"[{index:5d}]"
    for r in rows:
        if needle in r:
            return r
    raise AssertionError(f"no row for function index {index} in {rows!r}")


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


def _cjs_module_paths(hbc) -> dict:
    return {cm.function_index: cm.module_path for cm in hbc.cjs_modules()}


def test_cjs_module_tag_shows_resolved_path():
    # cjs_modules.hbc: main.js require()s foo.js, native `-commonjs` mode
    # (header.cjsModuleCount == 2) -- never the Metro `__d()` case. Every
    # wrapper function is literally named "cjs_module" in the function
    # table, so the path tag is the only way to tell them apart.
    hbc = _open("cjs_modules.hbc")
    cjs_module_paths = _cjs_module_paths(hbc)
    assert len(cjs_module_paths) == 2

    rows = display.functions_to_ansi_rows(
        hbc.functions(), hbc.header().global_code_index,
        cjs_module_paths=cjs_module_paths,
    )

    main_idx = next(i for i, p in cjs_module_paths.items() if p == "main.js")
    foo_idx = next(i for i, p in cjs_module_paths.items() if p == "foo.js")
    assert "module 'main.js'" in _row_for_index(rows, main_idx)
    assert "module 'foo.js'" in _row_for_index(rows, foo_idx)

    # The global (top-level) function is never a CJS module itself.
    global_idx = hbc.header().global_code_index
    assert global_idx not in cjs_module_paths
    assert "module '" not in _row_for_index(rows, global_idx)


def test_cjs_module_tag_absent_on_non_commonjs_file():
    # hello.hbc has header.cjsModuleCount == 0 -> no tag anywhere, same as a
    # Metro-bundled file (recover_modules() and cjs_modules() are mutually
    # exclusive in every real sample, see doc/hbc-format/v96-format.md §9).
    hbc = _open("hello.hbc")
    assert hbc.cjs_modules() == []
    rows = display.functions_to_ansi_rows(
        hbc.functions(), hbc.header().global_code_index, cjs_module_paths={},
    )
    assert all("module '" not in r for r in rows)


def test_info_table_shows_cjs_modules_when_present():
    # cjs_modules.hbc: header.cjsModuleCount == 2 -> "CJS modules" row, no
    # "Metro modules" row (the two mechanisms never coexist in practice).
    hbc = _open("cjs_modules.hbc")
    out = _render(display.make_info_table(hbc))
    assert "CJS modules" in out
    assert "2" in out.split("CJS modules")[1].splitlines()[0]
    assert "Metro modules" not in out


def test_info_table_shows_metro_modules_otherwise():
    # No CJS Module Table on this file -> falls back to "Metro modules"
    # (recover_modules(), the __d() pattern -- what real RN bundles use),
    # even when the count is 0.
    hbc = _open("hello.hbc")
    out = _render(display.make_info_table(hbc))
    assert "Metro modules" in out
    assert "CJS modules" not in out


def test_info_table_source_file_paths_are_not_truncated():
    # Source file(s) rows must show the full path, one per line, never a
    # "…"-elided version (regression: _short_path used to crop at 60 chars).
    hbc = _open("hello.hbc")
    src_files = hbc.source_files()
    assert src_files, "hello.hbc should carry at least one source file"

    out = _render(display.make_info_table(hbc))
    assert "…" not in out
    for sf in src_files:
        assert sf in out


def test_info_table_shows_each_source_file_on_its_own_line():
    # cjs_modules.hbc is a genuine debugInfoHeader.filenameCount == 2 file
    # (main.js + foo.js) -- regression for the source_files() bug where the
    # two names, not NUL-separated on disk, were concatenated into one
    # garbled entry instead of being decoded as two.
    hbc = _open("cjs_modules.hbc")
    src_files = hbc.source_files()
    assert len(src_files) == 2
    assert src_files[0] != src_files[1]

    out = _render(display.make_info_table(hbc))
    lines = out.splitlines()
    line0 = next(l for l in lines if src_files[0] in l)
    line1 = next(l for l in lines if src_files[1] in l)
    assert line0 != line1
    # Neither file's full path leaked into the other's row.
    assert src_files[1] not in line0
    assert src_files[0] not in line1
