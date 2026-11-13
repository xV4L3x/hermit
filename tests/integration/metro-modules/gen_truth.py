"""Extracts the Metro module table from an unminified Metro JS bundle into the
JSON ground truth used by test_metro_truth.py.

Metro (`react-native bundle --minify false`) closes every module definition with
a line `},<moduleId>,[<dependencyMap>]);` and starts the app with `__r(<id>);`.

    .venv/bin/python tests/integration/metro-modules/gen_truth.py \
        ../hermit-vault/build/main.js tests/integration/metro-modules/hermitvault_truth.json
"""

import json
import re
import sys
from pathlib import Path

_DEFINE = re.compile(r"^\},(\d+),(\[[0-9,null]*\])\);$", re.M)
_ENTRY  = re.compile(r"^__r\((\d+)\);$", re.M)


def extract(js: str) -> dict:
    modules = {int(mid): json.loads(deps) for mid, deps in _DEFINE.findall(js)}
    if js.count("\n__d(") + js.startswith("__d(") != len(modules):
        raise ValueError("some __d(...) definitions don't end with a `},id,[deps]);` line")
    return {"modules": {str(k): v for k, v in sorted(modules.items())},
            "entries": [int(e) for e in _ENTRY.findall(js)]}


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit("usage: gen_truth.py <metro-bundle.js> <out.json>")
    truth = extract(Path(sys.argv[1]).read_text())
    Path(sys.argv[2]).write_text(json.dumps(truth, separators=(",", ":")) + "\n")
    print(f"{len(truth['modules'])} modules, entries {truth['entries']}")


if __name__ == "__main__":
    main()
