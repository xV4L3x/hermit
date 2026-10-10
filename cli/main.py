"""hermit CLI: python -m cli.main [--version] [file.hbc]"""

import argparse

from cli._version import __version__


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="hermit",
        description="Hermes Reverse Engineering & Manipulation Interactive Toolkit.",
    )
    parser.add_argument("file", nargs="?", help="HBC file to open (.hbc)")
    parser.add_argument("--version", action="version", version=f"hermit {__version__}")
    args = parser.parse_args()

    from cli.repl import HermitREPL  # imported late: --version/--help don't need the C++ module
    HermitREPL().run(args.file)


if __name__ == "__main__":
    main()
