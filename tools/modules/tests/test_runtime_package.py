"""Real package round-trip, corruption rejection and atomic publication tests."""

import argparse
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import build_runtime  # noqa: E402
import verify_runtime  # noqa: E402
from _runtime_inputs import (
    DEFAULT_LANGUAGE,
    DEFAULT_ORACLE,
    ROOT,
    corpus,
    dictionary_audit,
    effective_dictionary,
    read_profile,  # noqa: E402
    source_path,
    write_json,
)

PACKAGE = None
PROFILE = DEFAULT_LANGUAGE
FONT = None
INPUT_ROOT = None
ORACLE = DEFAULT_ORACLE
DICTIONARY = ROOT / "data/languages/zh/dictionary.tsv"


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if PACKAGE is None:
            raise unittest.SkipTest(
                "Provide --package/--input-root/--font for owned-data integration tests"
            )
        cls.raw = PACKAGE.read_bytes()
        cls.profile = read_profile(PROFILE, INPUT_ROOT, ORACLE)
        cls.base = struct.unpack_from("<Q", cls.raw, 48)[0]
        cls.rec = struct.unpack_from("<Q", cls.raw, cls.base + 64)[0]
        cls.tex = struct.unpack_from("<Q", cls.raw, cls.base + 80)[0]
        cls.work = tempfile.TemporaryDirectory(prefix="ac1-package-tests-")
        cls.folder = Path(cls.work.name)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def changed_package(self, mutate, update_sections=False, update_global=True):
        raw = bytearray(self.raw)
        mutate(raw)
        if update_sections:
            for index in range(4):
                base = self.base + index * 224
                for offset, digest_at in ((64, 160), (80, 192)):
                    start, size = struct.unpack_from("<QQ", raw, base + offset)
                    raw[base + digest_at : base + digest_at + 32] = hashlib.sha256(
                        raw[start : start + size]
                    ).digest()
        if update_global:
            raw[64:96] = bytes(32)
            raw[64:96] = hashlib.sha256(raw).digest()
        path = self.folder / "mutated.bin"
        path.write_bytes(raw)
        return path

    def reject_structure(self, mutate, message, update_sections=False, update_global=True):
        path = self.changed_package(mutate, update_sections, update_global)
        with self.assertRaisesRegex(ValueError, message):
            verify_runtime.read_package(path, self.profile)

    def reject_font(self, mutate, message, font_index=0):
        path = self.changed_package(mutate, update_sections=True)
        _, _, fonts = verify_runtime.read_package(path, self.profile)
        effective = json.loads(
            (PACKAGE.parent / "dictionary-effective.json").read_text(encoding="utf-8")
        )
        cps, extra = corpus(effective, self.profile)
        with self.assertRaisesRegex(ValueError, message):
            verify_runtime.verify_glyphs(fonts[font_index], cps | extra, FONT)

    def test_01_real_dictionary_and_all_pixels(self):
        report = verify_runtime.verify(
            PACKAGE, PROFILE, font_path=FONT, input_root=INPUT_ROOT, oracle_path=ORACLE
        )
        self.assertEqual(report["effective_keys"], 1466)
        self.assertEqual(report["rejected_lines"], 0)
        self.assertEqual([f["codepoints"] for f in report["fonts"]], [1455] * 4)
        self.assertEqual([f["blank_glyphs"] for f in report["fonts"]], [1] * 4)
        offset, size = struct.unpack_from("<QQ", self.raw, 32)
        self.assertEqual(self.raw[offset : offset + size], DICTIONARY.read_bytes())

    def test_02_truncation(self):
        path = self.folder / "truncated.bin"
        path.write_bytes(self.raw[:-1])
        with self.assertRaisesRegex(ValueError, "length"):
            verify_runtime.read_package(path, self.profile)

    def test_03_wrong_version(self):
        self.reject_structure(lambda b: struct.pack_into("<I", b, 8, 3), "version")

    def test_04_wrong_digest(self):
        self.reject_structure(
            lambda b: b.__setitem__(len(b) - 1, b[-1] ^ 1), "digest", update_global=False
        )

    def test_05_reserved(self):
        self.reject_structure(lambda b: b.__setitem__(99, 1), "language")

    def test_06_count(self):
        self.reject_structure(lambda b: struct.pack_into("<I", b, 28, 5), "count")

    def test_07_overlap_with_recomputed_digest(self):
        dictionary_start = struct.unpack_from("<Q", self.raw, 32)[0]
        self.reject_structure(
            lambda b: struct.pack_into("<Q", b, 40, self.rec - dictionary_start + 16), "Overlapping"
        )

    def test_08_out_of_bounds(self):
        self.reject_structure(
            lambda b: struct.pack_into("<Q", b, self.base + 80, len(b) + 16), "bounds"
        )

    def test_09_alignment(self):
        self.reject_structure(lambda b: struct.pack_into("<Q", b, 32, 1025), "Unaligned")

    def test_10_duplicate_identity(self):
        def mutate(raw):
            raw[self.base + 224 + 16 : self.base + 224 + 24] = raw[self.base + 16 : self.base + 24]

        self.reject_structure(mutate, "Duplicate resource")

    def test_11_unknown_identity(self):
        self.reject_structure(lambda b: struct.pack_into("<I", b, self.base + 16, 123), "identity")

    def test_12_directory_order(self):
        def mutate(raw):
            a, b = self.base, self.base + 224
            raw[a : a + 224], raw[b : b + 224] = raw[b : b + 224], raw[a : a + 224]

        self.reject_structure(mutate, "sorted")

    def test_13_record_bytes(self):
        self.reject_structure(
            lambda b: struct.pack_into("<Q", b, self.base + 72, 31), "segment size"
        )

    def test_14_section_digest(self):
        self.reject_structure(lambda b: b.__setitem__(self.rec, 1), "record digest")

    def test_15_duplicate_codepoint(self):
        cp = struct.unpack_from("<H", self.raw, self.rec)[0]
        self.reject_structure(
            lambda b: struct.pack_into("<H", b, self.rec + 32, cp),
            "ascending",
            update_sections=True,
        )

    def test_16_nan_uv(self):
        self.reject_structure(
            lambda b: struct.pack_into("<f", b, self.rec + 32 + 12, float("nan")),
            "invalid UV",
            update_sections=True,
        )

    def test_17_record_page(self):
        self.reject_structure(
            lambda b: struct.pack_into("<H", b, self.rec + 28, 1), "page", update_sections=True
        )

    def test_18_per_font_coverage(self):
        count = struct.unpack_from("<I", self.raw, self.base + 52)[0]
        self.reject_font(
            lambda b: struct.pack_into("<H", b, self.rec + (count - 1) * 32, 0xFFFE), "coverage"
        )

    def test_19_original_texture_preserved(self):
        at = self.tex + (2048 - 512) * 4096
        self.reject_font(lambda b: b.__setitem__(at, b[at] ^ 1), "original BC2 row")

    def test_20_shared_baseline(self):
        at = self.rec + 32 + 8
        old = struct.unpack_from("<h", self.raw, at)[0]
        self.reject_font(lambda b: struct.pack_into("<h", b, at, old + 1), "baseline mismatch")

    def test_21_filter_guard(self):
        self.reject_font(lambda b: struct.pack_into("<Q", b, self.tex, 15), "filter guard")

    def test_22_white_glyphs(self):
        row = struct.unpack_from("<HHHhhh4fHH", self.raw, self.rec + 32)
        x, y = round(row[6] * 4096), round((1 - row[9]) * 2048)
        at = self.tex + (y // 4 * 1024 + x // 4) * 16

        def mutate(raw):
            struct.pack_into("<QHHI", raw, at, 0xFFFFFFFFFFFFFFFF, 0x07E0, 0x07E0, 0)

        self.reject_font(mutate, "Nonwhite")

    def test_23_unused_region_ink(self):
        at = self.tex + (4096 // 4 - 1) * 16
        self.reject_font(
            lambda b: struct.pack_into("<QHHI", b, at, 15, 0xFFFF, 0xFFFF, 0), "ink outside"
        )

    def test_24_dictionary_unsupported_units(self):
        for value in (b"\\x0000", b"\\xD800", "\U0001f600".encode("utf-8"), b"\\x001F"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "unsupported"):
                dictionary_audit(b"Key\t" + value + b"\n")

    def test_25_production_dictionary_edge_semantics(self):
        with tempfile.TemporaryDirectory(dir=self.folder) as temp:
            work = Path(temp)
            path = work / "edges.txt"
            path.write_bytes(
                b"\xef\xbb\xbf# comment\nA\tfirst\n A \tsecond\\nline...\n#Key\tvalue\nB\t\\x2026\nBroken\t\xff\n"
                + "中文\tvalue\n".encode()
            )
            oracle = source_path(self.profile["oracle_path"])
            with self.assertRaisesRegex(ValueError, "duplicate_key"):
                effective_dictionary(path, oracle, work)
            path.write_text(
                " A \tsecond\\nline...\n#Key\tvalue\nB\t\\x2026\n中文\tvalue\n", encoding="utf-8"
            )
            values, audit = effective_dictionary(path, oracle, work)
            self.assertEqual(
                values, {"A": "second\nline...", "#Key": "value", "B": "...", "中文": "value"}
            )
            self.assertFalse(audit["rejected"])

    def test_26_failed_verification_leaves_no_final(self):
        with tempfile.TemporaryDirectory(dir=self.folder) as temp:
            folder = Path(temp)
            dictionary = folder / "tiny.txt"
            dictionary.write_text("Key\t中\n", encoding="utf-8")
            profile = json.loads(PROFILE.read_text(encoding="utf-8"))
            profile["dictionary"] = dictionary.name
            config = folder / "language.json"
            config.write_text(json.dumps(profile), encoding="utf-8")
            destination = folder / "final"
            original_run = build_runtime.subprocess.run

            def fail_reader(command, **kwargs):
                if Path(command[0]).resolve() == Path(sys.executable).resolve():
                    return build_runtime.subprocess.CompletedProcess(command, 1)
                return original_run(command, **kwargs)

            with patch.object(build_runtime.subprocess, "run", side_effect=fail_reader):
                with self.assertRaisesRegex(ValueError, "Independent package"):
                    build_runtime.build(
                        dictionary,
                        FONT,
                        config,
                        destination,
                        input_root=INPUT_ROOT,
                        oracle_path=ORACLE,
                    )
            self.assertFalse(destination.exists())
            self.assertFalse(list(folder.glob(".runtime-build-*")))

    def test_27_existing_output_preserved(self):
        with tempfile.TemporaryDirectory(dir=self.folder) as temp:
            destination = Path(temp)
            sentinel = destination / "keep.bin"
            sentinel.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                build_runtime.build(
                    DICTIONARY,
                    FONT,
                    PROFILE,
                    destination,
                    input_root=INPUT_ROOT,
                    oracle_path=ORACLE,
                )
            self.assertEqual(sentinel.read_bytes(), b"keep")

    def test_28_nonzero_alignment_padding(self):
        start, size = struct.unpack_from("<QQ", self.raw, 32)
        self.assertNotEqual((start + size) % 16, 0)
        self.reject_structure(lambda b: b.__setitem__(start + size, 1), "padding")

    def test_29_source_font_identity_failure(self):
        with tempfile.TemporaryDirectory(dir=self.folder) as temp:
            folder = Path(temp)
            wrong_font = folder / "wrong.ttf"
            wrong_font.write_bytes(b"not the audited font")
            with self.assertRaisesRegex(ValueError, "fingerprint"):
                build_runtime.build(
                    DICTIONARY,
                    wrong_font,
                    PROFILE,
                    folder / "final",
                    input_root=INPUT_ROOT,
                    oracle_path=ORACLE,
                )
            self.assertFalse((folder / "final").exists())

    def test_30_padded_fonts_reject_corruption_with_valid_digests(self):
        for index, item in enumerate(self.profile["fonts"]):
            if item["bake"] != "padded_ink":
                continue
            base = self.base + index * 224
            records = struct.unpack_from("<Q", self.raw, base + 64)[0]
            pixels = struct.unpack_from("<Q", self.raw, base + 80)[0]
            record = records + 32
            row = struct.unpack_from("<HHHhhh4fHH", self.raw, record)
            self.assertNotEqual(row[0], 0x3000)
            width = item["target_dimensions"][0]
            for kind, mutate, message in (
                (
                    "uv",
                    lambda b, at=record + 20, value=row[8] + 1 / width: struct.pack_into(
                        "<f", b, at, value
                    ),
                    "density mismatch",
                ),
                (
                    "baseline",
                    lambda b, at=record + 8, value=row[4] + 1: struct.pack_into("<h", b, at, value),
                    "baseline mismatch",
                ),
                (
                    "filter_guard",
                    lambda b, at=pixels: struct.pack_into("<Q", b, at, 15),
                    "filter guard",
                ),
            ):
                with self.subTest(font=item["name"], corruption=kind):
                    self.reject_font(mutate, message, index)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, default=PACKAGE)
    parser.add_argument("--language", type=Path, default=DEFAULT_LANGUAGE)
    parser.add_argument("--input-root", type=Path)
    parser.add_argument("--font", type=Path)
    parser.add_argument("--oracle", type=Path, default=DEFAULT_ORACLE)
    parser.add_argument("--dict", type=Path, default=DICTIONARY, dest="dictionary")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.package and (not args.input_root or not args.font):
        parser.error("--package requires --input-root and --font")
    PACKAGE = args.package.resolve() if args.package else None
    PROFILE = args.language.resolve()
    INPUT_ROOT = args.input_root.resolve() if args.input_root else None
    FONT = args.font.resolve() if args.font else None
    ORACLE = args.oracle.resolve()
    DICTIONARY = args.dictionary.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(PackageTests)
    names = [test.id().split(".")[-1] for test in suite]
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if args.report:
        write_json(
            args.report,
            dict(
                status="PASS" if result.wasSuccessful() else "FAIL",
                tests=result.testsRun,
                cases=names,
                failures=[(test.id(), detail) for test, detail in result.failures],
                errors=[(test.id(), detail) for test, detail in result.errors],
            ),
        )
    raise SystemExit(0 if result.wasSuccessful() else 1)
