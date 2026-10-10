"""hermit CLI - python -m cli.main [file.hbc]"""

import sys

from cli.repl import HermitREPL


def main() -> None:
    initial_file = sys.argv[1] if len(sys.argv) > 1 else None
    HermitREPL().run(initial_file)


if __name__ == "__main__":
    main()
