#!/usr/bin/env bash
# build-real-bundle.sh - build a real React Native app's Hermes bundle (HBC v96)
# from an OPEN-SOURCE repository, without downloading any store artifact (IPA/APK).
#
# Why this exists
# ---------------
# hermit's structural patcher is validated against real, varied, production-grade
# bundles. To keep that validation (a) legally clean and (b) reproducible by a
# third party, we build the bundle from a permissively licensed source repo and
# compile it to HBC with our own hermesc. The HBC version is pinned to 96 by the
# compiler we choose, independently of the app's React Native version.
#
# Legal basis (reverse engineering for study/security research)
# -------------------------------------------------------------
# - EU: Directive 2009/24/EC art. 5(3) - a lawful user may observe, study and test
#   the functioning of a program; art. 6 - decompilation for interoperability.
# - Italy: L. 633/1941 (diritto d'autore) artt. 64-ter and 64-quater, which
#   transpose the above.
# Use only with OPEN-SOURCE apps under a permissive licence (MIT/Apache); the
# source is lawfully obtained and no App Store / Play Store ToS is involved.
# Constraints we keep:
#   * no modified bundle is redistributed;
#   * no modified bundle is executed against the app's live services;
#   * the bundle is used only to validate hermit's parser/serializer/patcher
#     in the lab (round-trip + structural patch + re-parse).
# This script never downloads a store binary and never bypasses any DRM.
#
# Usage
# -----
#   ./seeder/build-real-bundle.sh <repo-url> <out.hbc> [options]
#
# Options
#   --ref <git-ref>        commit / tag / branch to pin (default: repo default branch)
#   --platform <p>         android | ios   (default: android)
#   --entry <file>         JS entry file   (default: index.js)
#   --minify <true|false>  Metro minify    (default: false - mirrors RN+Hermes release)
#   --debug-level <n>      hermesc -g level (default: 0 - cleanest for patching;
#                          RN production ships -g1 on iOS / -g2 on Android)
#   --keep-workdir         do not delete the clone afterwards
#
# Environment
#   HERMES   path to the hermes compiler that emits HBC 96
#            (default: hermes on PATH, e.g. /usr/local/bin/hermes)
#
# Example
#   ./seeder/build-real-bundle.sh https://github.com/bluesky-social/social-app \
#       seeder/real-apps/bluesky.hbc --ref main --platform android
#
set -euo pipefail

# ----------------------------------------------------------------------------- args
if [[ $# -lt 2 ]]; then
  grep '^#' "$0" | sed 's/^# \{0,1\}//'   # print this header as help
  exit 2
fi

REPO_URL="$1"; shift
OUT_HBC="$1"; shift

REF=""
PLATFORM="android"
ENTRY="index.js"
MINIFY="false"
DEBUG_LEVEL="0"
KEEP_WORKDIR=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ref)         REF="$2"; shift 2 ;;
    --platform)    PLATFORM="$2"; shift 2 ;;
    --entry)       ENTRY="$2"; shift 2 ;;
    --minify)      MINIFY="$2"; shift 2 ;;
    --debug-level) DEBUG_LEVEL="$2"; shift 2 ;;
    --keep-workdir) KEEP_WORKDIR=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

HERMES="${HERMES:-hermes}"
EXPECTED_VERSION=96

# ----------------------------------------------------------------------------- checks
command -v git  >/dev/null || { echo "git not found" >&2; exit 1; }
command -v node >/dev/null || { echo "node not found" >&2; exit 1; }
command -v "$HERMES" >/dev/null || { echo "hermes compiler not found: $HERMES" >&2; exit 1; }

OUT_HBC_ABS="$(cd "$(dirname "$OUT_HBC")" && pwd)/$(basename "$OUT_HBC")"
mkdir -p "$(dirname "$OUT_HBC_ABS")"

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/hermit-realbundle.XXXXXX")"
JS_BUNDLE="$WORKDIR/app.jsbundle"
cleanup() { [[ "$KEEP_WORKDIR" -eq 1 ]] || rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "==> repo     : $REPO_URL"
echo "==> ref      : ${REF:-<default branch>}"
echo "==> platform : $PLATFORM   entry: $ENTRY   minify: $MINIFY   -g$DEBUG_LEVEL"
echo "==> hermes   : $HERMES"
echo "==> out      : $OUT_HBC_ABS"
echo "==> workdir  : $WORKDIR"

# ----------------------------------------------------------------------------- 1. clone
echo "==> [1/5] cloning (shallow)"
SRC="$WORKDIR/src"
if [[ -n "$REF" ]]; then
  git clone --filter=blob:none --no-checkout "$REPO_URL" "$SRC"
  git -C "$SRC" fetch --depth 1 origin "$REF"
  git -C "$SRC" checkout --detach FETCH_HEAD
else
  git clone --depth 1 "$REPO_URL" "$SRC"
fi
COMMIT_SHA="$(git -C "$SRC" rev-parse HEAD)"
echo "    commit $COMMIT_SHA"

# ----------------------------------------------------------------------------- 2. deps
echo "==> [2/5] installing dependencies"
pushd "$SRC" >/dev/null

# Honour the repo's declared package manager via corepack (ships with Node), so
# a repo that requires pnpm/yarn gets it at the pinned version. Using the right
# PM also sidesteps npm's hard EBADDEVENGINES error on the "devEngines" field
# (npm errors on it; pnpm/yarn ignore it). engine-strict is turned off defensively
# so a stricter-than-needed "engines" bound only warns instead of blocking.
export npm_config_engine_strict=false
PM_FIELD="$(node -e 'try{console.log(require("./package.json").packageManager||"")}catch(e){console.log("")}')"
PM=""
case "$PM_FIELD" in
  pnpm*) PM="pnpm" ;;
  yarn*) PM="yarn" ;;
  npm*)  PM="npm"  ;;
  *)  # no declared PM: fall back to the lockfile
      if   [[ -f pnpm-lock.yaml ]]; then PM="pnpm"
      elif [[ -f yarn.lock      ]]; then PM="yarn"
      else                               PM="npm"
      fi ;;
