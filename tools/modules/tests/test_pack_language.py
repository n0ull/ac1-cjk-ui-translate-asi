"""Translator inputs, independent DDS fixtures and publication failure checks."""

import copy
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import _runtime_inputs as inputs  # noqa: E402
import language_pack as packer  # noqa: E402


def dds(payload=bytes(range(16)), *, dx10=True):
    raw = bytearray(128)
    raw[:4] = b"DDS "
    struct.pack_into("<7I", raw, 4, 124, 0xA1007, 4, 4, 16, 1, 1)
    struct.pack_into("<2I", raw, 76, 32, 4)
    raw[84:88] = b"DX10" if dx10 else b"DXT3"
    struct.pack_into("<I", raw, 108, 0x1000)
    return bytes(raw) + (struct.pack("<5I", 74, 3, 0, 1, 0) if dx10 else b"") + payload


class DdsTests(unittest.TestCase):
    def test_dx10_and_dxt3_keep_every_compressed_byte(self):
        expected = bytes(range(16))
        for dx10 in (True, False):
            with self.subTest(dx10=dx10):
                self.assertEqual(packer.dds_payload(dds(dx10=dx10), [4, 4]), expected)

    def test_surface_flags_and_format_reject_before_accepting_identical_payload(self):
        invalid = (
            (4, 123, "header size"),
            (8, 0, "compressed texture"),
            (12, 8, "dimensions mismatch"),
            (20, 32, "linear size"),
            (24, 2, "one 2D surface"),
            (28, 2, "one 2D surface"),
            (76, 31, "header size"),
            (80, 0, "compressed texture"),
            (108, 0, "compressed texture"),
            (112, 0x200, "one 2D surface"),
            (128, 75, "BC2_UNORM"),
            (132, 4, "one non-cube 2D"),
            (136, 4, "one non-cube 2D"),
            (140, 2, "one non-cube 2D"),
            (144, 2, "one non-cube 2D"),
        )
        for offset, value, message in invalid:
            with self.subTest(offset=offset, value=value):
                raw = bytearray(dds())
                struct.pack_into("<I", raw, offset, value)
                with self.assertRaisesRegex(ValueError, message):
                    packer.dds_payload(raw, [4, 4])

    def test_other_compression_is_not_silently_reencoded(self):
        for fourcc in (b"DXT1", b"DXT5", b"RGBA"):
            raw = bytearray(dds(dx10=False))
            raw[84:88] = fourcc
            with self.subTest(fourcc=fourcc), self.assertRaisesRegex(ValueError, "DXT3/BC2"):
                packer.dds_payload(raw, [4, 4])

    def test_missing_headers_extra_mips_and_short_payload_are_rejected(self):
        for raw, message in (
            (b"not DDS", "Expected a DDS"),
            (dds()[:127], "Expected a DDS"),
            (dds()[:147], "Truncated DDS DX10"),
            (dds()[:-1], "payload size"),
            (dds() + bytes(16), "payload size"),
        ):
            with self.subTest(bytes=len(raw)), self.assertRaisesRegex(ValueError, message):
                packer.dds_payload(raw, [4, 4])


class TranslatorTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ac1-translator-test-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.exports = self.work / "anvil"
        self.output = self.work / "output"
        self.dictionary = self.work / "translations.tsv"
        self.dictionary.write_bytes(b"BACK\tvalue\n")
        self.font = self.work / "font.ttf"
        self.font.write_bytes(b"font fixture; rasterizer is a separate test surface")
        self.oracle = self.work / "dictionary_oracle.exe"
        self.oracle.write_bytes(b"checker fixture; never executed")
        self.oracle.with_suffix(".json").write_text("{}", encoding="utf-8")
        self.preset = self.work / "presets/fixture/language.json"
        self.preset.parent.mkdir(parents=True)
        self.recipe = json.loads(inputs.DEFAULT_LANGUAGE.read_text(encoding="utf-8"))
        self.recipe["font_sha256"] = inputs.sha(self.font.read_bytes())
        self.recipe["dictionary"] = "sample.tsv"
        self.preset.write_text(json.dumps(self.recipe), encoding="utf-8")
        self.profile = dict(fonts=[])
        # Export identifiers and bytes are independently specified, not read from the importer map.
        for name, mft_number, dds_number in (
            ("bold", 266, 736),
            ("techno", 265, 735),
            ("textpc", 268, 738),
            ("title", 267, 737),
        ):
            mft = f"{mft_number}_-_{name}.MagmaMftFile"
            texture = f"{dds_number}_-_{name}.dds"
            mft_bytes = (name + " original MFT").encode()
            pixels = bytes([mft_number % 256]) * 16
            for folder, filename, data in (
                ("mft-dir", mft, mft_bytes),
                ("dds-dir", texture, dds(pixels)),
            ):
                path = self.exports / folder / filename
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            self.profile["fonts"].append(
                dict(
                    name=name,
                    original_dimensions=[4, 4],
                    mft_sha256=inputs.sha(mft_bytes),
                    original_texture_sha256=inputs.sha(pixels),
                    mft=str(self.work / "converted/mft" / mft),
                    original_texture=str(self.work / "converted/textures" / (name + ".bc2")),
                )
            )

    def pack(self):
        return packer.pack(
            self.exports,
            self.dictionary,
            self.font,
            self.output,
            preset="fixture",
            oracle=self.oracle,
        )

    def test_numbered_exports_map_to_the_right_fonts_without_changing_sources(self):
        sources = {p: p.read_bytes() for p in self.exports.rglob("*") if p.is_file()}
        observations = packer.import_anvil(self.exports, self.profile)
        self.assertEqual(
            [row["font"] for row in observations], ["bold", "techno", "textpc", "title"]
        )
        for item in self.profile["fonts"]:
            self.assertEqual(inputs.sha(Path(item["mft"]).read_bytes()), item["mft_sha256"])
            self.assertEqual(
                inputs.sha(Path(item["original_texture"]).read_bytes()),
                item["original_texture_sha256"],
            )
        self.assertTrue(all(path.read_bytes() == raw for path, raw in sources.items()))

    def test_duplicate_or_missing_exports_do_not_choose_a_file_arbitrarily(self):
        duplicate = self.exports / "dds-dir/736_-_duplicate.dds"
        duplicate.write_bytes(dds())
        with self.assertRaisesRegex(ValueError, "found 2"):
            packer.import_anvil(self.exports, self.profile)
        duplicate.unlink()
        next((self.exports / "dds-dir").glob("736_*")).unlink()
        with self.assertRaisesRegex(ValueError, "found 0"):
            packer.import_anvil(self.exports, self.profile)

    def test_lookalike_number_is_not_selected(self):
        (self.exports / "dds-dir/7360_-_unrelated.dds").write_bytes(b"unrelated data")
        packer.import_anvil(self.exports, self.profile)

    def test_modified_original_files_are_rejected(self):
        mft = next((self.exports / "mft-dir").glob("266_*"))
        original = mft.read_bytes()
        mft.write_bytes(original + b"changed")
        with self.assertRaisesRegex(ValueError, "MFT fingerprint"):
            packer.import_anvil(self.exports, self.profile)
        mft.write_bytes(original)
        texture = next((self.exports / "dds-dir").glob("736_*"))
        texture.write_bytes(dds(bytes(16)))
        with self.assertRaisesRegex(ValueError, "DDS payload fingerprint"):
            packer.import_anvil(self.exports, self.profile)

    def test_existing_output_and_input_overlap_are_refused(self):
        self.output.mkdir()
        sentinel = self.output / "keep"
        sentinel.write_bytes(b"keep")
        with self.assertRaises(FileExistsError):
            self.pack()
        self.assertEqual(sentinel.read_bytes(), b"keep")
        self.output = self.exports / "output"
        with self.assertRaisesRegex(ValueError, "outside"):
            self.pack()

    def test_unknown_font_is_not_accepted_by_changing_the_preset_hash(self):
        self.font.write_bytes(b"another font")
        before = self.preset.read_bytes()
        with patch.object(packer, "PRESETS", self.work / "presets"):
            with self.assertRaisesRegex(ValueError, "Font does not match preset"):
                self.pack()
        self.assertEqual(before, self.preset.read_bytes())
        self.assertFalse(self.output.exists())

    def test_missing_checker_has_a_translator_facing_error(self):
        self.oracle.with_suffix(".json").unlink()
        with patch.object(packer, "PRESETS", self.work / "presets"):
            with self.assertRaisesRegex(FileNotFoundError, "complete translator toolkit"):
                self.pack()
        self.assertFalse(self.output.exists())

    def test_generated_recipe_comes_from_the_preset_and_failure_publishes_nothing(self):
        before = self.preset.read_bytes()
        observed = {}

        def fail(dictionary, font, configuration, destination, **kwargs):
            observed.update(json.loads(configuration.read_text(encoding="utf-8")))
            self.assertEqual(dictionary.read_bytes(), self.dictionary.read_bytes())
            destination.mkdir()
            (destination / "partial.bin").write_bytes(b"partial output")
            raise ValueError("independent verification rejected the package")

        def profile(configuration, originals, oracle):
            value = copy.deepcopy(self.profile)
            for item in value["fonts"]:
                item["mft"] = str(originals / "mft" / Path(item["mft"]).name)
                item["original_texture"] = str(
                    originals / "textures" / Path(item["original_texture"]).name
                )
            return value

        with (
            patch.object(packer, "PRESETS", self.work / "presets"),
            patch.object(inputs, "oracle_provenance", return_value={}),
            patch.object(inputs, "read_profile", side_effect=profile),
            patch.object(packer.build_runtime, "build", side_effect=fail),
        ):
            with self.assertRaisesRegex(ValueError, "independent verification"):
                self.pack()
        expected = dict(self.recipe, dictionary="dictionary.tsv")
        self.assertEqual(observed, expected)
        self.assertEqual(before, self.preset.read_bytes())
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.work.glob(".language-pack-*")))

    def test_help_works_without_inputs_or_raster_dependencies(self):
        result = subprocess.run(
            [sys.executable, "-X", "utf8", str(TOOLS.parent / "pack_language.py"), "--help"],
            cwd=self.work,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--dictionary", result.stdout)


if __name__ == "__main__":
    unittest.main()
