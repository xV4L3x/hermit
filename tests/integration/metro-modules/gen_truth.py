"""Extracts the Metro module table from an unminified Metro JS bundle into the
JSON ground truth used by test_metro_truth.py.

Metro (`react-native bundle --minify false`) closes every module definition with
a line `},<moduleId>,[<dependencyMap>]);` and starts the app with `__r(<id>);`.
`requires` lists, per module, the dependencyMap indices its code passes to
`_$$_REQUIRE(_dependencyMap[k])`. `exports` lists, per module, the names it
exports, with the rule of HBCFile::module_exports(): when the module assigns
`module.exports = ...`, the properties set on `module.exports` plus the keys of
an object literal assigned to it, otherwise the names set on `exports`
(`exports.X =`, `exports["X"] =`, `Object.defineProperty(exports, "X", ...)`);
`__esModule` excluded. `esModule` lists the modules that define `__esModule`,
`dynamicExports` those with a non-constant defineProperty name (`export *`).

    .venv/bin/python tests/integration/metro-modules/gen_truth.py \
        ../hermit-vault/build/main.js tests/integration/metro-modules/hermitvault_truth.json
"""

import json
import re
import sys
from pathlib import Path

_DEFINE = re.compile(r"^\},(\d+),(\[[0-9,null]*\])\);$", re.M)
_ENTRY  = re.compile(r"^__r\((\d+)\);$", re.M)
_REQUIRE = re.compile(r"_\$\$_REQUIRE\(_dependencyMap\[(\d+)\]\)")
_ID = r"[A-Za-z_$][\w$]*"
_EXPORTS_SET = re.compile(r"(?<![\w$.])exports(?:\.(" + _ID + r")|\[[\"']([^\"']+)[\"']\])\s*=(?!=)")
_MODULE_EXPORTS_SET = re.compile(r"module\.exports(?:\.(" + _ID + r")|\[[\"']([^\"']+)[\"']\])\s*=(?!=)")
_DEFINE_PROP = re.compile(r"Object\.defineProperty\(\s*(module\.exports|exports)\s*,\s*([\"']([^\"']+)[\"']|[^,)]+)")
_MODULE_EXPORTS_ASSIGN = re.compile(r"module\.exports\s*=(?!=)\s*")


def _literal_keys(src: str, start: int) -> list:
    """Top-level keys of the object literal whose `{` is at src[start]:
    `name: v`, `"name": v`, `name,` (shorthand), `get name()`, `name() {}`.
    Skips strings, template literals and comments."""
    keys, depth, i, n = [], 0, start, len(src)
    expect_key = True
    while i < n:
        c = src[i]
        if c in "\"'`":
            q = c
            j = i + 1
            while j < n and src[j] != q:
                j += 2 if src[j] == "\\" else 1
            if depth == 1 and expect_key and c != "`":
                rest = src[j + 1:].lstrip()
                if rest.startswith(":"):
                    keys.append(src[i + 1:j])
                    expect_key = False
            i = j + 1
            continue
        if src.startswith("//", i):
            i = src.find("\n", i)
            i = n if i < 0 else i
            continue
        if src.startswith("/*", i):
            i = src.find("*/", i)
            i = n if i < 0 else i + 2
            continue
        if c in "{[(":
            depth += 1
            i += 1
            continue
        if c in "}])":
            depth -= 1
            i += 1
            if depth == 0:
                return keys
            continue
        if depth == 1 and c == ",":
            expect_key = True
            i += 1
            continue
        if depth == 1 and expect_key:
            m = re.compile(r"(?:(?:get|set|async)\s+)?(" + _ID + r")\s*(:|\(|,|\})").match(src, i)
            if m and not src[i:].startswith(("...",)):
                keys.append(m.group(1))
                expect_key = False
                i = m.start(2)
                continue
            if not c.isspace():
                expect_key = False
        i += 1
    return keys


def _exports_of(body: str):
    """(names, esModule, dynamic) of one module's source, see the module doc."""
    reassigned = list(_MODULE_EXPORTS_ASSIGN.finditer(body))
    names, es, dynamic = set(), False, False
    target = "module.exports" if reassigned else "exports"
    setter = _MODULE_EXPORTS_SET if reassigned else _EXPORTS_SET
    for m in setter.finditer(body):
        names.add(m.group(1) or m.group(2))
    for m in _DEFINE_PROP.finditer(body):
        if m.group(1) != target:
            continue
        if m.group(3) is not None:
            names.add(m.group(3))
        else:
            dynamic = True
    for m in reassigned:
        if body.startswith("{", m.end()):
            names.update(_literal_keys(body, m.end()))
    if "__esModule" in names:
        es = True
        names.discard("__esModule")
    return sorted(names), es, dynamic


def extract(js: str) -> dict:
    modules = {int(mid): json.loads(deps) for mid, deps in _DEFINE.findall(js)}
    if js.count("\n__d(") + js.startswith("__d(") != len(modules):
        raise ValueError("some __d(...) definitions don't end with a `},id,[deps]);` line")
    requires, exports, es_modules, dynamic = {}, {}, [], []
    for chunk in re.split(r"(?m)^(?=__d\()", js):
        m = _DEFINE.search(chunk)
        if chunk.startswith("__d(") and m:
            mid, body = int(m.group(1)), chunk[:m.start()]
            requires[mid] = sorted({int(k) for k in _REQUIRE.findall(body)})
            names, es, dyn = _exports_of(body)
            exports[mid] = names
            if es:
                es_modules.append(mid)
            if dyn:
                dynamic.append(mid)
    return {"modules": {str(k): v for k, v in sorted(modules.items())},
            "entries": [int(e) for e in _ENTRY.findall(js)],
            "requires": {str(k): v for k, v in sorted(requires.items())},
            "exports": {str(k): v for k, v in sorted(exports.items())},
            "esModule": sorted(es_modules),
            "dynamicExports": sorted(dynamic)}


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit("usage: gen_truth.py <metro-bundle.js> <out.json>")
    truth = extract(Path(sys.argv[1]).read_text())
    Path(sys.argv[2]).write_text(json.dumps(truth, separators=(",", ":")) + "\n")
    print(f"{len(truth['modules'])} modules, entries {truth['entries']}")


if __name__ == "__main__":
    main()
