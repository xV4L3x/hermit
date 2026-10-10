# HBC Bytecode Version Map & Compiler Pinning

> Reference for choosing and reproducing a Hermes compiler that emits a **specific** HBC bytecode version. There is no official spec and no clean "React Native version -> HBC version" table in public sources, so this map is derived directly from the Hermes source history **and verified empirically** by compiling with real `hermesc` binaries.

**Last verified:** 2026-06-23.

---

## TL;DR - the version that matters is 96

**Real React Native apps ship HBC bytecode version 96**, and have for years. This was verified three independent ways (see [Evidence](#evidence)):

- `facebook/hermes` branch `main` -> `BYTECODE_VERSION = 96` (today).
- Every `rn/0.74-stable` … `rn/0.84-stable` branch -> 96. The bundled `hermesc` in npm `react-native` 0.73–0.84 all emit 96.
- The two facebook/hermes prebuilt CLI releases (v0.12.0, v0.13.0) -> both 96.

> **The release string does not identify the source commit (2026-10-03).**
> The installed `hermes` prints "release version 0.12.0, HBC bytecode version
> 96", but the git **tag** `v0.12.0` is commit `7d3e091b5` of 2022-08-16 and
> has `BYTECODE_VERSION = 89`. To read the source that matches a v96 binary use
> a React Native release tag, e.g.
> `hermes-2023-09-26-RNv0.73.0-ee2922a50fb719bdb378025d95dbd32ad93cd679`
> (`BYTECODE_VERSION = 96`), never `v0.12.0` and never the default branch.
> Reading the wrong commit is what produced the false "this part of the format
> is in no public source" claims about debug info, corrected on this date.

**97, 98, 99 are a trap.** They exist only on the **`static_h` branch** - Static Hermes, Meta's experimental ahead-of-time *JS->native* compiler - which happens to be the repo's **default branch** (this is exactly why earlier notes misread it). Static Hermes is **not** what React Native uses to produce the bytecode bundle in shipping apps. Targeting 97/98/99 means targeting a compiler almost no real app uses.

> **Correction history:** an earlier version of this file claimed `main` was 99 with 97/98 as recent mainline bumps and "98 = most current real apps." That was wrong - it read `BytecodeVersion.h` on the default branch (`static_h`) and assumed it was the HBC mainline. The cited bump commits are real but live on `static_h`. The whole project target was retargeted from 97/98/99 -> **96** (primary) + 94/95 (multi-version validation) as a result.

---

## Where the version lives

The bytecode version is a single hardcoded constant in the Hermes source:

```cpp
// include/hermes/BCGen/HBC/BytecodeVersion.h
const static uint32_t BYTECODE_VERSION = <N>;
```

It is **per-commit**, not per-release, and **per-branch**. This is the value written as a 32-bit little-endian integer at **offset `0x08`** of every `.hbc` file - immediately after the 8-byte magic `c6 1f bc 03 c1 03 19 1f` (the 64-bit constant `0x1F1903C103BC1FC6`).

```bash
# read the version field of any .hbc / RN bundle
xxd -s 8 -l 4 index.android.bundle      # 60 00 00 00 -> 0x60 = 96
```

## Two lines: `main` (HBC) vs `static_h` (Static Hermes)

| Branch | Today's `BYTECODE_VERSION` | What it is |
|--------|---------------------------|------------|
| `main` | **96** | The HBC mainline - what `hermesc` in React Native emits. |
| `rn/0.74-stable` … `rn/0.84-stable` | **96** | Per-RN-release branches; all 96. |
| `release-v0.16`, `release-v0.17` | **96** | Latest tagged Hermes releases. |
| **`static_h`** (repo default branch) | **99** | Static Hermes, experimental AOT JS->native. **Not** used by shipping RN apps. |

## HBC version -> introducing commit (the `main` / HBC line)

Confirmed by reading `BytecodeVersion.h` at each commit on `main` (use `?ref=<sha>` or `git checkout`).

| HBC | Introduced by commit (on `main`) | SHA | Date | Notes |
|-----|----------------------------------|-----|------|-------|
| 94  | "Add bytecode support for block scoping" | `1c717488d179` | 2023-03-08 | |
| 95  | "Add isStrict argument to DirectEval"    | `f6b56d334467` | 2023-03-29 | short-lived; **no RN release shipped 95** |
| 96  | "Add support for \"hasIndices\" RegExp flag" | `2afc7b09ffc7` | 2023-08-29 | current `main`; current real-world version |

Older: 90 (`0763eee4`, 2022-10-07), 91, 92, 93 … down to 59 (initial release). Walk `git log --sha=main` on the file to extend.

The 97/98/99 bumps (`16b5ada8`->97, `c00cc575`->98, `42235b8d`->99) are **on `static_h`**, not `main` - keep them only for the future-work chapter.

## React Native version -> HBC version (empirical, bundled `hermesc`)

Measured by extracting `package/sdks/hermesc/osx-bin/hermesc` from each npm `react-native` tarball and compiling a sample. RN pins a specific (often lagging) Hermes commit, so this is the **ground truth for what apps ship**.

| React Native | HBC emitted |
|--------------|-------------|
| 0.70.x | 89 |
| 0.71.x | 90 |
| 0.72.x (all patches) | **94** |
| 0.73.x – 0.83.x | **96** |
| 0.84.x – 0.86.x | **98** by default (Hermes V1); **96** with Hermes V1 disabled |
| 0.87+ | **98** only (legacy Hermes removed) |

Note RN jumps **94 -> 96** between 0.72 and 0.73, skipping 95 entirely.

### Update 2026-09-29: Hermes V1 = HBC 98 (RN 0.84+)

The row "0.73–0.84 -> 96" above was wrong for 0.84. From RN 0.84 `hermesc`
is no longer in `sdks/hermesc/` but in the npm package `hermes-compiler`
(and in the `hermes-engine` pod, `destroot/bin/hermesc`, which
`scripts/react-native-xcode.sh` uses). Verified by compiling a sample with
each binary and reading offset `0x08`:

| RN | `sdks/hermes-engine/version.properties` | HBC |
|---|---|---|
| 0.83 (2025-12-10) | `hermes-compiler` 0.14.1 | 96 |
| 0.84 (2026-02-11) | `HERMES_VERSION_NAME=0.15.1`, `HERMES_V1_VERSION_NAME=250829098.0.9` | V1: **98**, legacy 0.15.1: 96 |
| 0.85 | 0.16.0 / 250829098.0.10 | V1: 98, legacy: 96 |
| 0.86 | 0.17.0 / 250829098.0.17 | V1: 98, legacy 0.17.0: 96 |
| 0.87 (2026-08-11, latest) | `HERMES_VERSION_NAME=250829098.0.17` only | **98** only |

**Hermes V1 is the default** from 0.84: `hermes-engine.podspec` picks
`HERMES_V1_VERSION_NAME` unless `RCT_HERMES_V1_ENABLED=0` (iOS);
`hermesV1Enabled=false` in `gradle.properties` on Android. So apps built with
RN ≤ 0.83, or 0.84–0.86 with V1 opted out, ship **96**; new apps built with the
defaults ship **98**, which hermit does not support yet. The
demo app `hermit-vault/` uses RN 0.86.3 with V1 disabled to stay on 96.

## How to get a `hermesc` for a target version

### Easiest: prebuilt from a React Native npm package (no compiling)
```bash
# 96 -> RN >=0.73 ; 94 -> RN 0.72.x
V=0.72.0   # pick per the RN->HBC table above
URL=$(npm view react-native@$V dist.tarball)
curl -sL "$URL" | tar -xz package/sdks/hermesc/osx-bin/
BIN=package/sdks/hermesc/osx-bin/hermesc
xattr -dr com.apple.quarantine package 2>/dev/null
"$BIN" -emit-binary -out sample.hbc sample.js
xxd -s 8 -l 4 sample.hbc        # confirm the version
```
You already have a 96 compiler: the system `hermes` 0.12.0 (`hermes --version` -> HBC 96).

### When no prebuilt exists (e.g. 95): build from the pinned commit
```bash
git clone https://github.com/facebook/hermes.git && cd hermes
git checkout f6b56d334467            # 95 ; use the SHA from the table
cmake -S . -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_release --target hermesc      # -> build_release/bin/hermesc
grep BYTECODE_VERSION include/hermes/BCGen/HBC/BytecodeVersion.h   # confirm
```
This pulls LLVM and takes a while - only needed for versions with no RN prebuilt (95) or for the Static Hermes future-work line (`git checkout static_h`).

### Compilers + VMs used by the multi-version tests (94, 95)

`tests/integration/patcher/test_multiversion.py` compiles samples with the
`hermes` of each version and runs the patched output on the VM of the **same**
version. The `hermes` target contains both compiler and VM (the RN prebuilt
`hermesc` is compiler-only, so it can't execute). Built from a clone of
`facebook/hermes` (2026-09-29, macOS arm64, ~10 min each):

```bash
git clone https://github.com/facebook/hermes.git /tmp/hermes-src && cd /tmp/hermes-src
git worktree add --detach /tmp/hermes95 f6b56d334467     # 95
git worktree add --detach /tmp/hermes94 f6b56d334467^    # 94 (last commit before 95)
for v in 94 95; do
  # CMake 4 no longer accepts cmake_policy(SET CMP0026 OLD): drop that line
  sed -i '' 's/cmake_policy(SET CMP0026 OLD)//' /tmp/hermes$v/CMakeLists.txt
  cmake -S /tmp/hermes$v -B /tmp/build$v -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DHERMES_ENABLE_TEST_SUITE=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  cmake --build /tmp/build$v --target hermes
  install -D /tmp/build$v/bin/hermes ~/.local/share/hermit/hermes/hermes$v
done
```

The tests look in `$HERMES94` / `$HERMES95`, then in
`~/.local/share/hermit/hermes/hermes9{4,5}`; a missing compiler skips its version.
Note: `hermes` v94–96 compiles ES6 `class` only with `-Xes6-class`.

The **seeder** pins these compilers so the corpus is byte-stable. The **parser** must handle whatever the real target apps use - confirm with `xxd -s 8 -l 4` on each app first.

## Evidence

- `BytecodeVersion.h` content via GitHub contents API at `?ref=main` (96) vs `?ref=static_h` (99) vs `?ref=rn/0.84-stable` (96).
- Bundled `hermesc` from npm `react-native` 0.70/0.71/0.72/0.73/0.74/0.75/0.76/0.79/0.82 compiled a sample -> 89/90/94/96/96/96/96/96/96.
- RN `.hermesversion` pins: RN 0.82.0 -> `hermes-2025-09-01-RNv0.82.0-265ef62…` (emits 96).
- facebook/hermes prebuilt releases v0.12.0 and v0.13.0 -> both emit 96.
- `compare 913d31a…main` -> diverged (the 97/98/99 line is not on `main`).

## How to refresh this map

```bash
# every commit that touched the version file on the HBC mainline, newest first
git -C <hermes-clone> log --date=short --format='%ad %h %s' main \
    -- include/hermes/BCGen/HBC/BytecodeVersion.h
```
Or via the GitHub API - **pin the branch explicitly**, the default is `static_h`:
`https://api.github.com/repos/facebook/hermes/commits?path=include/hermes/BCGen/HBC/BytecodeVersion.h&sha=main`

## Sources

- Hermes `BytecodeVersion.h` (pick branch): <https://github.com/facebook/hermes/blob/main/include/hermes/BCGen/HBC/BytecodeVersion.h>
- hbctool: <https://github.com/bongtrop/hbctool> · hermes-dec: <https://github.com/P1sec/hermes-dec>
- Using Hermes (React Native): <https://reactnative.dev/docs/hermes>
