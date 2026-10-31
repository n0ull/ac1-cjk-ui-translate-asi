# Build and test

## Native build

The configured environment is Windows x64, PowerShell 7, MSVC **14.44.35207** (VS 2022 Build Tools) and
Windows SDK **10.0.26100.0** in their standard installation directories. Output targets x86 with a static CRT.
Paths are set in `tools/modules/native-toolchain.ps1`; PE inspection also uses the configured MSVC path.
Other toolchain installations require corresponding code changes and verification.

From the repository root:

```powershell
git submodule update --init --recursive
pwsh -NoProfile -File build.ps1 -RunTests
```

Output: `app/out/ac1_runtime_package.asi`. The build tests four modules and loader startup.
It does not start or install the game.

## Dictionary checker

Language packaging uses the production C++ dictionary implementation to validate input.
For a source checkout, build the checker once with the native toolchain:

```powershell
pwsh -NoProfile -File tools/modules/build_runtime_oracle.ps1
```

This creates `build/runtime-oracle/dictionary_oracle.exe` and `dictionary_oracle.json`.
The packer verifies both the executable and its recorded source files. Rebuild after changing those sources.
A translator toolkit supplies this pair, so translators do not need a compiler.

## Python tests

Requires Python 3.11+:

```powershell
python -m pip install -r tools/modules/runtime/requirements.in
python -X utf8 -m unittest discover -s tools/modules/tests -v
```

Tests use temporary synthetic inputs. Cases requiring private dictionaries, native probes or explicitly enabled Windows
process checks report a skip when prerequisites are absent. No test deploys to your real game directory.
Use [the translator workflow](translate.md) for an integration build with your own exports and font.

## Tool layout

| Module | Responsibility |
| --- | --- |
| `translation_sheet.py` | LG decoding, Excel export and translation import |
| `language_pack.py` | Anvil imports, preset selection and bin publication |
| `build_runtime.py` / `verify_runtime.py` | Package writer and independent pixel/metric reader |
| `_dictionary.py`, `_runtime_inputs.py`, `_runtime_raster.py` | Dictionary rules, input validation and rasterization |
| `runtime_candidate.py` | Build and verify a matched ASI/checker, then assemble language candidates |
| `runtime_install.py` | Installation checks, plans, deployment, verification and rollback |
| `runtime_archive.py` / `audit_runtime_text.py` | Run-log snapshots and untranslated-text reports |

These files live under `tools/modules/`; `tools/` exposes only the two translator entries.
Internal build commands and normalized input layouts are in [Build and audit](../tools/modules/runtime/README.md).
Recipe fields and calibration are documented in [Font presets](fonts.md).

## Assemble a translator toolkit

Include the source tree, supplied presets, resource baselines and licenses, plus:

```text
scripts/ac1_runtime_package.asi
build/runtime-oracle/dictionary_oracle.exe
build/runtime-oracle/dictionary_oracle.json
```

Build the ASI and checker from the same source revision. Keep checker provenance files byte-for-byte intact,
including the referenced C++ sources, headers and build scripts. Fingerprints check consistency, not authorship.

Exclude `local/`, game exports, fonts, full dictionaries and generated bins. Test an independently unpacked toolkit by
exporting an Excel workbook and building a sample bin, without a compiler on PATH.
Check menus, controls and scene loading in both Dx9 and Dx10 before releasing runtime or font changes.
