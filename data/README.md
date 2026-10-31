# Language data

`languages/<preset>/language.json` supplies a calibrated source-font recipe.
The translator entry selects a preset and supplies your dictionary automatically; see [Make a language package](../docs/translate.md).

| Preset | Included sample | Use |
| --- | --- | --- |
| `zh` | `dictionary.sample.tsv`, 8 menu keys | Minimal Chinese example |
| `ja-test` | `dictionary.sample.tsv`, 36 keys | Japanese menu and control-label test |

Samples are optional. To test one, pass its `dictionary.sample.tsv` to `pack_language.py --dictionary` instead of a workbook.
The Chinese preset is the default; add `--preset ja-test` for the Japanese sample. Use the matching source font.
No sample needs to be generated before building your own translations.

## Text dictionary format

UTF-8, one `original<TAB>translation` per line. Empty lines are ignored. A line beginning with `#` is a comment
only when it has no tab, so actual keys may start with `#`. Escapes: `\n`, `\t`, `\\`, `\xNNNN`.

Original keys match exactly after whitespace normalization; case, arrows and decorations matter.
Invalid UTF-8, duplicate normalized keys, empty values, unsupported code points and capacity overflows reject the build.
Translation chains and cycles are rejected so repeated lookups remain stable. U+2026 in translations becomes three ASCII dots
to preserve the original game icon at that code point.

For everyday translation, use Excel through the two public entries. Keep matching keys unchanged and use plain text in cells.

## Font recipes and audit policy

Add a preset for a new source font using the [field reference and calibration steps](../docs/fonts.md#add-a-preset).
The recipe controls rendering; the optional sample demonstrates a few translations.

`notranslate.policy` is an optional offline audit list, one `text<TAB>reason` per line.
A trailing `*` matches a prefix, and `#` starts a comment. It is not compiled into the bin.

Full dictionaries, fonts and game resources are supplied locally and excluded from Git.
