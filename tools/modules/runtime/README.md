# Build and audit

These internal commands are for maintainers. Translators use [Make a language package](../../../docs/translate.md).
Run commands from the repository root. Dependencies and native toolchain requirements are in
[Build and test](../../../docs/development.md).

## Build a matched runtime

```powershell
python -m pip install -r tools/modules/runtime/requirements.in
python -X utf8 tools/modules/runtime_candidate.py build-runtime --version 0.2.0-rc.1 --out build/runtime-0.2.0-rc.1
```

This compiles and tests the ASI and dictionary checker together, inspects the x86 PE imports/exports,
and records source and output fingerprints. Native source changes require a fresh runtime build.
Do not compile concurrently while `build/runtime-candidate.lock` exists. If a build was interrupted,
confirm that its processes have stopped before removing a remaining lock.

## Normalized inputs

The public packer accepts Anvil MFT/DDS exports directly. Internal commands use this normalized resource layout:

```text
local/originals/
  mft/
    265_-_AnimusTechno Regular 22_PC.MagmaMftFile
    266_-_AnimusText Bold_PC.MagmaMftFile
    267_-_Animus Title.MagmaMftFile
    268_-_AnimusText PC.MagmaMftFile
  textures/
    bold.bc2
    techno.bc2
    textpc.bc2
    title.bc2
```

Each `.bc2` is the original single-mip block payload without a DDS header. Resource identities are checked against
`hosts/ac1-pc-v1.json`. Use the public packer when working with Anvil exports; it performs the import itself.
Font recipe fields are in [Font presets](../../../docs/fonts.md).

## Build and verify a language package

```powershell
python -X utf8 tools/modules/build_runtime.py --language data/languages/ja-test/language.json --font local/font.ttf --input-root local/originals --oracle build/runtime-0.2.0-rc.1/dictionary_oracle.exe --out build/package-ja-test
python -X utf8 tools/modules/verify_runtime.py build/package-ja-test/AC1_Language.bin --language data/languages/ja-test/language.json --font local/font.ttf --input-root local/originals --oracle build/runtime-0.2.0-rc.1/dictionary_oracle.exe --report build/verify-ja-test.json
```

The example uses the Japanese sample and its matching font. Direct builds require the dictionary named in the recipe
beside `language.json`; copy a recipe and your TSV to a local directory for custom inputs.
The writer publishes only after independent pixel, metric and original-resource verification.
See [Package layout](../../../docs/RUNTIME_PACKAGE.md) for limits and binary fields.

## Assemble a candidate

```powershell
python -X utf8 tools/modules/runtime_candidate.py verify-runtime build/runtime-0.2.0-rc.1
python -X utf8 tools/modules/runtime_candidate.py build --runtime build/runtime-0.2.0-rc.1 --language data/languages/ja-test/language.json --font local/font.ttf --input-root local/originals --version 0.2.0-rc.1 --out build/candidate-ja-test
python -X utf8 tools/modules/runtime_candidate.py verify build/candidate-ja-test
```

Outputs must be new directories. A candidate contains installation files under `install/`, runtime/package reports
under `evidence/`, `manifest.json` and `SHA256SUMS`. Verification checks the file set and fingerprints, not in-game behavior.

## Inspect, install and archive

- [Read-only installation checks](INSTALL_INSPECT.md)
- [Deployment and rollback](INSTALL_DEPLOY.md)
- [Run snapshots and feedback](RUN_ARCHIVE.md)

## Runtime text audit

```powershell
python -X utf8 tools/modules/audit_runtime_text.py --candidate build/candidate-ja-test --inventory local/strings-first.txt local/strings-second.txt --notranslate data/languages/ja-test/notranslate.policy --out build/audit-ja-test
```

This classifies observed text against the candidate dictionary and an explicit audit policy. It never generates translations.
Inventory text is limited to 48 UTF-16 code units and the first saved state; entries may be truncated and do not prove display.
Keep audit results separate from actual in-game acceptance.
