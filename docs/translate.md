# Make a language package

You need Python 3.11+, an LG dictionary, your game's AnvilToolkit exports, and a font matching a supplied preset.
Use the translator toolkit for a ready-to-run dictionary checker. If you cloned the source, first follow
[Build the dictionary checker](development.md#dictionary-checker).

## 1. Prepare files

With AnvilToolkit, unpack `DataPC.forge`, then unpack `2_-_Game Bootstrap Settings.data`.
Keep the exported filenames:

- Copy resources **265–268** (`.MagmaMftFile`) to `input/mft-dir/`.
- Export resources **735–738** with **Save As DDS** to `input/dds-dir/`.

Place your LG dictionary([lg patch](https://community.pcgamingwiki.com/files/file/3594-assassins-creed-chinese-localization-patches)) and font in `local/`:

```text
project/
  tools/
    lg_to_excel.py
    pack_language.py
    modules/
  input/
    mft-dir/              Four MFT exports
    dds-dir/              Four DDS exports
  local/
    LGCStringDict_01.txt
    font.ttf              Font matching the selected preset
  build/                  Generated output
```

Create these local directories as needed. Their contents are ignored by Git.
The default preset is `zh`; `ja-test` is also supplied. Check the required font file in [Font presets](fonts.md).
The scripts read DDS directly; no image conversion or archive repacking is needed.

## 2. Export and translate

Open a terminal in the project directory:

```powershell
cd tools
python -m pip install -r modules/runtime/requirements.in
python -X utf8 lg_to_excel.py --input ../local/LGCStringDict_01.txt --out ../local/translation.xlsx
```

Open `local/translation.xlsx`. It has one sheet named **翻译** (Translation):

| Column | What to do |
| --- | --- |
| 原文 — Original | Keep the matching key unchanged. |
| 译文 — Translation | Enter your translation here. Leave pending entries blank. |
| LG参考译文 — LG reference | Optional reference, never included automatically. |
| 备注 — Notes | Add your own notes. |

Repeated original keys are merged. Different reference translations are retained together.
Entries with unsupported control codes appear at the end with a note; leave those translations blank.

Use plain text, not formulas. Enter numeric labels as text. Line breaks may be entered directly or as `\n`;
`\t` is a tab and `\\` is a literal backslash. Keep the headers and single-sheet layout unchanged.
The exporter refuses to overwrite an existing workbook.

## 3. Build

Save and close the workbook. From `tools/`, run:

```powershell
python -X utf8 pack_language.py --input-root ../input --dictionary ../local/translation.xlsx --font ../local/font.ttf --out ../build/my-language
```

For the Japanese test preset, add `--preset ja-test` and use its matching font.
The output directory must be new and outside `input/`. Use a new output name for each build.

Blank translations keep the game's original text. An entirely untranslated workbook is rejected.
The builder checks the source resources, font, characters, atlas capacity, pixels and metrics before publishing output.
Missing glyphs, invalid cells or resource mismatches stop the build.

Take `build/my-language/AC1_Language.bin`. The other files are reports and configuration snapshots;
they are not installed in the game. `import-report.json` records translated and pending row counts.

Close the game, back up your current bin, and put the new bin beside `ac1_runtime_package.asi` in `scripts/`.
Check menus, control labels and scene loading in both Dx9 and Dx10.

## Samples and presets

The [sample dictionaries](../data/README.md) are optional examples. Your own workbook replaces them;
there is no need to generate a sample first. An existing UTF-8 tab-separated dictionary can also be passed to `--dictionary`.

`data/languages/<preset>/language.json` is a supplied font recipe. The packer loads it and connects your translations
automatically. You do not create or edit JSON when translating with an existing preset.

A different font requires a calibrated preset. See [Add a preset](fonts.md#add-a-preset) for configuration fields,
validation and visual checks. Creating a preset does not require another `pack_language.py`.
