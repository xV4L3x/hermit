"""`strings` command: lists strings with their kind."""

from cli import display


class StringsCommand:
    def cmd_strings(self, args: list[str]) -> None:
        """strings [--identifiers|--strings] [filter:<text>]  - lists strings with kind"""
        proj = self._require_project()
        filter_text, kind_filter, _ = self._parse_listing_args(
            args, {"--identifiers": "identifier", "--strings": "string"})

        strings = proj.hbc.strings()
        kinds   = proj.hbc.string_kinds()
        verdicts = ([e.verdict for e in proj.hbc.string_entries()]
                    if proj.is_permissive else None)
        rows    = display.strings_to_ansi_rows(strings, kinds, filter_text, kind_filter, verdicts)
        title   = f"strings  {len(rows)}/{len(strings)}"
        if kind_filter:
            title += f"  [{kind_filter}s only]"
        if filter_text:
            title += f"  filter={filter_text!r}"
        self._show_rows(rows, title)
