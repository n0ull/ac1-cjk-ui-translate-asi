# Font presets

A preset maps one source font to the game's `bold`, `textpc`, `techno` and `title` fonts.
It contains the font fingerprint, sizes, weights, advances, baselines and single-page atlas dimensions.
The packer supplies the current dictionary automatically.

## Supplied presets

| Preset | Source font | Sample |
| --- | --- | --- |
| `zh` | Noto Sans SC variable font, version 2.004-H2 | 8 Chinese menu keys |
| `ja-test` | Noto Sans JP variable font | 36 Japanese test keys |

The filename may change, but the font's SHA-256 must match:

```text
zh
6cbd3ac22ef454e4e9bcfb67369ea94297002a218e9abebc90852534afee096a

ja-test
c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f
```

```powershell
Get-FileHash -Algorithm SHA256 local/font.ttf
```

Font files and their licenses must be obtained separately. The Japanese font has a
[pinned source](../data/languages/ja-test/README.md). A font with the same family name can contain different bytes;
do not replace the fingerprint alone to bypass a mismatch.

## Add a preset

Create `data/languages/<id>/language.json` using a supplied preset as a structural example.
Use a lowercase ID such as `my-language`, at most 31 ASCII characters, with letters, digits and separating hyphens.

| Field | Meaning |
| --- | --- |
| `language_format_version` | `1` |
| `language_id` | Target language/package label |
| `baseline` | `ac1-pc-v1`, the shared game resource identities |
| `dictionary` | Adjacent TSV filename for internal direct builds; the translator entry replaces it |
| `font_sha256` | SHA-256 of the exact source font |
| `extra_characters` | Explicit original UI text needing font coverage without a dictionary translation |
| `fonts` | Exactly four recipes, named `bold`, `textpc`, `techno`, `title` |

For `extra_characters`, use `source_kind: "original_ui_text"`, document `evidence`, and list every character in
`source_text` exactly once in `codepoints` as `U+NNNN`. An empty `source_text` uses an empty list.
Only add characters observed in original UI text; translated characters are collected automatically.

Each font recipe has:

| Field | Meaning |
| --- | --- |
| `target_dimensions` | Power-of-two atlas width and height, each 4–8192 and no smaller than the original |
| `font_px` | Logical source font size; rasterization uses twice this value |
| `advance` | Logical distance to the next character |
| `font_baseline` | Common source-font baseline used for rasterization |
| `native_baseline` | Baseline placement in the game's logical coordinates |
| `density` | `2` |
| `bake` | `bold_cell` or `padded_ink`, matching the selected layout strategy |
| `axes` | Variable-font axis values; use `{}` for a static font |

Start with a few representative translations in Excel and select the preset with `--preset <id>`.
A separate sample file is optional. To publish a sample, save a few reviewed entries as UTF-8 `original<TAB>translation`.

### Calibrate and verify

1. Reconstruct original Latin glyphs from their MFT metrics and DDS pixels as a size and weight reference.
2. Bake the target font through the actual packer, including BC2 alpha quantization. Compare horizontal and vertical
   stroke widths independently; equal weight numbers do not imply equal appearance across fonts.
3. Use one baseline for all glyphs in a font. Centering each cropped glyph vertically causes uneven text.
4. Check all four fonts, highlighted and inactive labels, punctuation and mixed Latin/target-language text in the game.
   Validate both renderers and the intended full vocabulary before treating the preset as supported.

The packer independently checks pixels, metrics, coverage and clipping. It does not infer a new font's visual calibration
or automatically choose atlas sizes. AI can help edit a recipe using these fields and tests; visual verification is still required.

## Support limits

- Supported PC hosts: Dx9 CRC32 `E8936C99`, Dx10 CRC32 `3AF8F9D0`. Original fonts must match the shared baseline.
- Text uses BMP code points and existing left-to-right layout. Complex shaping, bidirectional layout,
  vertical text and supplementary-plane characters are unsupported.
- Each font uses one texture page, preserving original characters and icons. Each font has at most 4096 total records;
  the package limit is 64 MiB.
- Atlas dimensions must also fit the graphics device. The `zh` title preset uses 4096×8192; the runtime does not
  query device texture limits before preparing it. The format limit of 8192 does not guarantee device support.
- The Japanese sample verifies a limited vocabulary, not a complete translation. Full vocabulary capacity needs its own check.

Changing a font, recipe or dictionary requires rebuilding the bin. Changing the installed bin requires restarting the game.
