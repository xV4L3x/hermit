#!/usr/bin/env bash
# Builds the release archives of hermit for the current machine.
#
#   packaging/build-release.sh <version> <platform>
#
# <platform> only names the archives (macos-arm64, linux-x86_64, ...). Output,
# in dist/:
#   hermit-<version>-<platform>.tar.gz          the REPL (frozen with PyInstaller)
#   hermit-<version>-<platform>.tar.gz.sha256   its checksum
#
# Needs CMake, a C++20 compiler, and a Python with requirements.txt and
# PyInstaller installed. $PYTHON selects the interpreter (default: python3).
set -euo pipefail

VERSION="${1:?usage: $0 <version> <platform>}"
PLATFORM="${2:?usage: $0 <version> <platform>}"
PYTHON="$(command -v "${PYTHON:-python3}")"

cd "$(dirname "$0")/.."
ROOT="$PWD"
DIST="$ROOT/dist"
APP="hermit-$VERSION-$PLATFORM"

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE="$PYTHON")
if [[ "$(uname -s)" == "Darwin" ]]; then
    CMAKE_ARGS+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0)
fi

sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

rm -rf "$DIST/$APP" "$DIST/$APP.tar.gz" "$DIST/$APP.tar.gz.sha256"
mkdir -p "$DIST"

echo "==> Python module"
cmake -S . -B "$DIST/_cmake-app" "${CMAKE_ARGS[@]}" -DHERMIT_BUILD_TESTS=OFF
cmake --build "$DIST/_cmake-app" --target _libhbc --parallel

echo "==> REPL executable"
"$PYTHON" -m PyInstaller --noconfirm --clean --onedir --name hermit \
    --distpath "$DIST/_pyi" --workpath "$DIST/_pyi-work" --specpath "$DIST/_pyi-work" \
    --paths "$ROOT" --paths "$DIST/_cmake-app/bindings" --hidden-import _libhbc \
    --collect-submodules pygments --collect-submodules rich --collect-submodules prompt_toolkit \
    packaging/entry.py

mkdir "$DIST/$APP"
cp -R "$DIST/_pyi/hermit/hermit" "$DIST/_pyi/hermit/_internal" README.md LICENSE "$DIST/$APP/"

echo "==> Archive"
cd "$DIST"
tar -czf "$APP.tar.gz" "$APP"
sha256 "$APP.tar.gz" > "$APP.tar.gz.sha256"
cat "$APP.tar.gz.sha256"
