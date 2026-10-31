"""将 AnvilToolkit 导出物、Excel/TSV 译文和预设对应字体打包为 AC1_Language.bin。"""

import argparse
import json
import re
import struct
import sys
import tempfile
from pathlib import Path

import _runtime_inputs as inputs
import build_runtime

PRESETS = inputs.ROOT / "data/languages"
ANVIL_IDS = {
    "techno": (265, 735),
    "bold": (266, 736),
    "title": (267, 737),
    "textpc": (268, 738),
}


def dds_payload(raw, dimensions):
    """Read one BC2 surface without changing its compressed blocks."""
    if len(raw) < 128 or raw[:4] != b"DDS ":
        raise ValueError("Expected a DDS file exported with Save As DDS")
    header_bytes, flags, height, width, linear_size, depth, mips = struct.unpack_from("<7I", raw, 4)
    pixel_bytes, pixel_flags = struct.unpack_from("<2I", raw, 76)
    caps, caps2 = struct.unpack_from("<2I", raw, 108)
    if header_bytes != 124 or pixel_bytes != 32:
        raise ValueError("Invalid DDS header size")
    required_fields = 0x1 | 0x2 | 0x4 | 0x1000  # CAPS, HEIGHT, WIDTH, PIXELFORMAT.
    has_dimensions_and_format = flags & required_fields == required_fields
    is_fourcc_texture = pixel_flags & 0x4 and caps & 0x1000
    if not has_dimensions_and_format or not is_fourcc_texture:
        raise ValueError("DDS must describe a compressed texture")
    if [width, height] != dimensions:
        raise ValueError(f"DDS dimensions mismatch: expected {dimensions}, got {[width, height]}")
    if mips not in (0, 1) or depth not in (0, 1) or caps2:
        raise ValueError("DDS must contain one 2D surface and one mip")

    fourcc = raw[84:88]
    offset = 128
    if fourcc == b"DX10":
        if len(raw) < 148:
            raise ValueError("Truncated DDS DX10 header")
        pixel_format, dimension, misc, array_size, alpha_mode = struct.unpack_from("<5I", raw, 128)
        if pixel_format != 74:  # DXGI_FORMAT_BC2_UNORM.
            raise ValueError("DDS must use BC2_UNORM, not another DXGI format")
        if dimension != 3 or misc or array_size != 1 or alpha_mode not in (0, 1):
            raise ValueError("DDS DX10 must describe one non-cube 2D surface")
        offset = 148
    elif fourcc != b"DXT3":
        raise ValueError("DDS must use DXT3/BC2; conversion from other formats is unsupported")

    expected_bytes = ((width + 3) // 4) * ((height + 3) // 4) * 16
    if flags & 0x80000 and linear_size != expected_bytes:
        raise ValueError("DDS linear size does not match its BC2 surface")
    payload = raw[offset:]
    if len(payload) != expected_bytes:
        raise ValueError("DDS payload size mismatch; extra mips and trailing data are rejected")
    return payload


def anvil_file(folder, number, extension):
    if not folder.is_dir():
        raise FileNotFoundError(f"Missing AnvilToolkit export directory: {folder}")
    prefix = re.compile(rf"{number}(?:_|\s*-)")
    matches = [
        path
        for path in folder.iterdir()
        if path.is_file() and prefix.match(path.name) and path.suffix.casefold() == extension
    ]
    if len(matches) != 1:
        raise ValueError(
            f"Expected exactly one {number} {extension} export in {folder}; found {len(matches)}"
        )
    return matches[0]


def import_anvil(source, profile):
    observations = []
    for item in profile["fonts"]:
        mft_number, dds_number = ANVIL_IDS[item["name"]]
        mft = anvil_file(source / "mft-dir", mft_number, ".magmamftfile")
        dds = anvil_file(source / "dds-dir", dds_number, ".dds")
        mft_bytes, dds_bytes = mft.read_bytes(), dds.read_bytes()
        if inputs.sha(mft_bytes) != item["mft_sha256"]:
            raise ValueError(f"Original MFT fingerprint mismatch: {mft}")
        try:
            payload = dds_payload(dds_bytes, item["original_dimensions"])
        except ValueError as error:
            raise ValueError(f"{dds.name}: {error}") from error
        if inputs.sha(payload) != item["original_texture_sha256"]:
            raise ValueError(f"Original DDS payload fingerprint mismatch: {dds}")
        for target, data in ((item["mft"], mft_bytes), (item["original_texture"], payload)):
            path = Path(target)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        observations.append(
            dict(
                font=item["name"],
                mft=dict(path=str(mft), bytes=len(mft_bytes), sha256=inputs.sha(mft_bytes)),
                dds=dict(path=str(dds), bytes=len(dds_bytes), sha256=inputs.sha(dds_bytes)),
                payload_sha256=inputs.sha(payload),
            )
        )
    return observations


def pack(input_root, dictionary, font, out, *, preset="zh", oracle=inputs.DEFAULT_ORACLE):
    input_root, dictionary, font, out, oracle = (
        path.resolve() for path in (input_root, dictionary, font, out, oracle)
    )
    if out.exists():
        raise FileExistsError(f"Refusing to overwrite output: {out}")
    if out == input_root or input_root in out.parents:
        raise ValueError("Output must be outside the AnvilToolkit input directory")
    for path in (dictionary, font):
        if not path.is_file():
            raise FileNotFoundError(f"Missing input file: {path}")
    if not re.fullmatch(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*", preset):
        raise ValueError("Preset must name a directory under data/languages")
    preset_path = PRESETS / preset / "language.json"
    if not preset_path.is_file():
        raise ValueError(f"Unknown font preset: {preset}")
    recipe_bytes = preset_path.read_bytes()
    recipe = json.loads(recipe_bytes.decode("utf-8"))
    if inputs.sha(font.read_bytes()) != recipe["font_sha256"]:
        raise ValueError(
            f"Font does not match preset {preset!r}. Use its matching font or a calibrated preset; "
            "the packer does not automatically change font hashes or metrics."
        )
    if not oracle.is_file() or not oracle.with_suffix(".json").is_file():
        raise FileNotFoundError(
            "Missing bundled dictionary checker. Obtain the complete translator toolkit "
            "with dictionary_oracle.exe and dictionary_oracle.json."
        )
    inputs.oracle_provenance(oracle)
    dictionary_source = dictionary.read_bytes()
    dictionary_stats = None
    if dictionary.suffix.casefold() == ".xlsx":
        from translation_sheet import workbook_dictionary

        dictionary_bytes, dictionary_stats = workbook_dictionary(dictionary)
    else:
        if inputs.dictionary_text.looks_like_lg_dict(dictionary):
            raise ValueError(
                "LG dictionaries need tools/lg_to_excel.py first; supply its translated .xlsx"
            )
        dictionary_bytes = dictionary_source
    recipe["dictionary"] = "dictionary.tsv"
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".language-pack-", dir=out.parent) as temporary:
        stage = Path(temporary)
        configuration = stage / "language.json"
        source_dictionary = stage / "dictionary.tsv"
        originals = stage / "originals"
        originals.mkdir()
        inputs.write_json(configuration, recipe)
        source_dictionary.write_bytes(dictionary_bytes)
        profile = inputs.read_profile(configuration, originals, oracle)
        imported = import_anvil(input_root, profile)
        package = stage / "package"
        report = build_runtime.build(
            source_dictionary,
            font,
            configuration,
            package,
            input_root=originals,
            oracle_path=oracle,
            show_success=False,
        )
        inputs.write_json(
            package / "import-report.json",
            dict(
                preset=dict(path=str(preset_path), sha256=inputs.sha(recipe_bytes)),
                dictionary=dict(
                    path=str(dictionary),
                    sha256=inputs.sha(dictionary_source),
                    compiled_sha256=inputs.sha(dictionary_bytes),
                    workbook=dictionary_stats,
                ),
                generated_configuration="profile.json",
                exports=imported,
                tools={
                    str(path.relative_to(inputs.ROOT)): inputs.sha(path.read_bytes())
                    for path in (
                        inputs.ROOT / "tools/pack_language.py",
                        Path(__file__),
                        inputs.HERE / "translation_sheet.py",
                    )
                },
            ),
        )
        # Keep the verified bin and its evidence together; failed builds publish no output.
        package.rename(out)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input-root", type=Path, required=True, help="Contains mft-dir/ and dds-dir/"
    )
    parser.add_argument(
        "--dictionary", type=Path, required=True, help="已填写译文的 .xlsx 或 UTF-8 TAB 词典"
    )
    parser.add_argument(
        "--font", type=Path, required=True, help="Font matching the selected preset"
    )
    parser.add_argument("--out", type=Path, required=True, help="New output directory")
    parser.add_argument(
        "--preset", default="zh", help="Font preset under data/languages (default: zh)"
    )
    developer = parser.add_argument_group("developer options")
    developer.add_argument(
        "--oracle", type=Path, default=inputs.DEFAULT_ORACLE, help="Bundled checker"
    )
    args = parser.parse_args(argv)
    try:
        report = pack(
            args.input_root,
            args.dictionary,
            args.font,
            args.out,
            preset=args.preset,
            oracle=args.oracle,
        )
    except (ImportError, OSError) as error:
        print(f"INPUT_ERROR: {error}", file=sys.stderr)
        return 2
    except (ValueError, KeyError, TypeError, AttributeError, struct.error) as error:
        print(f"PACK_REJECTED: {error}", file=sys.stderr)
        return 1
    print(
        f"OFFLINE_PASS {args.out / 'AC1_Language.bin'}; {report['dictionary']['stats']['keys']} keys"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
