# Japanese test preset

`ja-test` contains a font recipe and 36 representative translations for menus and control labels.
Unmatched text stays in its original language. This is a test vocabulary, not a full Japanese translation.

Use `--preset ja-test` with `pack_language.py` and either your translated workbook or `dictionary.sample.tsv`.
See [Make a language package](../../../docs/translate.md).

## Source font

Use `NotoSansJP[wght].ttf` from Google Fonts commit
[`66a36c8c94b1a5d992ee4e7f392fccfe4945767c`](https://github.com/google/fonts/tree/66a36c8c94b1a5d992ee4e7f392fccfe4945767c/ofl/notosansjp).

SHA-256: `c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f`.
Obtain the font and its [SIL OFL 1.1 license](https://github.com/google/fonts/blob/66a36c8c94b1a5d992ee4e7f392fccfe4945767c/ofl/notosansjp/OFL.txt) separately.

The sample covers Kana/kanji, precomposed characters, full-width punctuation and spaces, line breaks, and mixed numeric text.
Both renderers require an actual display check for any changed recipe or vocabulary. See [Font presets](../../../docs/fonts.md).
