"""pytest config for the (Python) integration tests.

Adds the repo root and `build/bindings` (where `_libhbc.so` lives) to sys.path.
If the C++ module isn't built, the whole integration folder is skipped
with a clear message instead of failing with an ImportError.
"""

import sys
from pathlib import Path


_REPO_ROOT = Path(__file__).resolve().parents[2]

for p in (_REPO_ROOT, _REPO_ROOT / "build" / "bindings"):
    sp = str(p)
    if sp not in sys.path:
        sys.path.insert(0, sp)

try:
    import _libhbc  # noqa: F401
except ImportError:
    import warnings

    warnings.warn(
        "_libhbc not importable - build it with "
        "`cmake --build build --target _libhbc` to run the integration tests"
    )
    collect_ignore_glob = ["*"]
