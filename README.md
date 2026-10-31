# AC1 UI Translate

Runtime UI translation for **Assassin's Creed (2008, PC)**, supporting the Dx9 and Dx10 renderers.

One `ac1_runtime_package.asi` loads one `AC1_Language.bin` containing a dictionary and four font atlases.
Text is replaced before the game measures and lays it out. Game archives stay unchanged.

## Install

1. Install a compatible x86 ASI loader that calls `InitializeASI`.
2. Close the game. Put `ac1_runtime_package.asi` and your `AC1_Language.bin` in the game's `scripts/` directory.
3. Start the game. To change languages, replace the bin while the game is closed and restart.

Python, Excel and source fonts are only needed to build a language package.
Runtime logs are written to `scripts/AC1_Runtime/`.

## Make a language package

Follow the [translator guide](docs/translate.md) to prepare game exports, translate an Excel workbook and build a bin.
The translator toolkit includes the ASI and a precompiled dictionary checker. A source checkout needs the
[checker build step](docs/development.md#dictionary-checker).

```text
tools/
  lg_to_excel.py       LG dictionary → Excel translation sheet
  pack_language.py     Excel + game exports + font → AC1_Language.bin
  modules/             Implementation, dependencies and tests
```

The repository includes optional Chinese and Japanese samples. Provide your own translations, game exports
and the font matching your [font preset](docs/fonts.md). Samples are not required for your own package.

## Develop

- [Build and test](docs/development.md)
- [Add or calibrate a font preset](docs/fonts.md#add-a-preset)
- [Runtime implementation](docs/ENGINEERING.md)

Supported hosts and text limitations are listed in [support limits](docs/fonts.md#support-limits).
Full dictionaries, fonts, game resources and generated language packages are not included in the source repository.

## License

Project code and original documentation: [MIT](LICENSE).
See [third-party notices](THIRD_PARTY_NOTICES.md) for MinHook, fonts and game resources.
This project is not affiliated with Ubisoft.
