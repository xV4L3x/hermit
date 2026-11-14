"""REPL commands, one module per command.

Each module defines a small mixin class (`OpenCommand`, `InfoCommand`, ...) that
`cli.repl.HermitREPL` inherits. The command methods use the shared state and
helpers the base class provides (`self.console`, `self.project`, `self._require_project`,
`self._resolve_*`, `self._show_rows`, `self._parse_listing_args`); they never
import `cli.repl`, so there is no import cycle.
"""