esac

if [[ -n "$PM_FIELD" ]] && command -v corepack >/dev/null; then
  corepack enable >/dev/null 2>&1 || true
  corepack prepare "$PM_FIELD" --activate >/dev/null 2>&1 || true
fi
echo "    package manager: ${PM_FIELD:-$PM (from lockfile)}"

case "$PM" in
  pnpm) pnpm install --config.engine-strict=false --frozen-lockfile || pnpm install --config.engine-strict=false ;;
  yarn) yarn install --frozen-lockfile || yarn install ;;
  npm)  npm ci --engine-strict=false || npm install --engine-strict=false ;;
esac

RN_VERSION="$(node -e 'try{console.log(require("./node_modules/react-native/package.json").version)}catch(e){console.log("unknown")}')"
echo "    react-native: $RN_VERSION"

# ----------------------------------------------------------------------------- 3. JS bundle
# Produce a plain (pre-Hermes) JS bundle with Metro, so WE control the HBC version
# in step 4. Try bare-RN first, then the Expo equivalent (export:embed).
echo "==> [3/5] producing Metro JS bundle"
# Call the LOCAL binaries directly (node_modules/.bin), never npx: npx is npm and
# re-triggers npm's hard EBADDEVENGINES gate even on a pnpm/yarn project. The bin
# shebangs run plain node, which has no devEngines gate.
RNBIN="node_modules/.bin/react-native"
EXPOBIN="node_modules/.bin/expo"
set +e
if [[ -x "$RNBIN" ]]; then
  "$RNBIN" bundle --platform "$PLATFORM" --dev false --minify "$MINIFY" \
    --entry-file "$ENTRY" --bundle-output "$JS_BUNDLE" --reset-cache
fi
if [[ ! -s "$JS_BUNDLE" && -x "$EXPOBIN" ]]; then
  echo "    react-native bundle unavailable/failed - trying 'expo export:embed'"
  "$EXPOBIN" export:embed --platform "$PLATFORM" --dev false \
    --bundle-output "$JS_BUNDLE" --entry-file "$ENTRY"
fi
set -e
popd >/dev/null
[[ -s "$JS_BUNDLE" ]] || { echo "ERROR: no JS bundle produced" >&2; exit 1; }
echo "    JS bundle: $(wc -c < "$JS_BUNDLE") bytes"

# ----------------------------------------------------------------------------- 4. compile HBC
echo "==> [4/5] compiling to HBC (-O -g$DEBUG_LEVEL)"
"$HERMES" -emit-binary -O "-g$DEBUG_LEVEL" -out "$OUT_HBC_ABS" "$JS_BUNDLE"

# ----------------------------------------------------------------------------- 5. verify
echo "==> [5/5] verifying HBC header"
# magic: uint64 @ 0x00 == 0x1F1903C103BC1FC6 ; version: uint32 LE @ 0x08
MAGIC_HEX="$(od -An -tx1 -N8 "$OUT_HBC_ABS" | tr -d ' \n')"   # little-endian byte order
VERSION="$(od -An -tu4 -j8 -N4 "$OUT_HBC_ABS" | tr -d ' ')"
if [[ "$MAGIC_HEX" != "c61fbc03c103191f" ]]; then
  echo "ERROR: bad HBC magic ($MAGIC_HEX) - output is not a valid HBC file" >&2
  exit 1
fi
if [[ "$VERSION" != "$EXPECTED_VERSION" ]]; then
  echo "ERROR: HBC version is $VERSION, expected $EXPECTED_VERSION." >&2
  echo "       Your '$HERMES' emits a different version. Point HERMES at a" >&2
  echo "       compiler that emits HBC 96, or re-check the toolchain." >&2
  exit 1
fi
echo "    OK: HBC version $VERSION, $(wc -c < "$OUT_HBC_ABS") bytes"

# ----------------------------------------------------------------------------- provenance
# Audit trail: exactly how this bundle was produced, so it is reproducible and the
# legal/academic provenance is documented next to the artifact.
PROV="$OUT_HBC_ABS.provenance.txt"
{
  echo "# Provenance of $(basename "$OUT_HBC_ABS")"
  echo "generated-at : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "repo-url     : $REPO_URL"
  echo "commit-sha   : $COMMIT_SHA"
  echo "react-native : $RN_VERSION"
  echo "hermes       : $HERMES"
  echo "platform     : $PLATFORM"
  echo "entry        : $ENTRY"
  echo "minify       : $MINIFY"
  echo "debug-level  : -g$DEBUG_LEVEL"
  echo "hbc-version  : $VERSION"
  echo "size-bytes   : $(wc -c < "$OUT_HBC_ABS" | tr -d ' ')"
  echo "legal-basis  : Dir. 2009/24/EC art.5(3)/6; L.633/1941 artt.64-ter/64-quater"
  echo "note         : open-source build; not redistributed; not run against live services"
} > "$PROV"
echo "==> provenance written: $PROV"
echo "==> done."
