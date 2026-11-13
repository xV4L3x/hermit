"""recover_modules() / metro_entry_modules() against Metro's own module table.

hermitvault_truth.json is extracted by gen_truth.py from the Metro JS bundle
(hermit-vault/build/main.js) that hermesc compiled into seeder/corpus/hermitvault.hbc:
every module id, its dependencyMap and the __r entry points must come back
exactly from the bytecode.
"""

import json
from pathlib import Path

import _libhbc

from testutil import corpus_path

_TRUTH = json.loads((Path(__file__).parent / "hermitvault_truth.json").read_text())


def test_hermitvault_modules_match_metro_js():
    f = _libhbc.HBCFile.open(str(corpus_path("hermitvault.hbc")), _libhbc.ValidationMode.Strict)
    mods = f.recover_modules()

    assert all(m.dependencies_known for m in mods)
    recovered = {str(m.module_id): list(m.dependencies) for m in mods}
    assert len(recovered) == len(mods)          # unique ids
    assert recovered == _TRUTH["modules"]
    assert f.metro_entry_modules() == _TRUTH["entries"]


def test_factories_are_distinct_functions():
    f = _libhbc.HBCFile.open(str(corpus_path("hermitvault.hbc")), _libhbc.ValidationMode.Strict)
    mods = f.recover_modules()
    gidx = f.header().global_code_index
    factories = [m.function_index for m in mods]
    assert len(set(factories)) == len(factories)
    assert gidx not in factories
