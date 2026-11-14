"""`arrays` command: lists array/object literal sites."""

from cli import display


class ArraysCommand:
    def cmd_arrays(self, args: list[str]) -> None:
        """arrays [--arrays|--objects] [filter:<text>] [func:<N|name>]  - lists array/object literal sites"""
        proj = self._require_project()
        filter_text, kind_filter, func_arg = self._parse_listing_args(
            args, {"--arrays": "array", "--objects": "object"})

        func_filter = None
        if func_arg is not None:
            func_filter = self._resolve_func_arg(func_arg).index

        sites = proj.hbc.literal_sites()
        rows  = display.arrays_to_ansi_rows(
            sites, proj.hbc.array_buffer(), proj.hbc.obj_key_buffer(),
            proj.hbc.obj_value_buffer(), proj.hbc.strings(), proj.hbc.functions(),
            filter_text, kind_filter, func_filter,
        )
        title = f"arrays  {len(rows)}/{len(sites)}"
        if kind_filter:
            title += f"  [{kind_filter}s only]"
        if func_filter is not None:
            title += f"  func:{func_filter}"
        if filter_text:
            title += f"  filter={filter_text!r}"
        self._show_rows(rows, title)
