"""Language configuration, v2 wire contract and frozen-runtime reuse without game assets."""

import argparse
import copy
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import _runtime_inputs as inputs  # noqa: E402
import build_runtime as writer  # noqa: E402
import runtime_candidate as candidate  # noqa: E402
import verify_runtime as reader  # noqa: E402


def synthetic_package(label="ja", record=None):
    fonts = []
    for index, name in enumerate(("bold", "techno", "textpc", "title")):
        records = record if index == 0 and record else b""
        size = 16 if records else 4
        item = dict(
            name=name,
            asset_id=index + 1,
            class_hash=1,
            original_dimensions=[4, 4],
            target_dimensions=[size, size],
            original_count=1,
            density=2,
            advance=2,
            original_texture_sha256=inputs.sha(bytes(16)),
            original_records_sha256="0" * 64,
        )
        fonts.append(
            dict(
                profile=item,
                appended=len(records) // 32,
                records=records,
                payload=bytes(size * size),
            )
        )
    profile = dict(
        runtime_package_version=2,
        dictionary_rules_version=2,
        language_id=label,
        max_package_bytes=67108864,
        max_total_records_per_font=4096,
        fonts=[f["profile"] for f in fonts],
    )
    return writer.serialize(b"A\tB\n", fonts, 67108864, label), profile


class LanguagePackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ac1-language-package-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.path = self.work / "package.bin"

    def test_zero_additions_have_empty_records_and_original_dimensions(self):
        raw, profile = synthetic_package()
        self.path.write_bytes(raw)
        _raw, dictionary, fonts = reader.read_package(self.path, profile)
        self.assertEqual(dictionary, b"A\tB\n")
        self.assertTrue(all(not font["rows"] and len(font["payload"]) == 16 for font in fonts))
        self.assertEqual(raw[96:128], b"ja" + bytes(30))

    def test_wire_rejects_bad_label_padding_unknown_rules_and_version(self):
        raw, profile = synthetic_package()
        for at, value in (
            (96, b"JA"),
            (99, b"x"),
            (60, struct.pack("<I", 1)),
            (8, struct.pack("<I", 3)),
        ):
            with self.subTest(at=at):
                altered = bytearray(raw)
                altered[at : at + len(value)] = value
                altered[64:96] = bytes(32)
                altered[64:96] = hashlib.sha256(altered).digest()
                self.path.write_bytes(altered)
                with self.assertRaises(ValueError):
                    reader.read_package(self.path, profile)

    def test_nonzero_page_and_unsupported_codepoints_reject(self):
        for cp, page in ((0x0416, 1), (0xD800, 0), (127, 0)):
            with self.subTest(cp=cp, page=page):
                record = struct.pack(
                    "<HHHhhh4fHH", cp, 1, 1, 0, 0, 2, 0.25, 0.625, 0.375, 0.75, page, 0
                )
                raw, profile = synthetic_package(record=record)
                self.path.write_bytes(raw)
                with self.assertRaises(ValueError):
                    reader.read_package(self.path, profile)

    def test_retired_wire_is_rejected_even_with_a_matching_embedded_digest(self):
        raw, profile = synthetic_package()
        altered = bytearray(raw)
        altered[:8] = b"AC1RTPK\0"
        struct.pack_into("<I", altered, 8, 1)
        altered[64:96] = hashlib.sha256(altered[:64] + bytes(32) + altered[96:]).digest()
        self.path.write_bytes(altered)
        with self.assertRaisesRegex(ValueError, "Bad magic"):
            reader.read_package(self.path, profile)


class LanguageConfigurationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ac1-language-config-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.language = candidate.read_json(inputs.DEFAULT_LANGUAGE)
        self.baseline = candidate.read_json(inputs.BASELINES / "ac1-pc-v1.json")
        self.config = self.work / "language.json"
        override = patch.object(inputs, "BASELINES", self.work)
        override.start()
        self.addCleanup(override.stop)

    def read(self):
        self.config.write_text(json.dumps(self.language), encoding="utf-8")
        (self.work / "ac1-pc-v1.json").write_text(json.dumps(self.baseline), encoding="utf-8")
        return inputs.read_profile(self.config, self.work)

    def test_language_and_shared_identity_are_composed_once(self):
        profile = self.read()
        self.assertEqual(profile["language_id"], "zh")
        self.assertEqual(profile["runtime_package_version"], 2)
        self.assertEqual(profile["fonts"][0]["asset_id"], self.baseline["fonts"][0]["asset_id"])
        self.assertEqual(profile["fonts"][0]["axes"], {"wght": 485})
        self.assertEqual(profile["dictionary_path"], str(self.work / "dictionary.tsv"))

    def test_language_cannot_override_original_resource_identity(self):
        self.language["fonts"][0]["asset_id"] = 123
        with self.assertRaisesRegex(ValueError, "recipe fields"):
            self.read()

    def test_bad_identifiers_and_nonlocal_dictionary_paths_reject(self):
        original = copy.deepcopy(self.language)
        for label in ("", "JA", "ja-", "ja--jp", "../ja", "a" * 32):
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, "identifier"):
                self.language = dict(original, language_id=label)
                self.read()
        for name in ("../dictionary.tsv", "C:/dictionary.tsv", "sub\\dictionary.tsv"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "adjacent"):
                self.language = dict(original, dictionary=name)
                self.read()

    def test_static_font_recipe_and_empty_explicit_supplement_are_representable(self):
        for font in self.language["fonts"]:
            font["axes"] = {}
        self.language["extra_characters"].update(source_text="", codepoints=[])
        self.assertTrue(all(not f["axes"] for f in self.read()["fonts"]))


class FrozenRuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ac1-frozen-runtime-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.runtime = self.work / "runtime"
        self.runtime.mkdir()
        for name in candidate.RUNTIME_CONTENT_FILES:
            (self.runtime / name).write_bytes(b"synthetic receipt test; never executed")
        (self.runtime / candidate.ASI).write_bytes(
            b"\n".join(
                (
                    candidate.MARKER,
                    candidate.STARTUP_MARKER,
                    candidate.HOST_ADAPTER_MARKER,
                    candidate.LANGUAGE_MARKER,
                )
            )
        )
        self.sources = candidate.native_sources()
        oracle = dict(
            format_version=1,
            target="x86",
            exe_sha256=inputs.sha((self.runtime / "dictionary_oracle.exe").read_bytes()),
            sources={name: self.sources[name]["sha256"] for name in inputs.ORACLE_SOURCES},
        )
        (self.runtime / "dictionary_oracle.json").write_text(json.dumps(oracle), encoding="utf-8")
        self.receipt = dict(
            format_version=1,
            version="0.2.0-rc.1",
            status="RUNTIME_OFFLINE_PASS",
            native="NOT_TESTED_BY_TOOL",
            package_version=2,
            dictionary_rules_version=2,
            hosts=candidate.HOSTS,
            sources=self.sources,
            native_tests={},
            pe={},
            files={
                name: candidate.fingerprint(self.runtime / name)
                for name in candidate.RUNTIME_CONTENT_FILES
            },
        )
        (self.runtime / "runtime-build.json").write_text(json.dumps(self.receipt), encoding="utf-8")

    def test_runtime_receipt_binds_asi_oracle_and_same_sources(self):
        candidate.verify_runtime(self.runtime, current=True)
        (self.runtime / candidate.ASI).write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            candidate.verify_runtime(self.runtime)

    def test_native_source_changes_require_a_new_runtime_but_historical_read_does_not(self):
        with patch.object(candidate, "native_sources", return_value={}):
            candidate.verify_runtime(self.runtime)
            with self.assertRaisesRegex(ValueError, "Native sources changed"):
                candidate.verify_runtime(self.runtime, current=True)

    def test_language_candidate_reuses_the_frozen_asi_without_native_build(self):
        def package_step(_command, log):
            log.write_text("package construction fixture", encoding="utf-8")
            package = log.parent.parent / "package"
            package.mkdir()
            raw, _profile = synthetic_package("zh")
            (package / candidate.PACKAGE).write_bytes(raw)
            for name in candidate.PACKAGE_REPORTS | {"baseline.json"}:
                (package / name).write_text("{}", encoding="utf-8")
            report = dict(
                status="OFFLINE_PASS",
                package_version=2,
                language_id="zh",
                package_bytes=len(raw),
                package_sha256=inputs.sha(raw),
                font_sha256="0" * 64,
                oracle_sha256=self.receipt["files"]["dictionary_oracle.exe"]["sha256"],
            )
            for filename, source in (
                ("profile.json", inputs.DEFAULT_LANGUAGE),
                ("baseline.json", inputs.BASELINES / "ac1-pc-v1.json"),
            ):
                (package / filename).write_bytes(source.read_bytes())
            report.update(
                baseline_id="ac1-pc-v1",
                profile_sha256=inputs.sha((package / "profile.json").read_bytes()),
                baseline_sha256=inputs.sha((package / "baseline.json").read_bytes()),
            )
            for name in ("build-report.json", "verify-report.json"):
                (package / name).write_text(json.dumps(report), encoding="utf-8")

        args = argparse.Namespace(
            out=self.work / "candidate",
            runtime=self.runtime,
            language=inputs.DEFAULT_LANGUAGE,
            font=self.work / "source.ttf",
            input_root=self.work,
            version="0.2.0-rc.1",
        )
        with (
            patch.object(candidate, "run", side_effect=package_step),
            patch.object(candidate, "font_notices", return_value=""),
            patch.object(candidate, "native_build", side_effect=AssertionError("must reuse")),
        ):
            manifest = candidate.build(args)
        self.assertEqual(manifest["format_version"], 4)
        self.assertEqual(
            manifest["files"][f"install/scripts/{candidate.ASI}"],
            self.receipt["files"][candidate.ASI],
        )
        self.assertEqual(len(candidate.verify(args.out)["hosts"]), 2)


if __name__ == "__main__":
    unittest.main()
