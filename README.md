# ac1-ui-translate-chinese

Chinese (CJK-capable,but untested ja and kr,may work) UI translation mod for **Assassin's Creed** (2008, PC),
covering both renderers (`AssassinsCreed_Dx9.exe` and `AssassinsCreed_Dx10.exe`).

The mod replaces in-game UI text at runtime and injects the CJK glyphs those
strings need: the original fonts contain no CJK glyphs, so the mod packs
pre-baked glyph atlases into `DataPC.forge` (one-time, offline)(live replacement is much harder on dx10,but if you want to make this,[docs](docs/ENGINEERING.md) may can help you) and extends the
engine's glyph tables in memory.

**Note:** only tested for steam version and chinese. 

## Features

- Full UI text translation through a single verified text funnel
- CJK glyph injection into all four game fonts (textpc / bold / techno / title)
- Works on both the Dx9 and Dx10 renderers

## Install

1. Build the mod (**see release**,you don't need it).
2. (Optional)Make a translate template if you dont have a finish dict.([lg patch](https://community.pcgamingwiki.com/files/file/3594-assassins-creed-chinese-localization-patches) is a chinese dict,but you can use it for other language)
```
cd tools
python make_dict_template.py --dict input\LGCStringDict_01.txt --lang zh --prefill --out out\dict.txt
```
3. Unpacking `DataPC.forge` with AnvilToolkit, then Unpacking  `2_-_Game Bootstrap Settings.data`.
4. Copy 265-268 to `tool/input/mft-dir`, 735-738(**need Save As dds**) to`tool/input/mft-dir`
5. Bake the glyph atlases
```
cd tools
python pack_glyphs.py --mft-dir input/mft-dir --dds-dir input/dds-dir --dict out\dict.txt --ttf input\fonts\NotoSansSC-VariableFont_wght.ttf --out out\glyphs --lang zh
python rgba_to_dds.py --all
```
6. Repack 4 dds in AnvilToolkit. Run `tools/write_forge_sidecar.py --game "<game dir>"` to drop the
   integrity sidecar.
7. Copy `ac1_cjk.asi`, `AC1_CJK_Glyphs.bin` and your `AC1_Dict.txt`
   (UTF-8, `English<TAB>译文`) into the game's `scripts\` folder
   (an ASI loader such as Ultimate-ASI-Loader is required).

**Note on the dictionary:** the source ships as `data/dict.txt` — copy it to
`scripts\AC1_Dict.txt` (or bring your own in the same `ENG<TAB>译文` UTF-8 format).

## Build from source

Requirements: Visual Studio 2022 Build Tools (x86 toolchain), Python 3 +
Pillow (for the offline glyph packer), a CJK font file for baking.

```powershell
pwsh -NoProfile -File build.ps1 -RunTests     # build
```

Output: `app\out\ac1_cjk.asi`.

## How it works (short version)

- Four hooked engine points: one text funnel, three glyph hooks
  (font load / charmap lookup / DrawText).
- Atlases are baked offline and stored in `DataPC.forge` by the installer;
  at runtime the game itself loads them — the mod never touches D3D.
- A startup integrity check (forge size/CRC sidecar) gates every patch:
  if the forge is restored (e.g. Steam verify), the mod goes fully inert.

Internals: see `docs/ENGINEERING.md`. 

## License & credits

- This project's code: MIT (see `LICENSE`).
- [minhook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause), bundled
  under `third_party/`.
- Fonts used for baking are not redistributed; bring your own.
- This is a fan-made mod, not affiliated with or endorsed by Ubisoft.
