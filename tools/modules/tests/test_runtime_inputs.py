"""Fast contract checks; no game data, native compiler or raster dependencies required."""

import ast
import contextlib
import copy
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import _dictionary as dictionary_text  # noqa: E402
import _runtime_inputs as inputs  # noqa: E402


class TextRuleTests(unittest.TestCase):
    def test_key_normalization_and_value_whitespace_are_distinct(self):
        for source, expected in (
            ("  a \t b\r\nc  ", "a b c"),
            (" MiXeD ", "MiXeD"),
            (" \t\r\n", ""),
        ):
            with self.subTest(source=source):
                self.assertEqual(dictionary_text.canon(source), expected)
        self.assertEqual(dictionary_text.trim_value(" \t中  文\n下一行\r\n"), "中  文\n下一行")

    def test_hex_escapes_require_exactly_four_hex_digits(self):
        for source, expected in (("4E2D", "中"), ("00a4", "¤")):
            with self.subTest(source=source):
                self.assertEqual(dictionary_text.hex4(source), expected)
                self.assertEqual(dictionary_text.unescape_value("a\\x" + source), "a" + expected)
        for digits in ("4E2Z", " 123", "+123", "1_23", "123", ""):
            with self.subTest(digits=digits):
                self.assertIsNone(dictionary_text.hex4(digits))
                literal = "a\\x" + digits
                self.assertEqual(dictionary_text.unescape_value(literal), literal)

    def test_literal_backslashes_and_unknown_escapes_remain_literal(self):
        for source, expected in (
            (r"a\nb\tc", "a\nb\tc"),
            (r"x\\ny", r"x\ny"),
            (r"path\\to\\file", r"path\to\file"),
            (r"%1% \/ %2%", r"%1% \/ %2%"),
            ("tail\\", "tail\\"),
        ):
            with self.subTest(source=source):
                self.assertEqual(dictionary_text.unescape_value(source), expected)


class InputContractTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ac1-runtime-contract-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.template = json.loads(inputs.DEFAULT_LANGUAGE.read_text(encoding="utf-8"))
        self.baseline = json.loads(
            (inputs.BASELINES / "ac1-pc-v1.json").read_text(encoding="utf-8")
        )
        override = patch.object(inputs, "BASELINES", self.work)
        override.start()
        self.addCleanup(override.stop)

    def test_required_text_inputs_cannot_silently_disappear(self):
        for loader in (dictionary_text.load_supplement, dictionary_text.load_notranslate):
            for path in (None, self.work / "missing.txt"):
                with self.subTest(loader=loader.__name__, path=path):
                    with contextlib.redirect_stdout(io.StringIO()):
                        with self.assertRaises(SystemExit) as rejected:
                            loader(path)
                    self.assertEqual(rejected.exception.code, 2)

    def profile(self, mutate=None, baseline_mutate=None):
        value = copy.deepcopy(self.template)
        baseline = copy.deepcopy(self.baseline)
        if mutate:
            mutate(value)
        if baseline_mutate:
            baseline_mutate(baseline)
        (self.work / "ac1-pc-v1.json").write_text(json.dumps(baseline), encoding="utf-8")
        path = self.work / "language.json"
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return inputs.read_profile(path, self.work)

    def oracle(self):
        root = self.work / "source"
        fingerprints = {}
        for name in inputs.ORACLE_SOURCES:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
            fingerprints[name] = inputs.sha(path.read_bytes())
        executable = self.work / "dictionary_oracle.exe"
        executable.write_bytes(b"oracle fixture; never executed")
        manifest = dict(
            format_version=1,
            target="x86",
            exe_sha256=inputs.sha(executable.read_bytes()),
            sources=fingerprints,
        )
        executable.with_suffix(".json").write_text(json.dumps(manifest), encoding="utf-8")
        return root, executable

    def test_profile_paths_are_relative_to_explicit_input_root(self):
        profile = self.profile()
        for font in profile["fonts"]:
            for field in ("mft", "original_texture"):
                self.assertTrue(Path(font[field]).is_absolute())
                self.assertEqual(Path(font[field]).parents[1], self.work)

    def test_profile_does_not_relax_native_budgets(self):
        for key, value in (("max_package_bytes", 67108865), ("max_total_records_per_font", 4097)):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "budget"):
                self.profile(
                    baseline_mutate=lambda p, key=key, value=value: p.__setitem__(key, value)
                )

    def test_profile_rejects_old_shape_and_boolean_versions(self):
        for value in (2, True, 1.0):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "version"):
                self.profile(lambda p, value=value: p.__setitem__("language_format_version", value))

    def test_profile_rejects_invalid_dimensions(self):
        for dimensions in ([2047, 2048], [True, 2048], [2048], "2048x2048"):
            with self.subTest(dimensions=dimensions), self.assertRaises(ValueError):
                self.profile(
                    lambda p, dimensions=dimensions: p["fonts"][0].__setitem__(
                        "target_dimensions", dimensions
                    )
                )

    def test_profile_rejects_duplicate_native_identity(self):
        def duplicate(profile):
            for key in ("asset_id", "class_hash"):
                profile["fonts"][1][key] = profile["fonts"][0][key]

        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.profile(baseline_mutate=duplicate)

    def test_profile_requires_all_four_fonts(self):
        with self.assertRaisesRegex(ValueError, "four"):
            self.profile(lambda p: p["fonts"].pop())

    def test_extra_characters_require_matching_documented_source(self):
        for change in (dict(evidence=""), dict(codepoints=["U+6309"]), dict(codepoints=["U+ZZZZ"])):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.profile(lambda p, change=change: p["extra_characters"].update(change))

    def test_dictionary_controls_fail_even_in_overridden_lines(self):
        for value in (b"\\x0000", b"\\xD800", b"\\x001F", "😀".encode()):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "unsupported"):
                inputs.dictionary_audit(b"Key\t" + value + b"\nKey\tsafe\n")

    def test_valid_oracle_provenance(self):
        root, executable = self.oracle()
        with patch.object(inputs, "ROOT", root):
            self.assertEqual(inputs.oracle_provenance(executable)["target"], "x86")

    def test_lg_input_must_be_exported_explicitly(self):
        dictionary = self.work / "wrong-dictionary.txt"
        for marker in (b"xfhsm_res_ENG_Start", b"xfhsm_res_CHI_Start"):
            with self.subTest(marker=marker):
                dictionary.write_bytes(marker + b"\nKey\tvalue\n")
                with self.assertRaisesRegex(ValueError, "export LG input explicitly"):
                    inputs.effective_dictionary(dictionary, self.work / "absent.exe", self.work)

    def test_escaped_whitespace_keeps_current_query_and_audit_policies(self):
        dictionary = self.work / "whitespace.txt"
        raw = "Valid\t中\nKey\t\\n\n".encode()
        dictionary.write_bytes(raw)
        self.assertEqual(dictionary_text.load_supplement(dictionary), {"Valid": "中", "Key": "\n"})
        self.assertEqual(
            inputs.dictionary_audit(raw)["rejected"],
            [dict(line=2, key="Key", reason="empty_key_or_value")],
        )

    def test_stale_oracle_production_source_is_rejected(self):
        root, executable = self.oracle()
        (root / "modules/dict/src/dict.cpp").write_bytes(b"changed semantics")
        with patch.object(inputs, "ROOT", root), self.assertRaisesRegex(ValueError, "Stale"):
            inputs.oracle_provenance(executable)

    def test_changed_shared_toolchain_is_rejected_by_oracle_provenance(self):
        root, executable = self.oracle()
        (root / "tools/modules/native-toolchain.ps1").write_bytes(b"changed toolchain")
        with (
            patch.object(inputs, "ROOT", root),
            self.assertRaisesRegex(ValueError, "Stale.*tools/modules/native-toolchain.ps1"),
        ):
            inputs.oracle_provenance(executable)

    def test_oracle_receipt_cannot_omit_the_shared_toolchain(self):
        self.assertIn("tools/modules/native-toolchain.ps1", inputs.ORACLE_SOURCES)
        root, executable = self.oracle()
        path = executable.with_suffix(".json")
        manifest = json.loads(path.read_text(encoding="utf-8"))
        del manifest["sources"]["tools/modules/native-toolchain.ps1"]
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with (
            patch.object(inputs, "ROOT", root),
            self.assertRaisesRegex(ValueError, "source set mismatch"),
        ):
            inputs.oracle_provenance(executable)

    def test_oracle_rejection_reason_reaches_the_build_caller(self):
        root, executable = self.oracle()
        dictionary = self.work / "input.txt"
        dictionary.write_bytes(b"Key\tvalue\n")
        result = subprocess.CompletedProcess(
            [], 3, b"result=rejected reason=value_pool_full failure_line=17\n", b""
        )
        with (
            patch.object(inputs, "ROOT", root),
            patch.object(inputs.subprocess, "run", return_value=result),
            self.assertRaisesRegex(ValueError, "exit 3.*value_pool_full failure_line=17"),
        ):
            inputs.effective_dictionary(dictionary, executable, self.work)

    def test_dictionary_queries_accept_nonlatin_without_a_format_selector(self):
        source = self.work / "queries.tsv"
        source.write_text("Key\tvalue\n日本語\t訳文\n", encoding="utf-8")
        self.assertEqual(
            dictionary_text.load_supplement(source), {"Key": "value", "日本語": "訳文"}
        )

    def test_oracle_executable_mismatch_is_rejected(self):
        root, executable = self.oracle()
        executable.write_bytes(b"changed executable")
        with (
            patch.object(inputs, "ROOT", root),
            self.assertRaisesRegex(ValueError, "executable fingerprint"),
        ):
            inputs.oracle_provenance(executable)

    def test_reader_has_no_writer_or_raster_import(self):
        tree = ast.parse((TOOLS / "verify_runtime.py").read_text(encoding="utf-8"))
        imports = {node.module for node in ast.walk(tree) if isinstance(node, ast.ImportFrom)}
        imports.update(
            alias.name
            for node in ast.walk(tree)
            if isinstance(node, ast.Import)
            for alias in node.names
        )
        self.assertFalse({"build_runtime", "_runtime_raster"} & imports)
        for name in (
            "build_runtime.py",
            "verify_runtime.py",
            "_runtime_inputs.py",
            "_runtime_raster.py",
        ):
            source = (TOOLS / name).read_text(encoding="utf-8")
            self.assertNotIn(".scratch", source)
            self.assertNotIn("sys.path.insert", source)

    def test_command_help_works_outside_checkout(self):
        for name in ("build_runtime.py", "verify_runtime.py"):
            result = subprocess.run(
                [sys.executable, str(TOOLS / name), "--help"],
                cwd=self.work,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_existing_output_is_preserved_through_public_cli(self):
        destination = self.work / "keep"
        destination.mkdir()
        sentinel = destination / "keep.bin"
        sentinel.write_bytes(b"keep")
        result = subprocess.run(
            [
                sys.executable,
                str(TOOLS / "build_runtime.py"),
                "--font",
                "absent",
                "--input-root",
                str(self.work),
                "--out",
                str(destination),
            ],
            cwd=self.work,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(sentinel.read_bytes(), b"keep")

    def test_existing_verification_report_is_preserved(self):
        report = self.work / "previous.json"
        report.write_bytes(b"previous receipt")
        result = subprocess.run(
            [
                sys.executable,
                str(TOOLS / "verify_runtime.py"),
                "absent",
                "--font",
                "absent",
                "--input-root",
                str(self.work),
                "--report",
                str(report),
            ],
            cwd=self.work,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(report.read_bytes(), b"previous receipt")


if __name__ == "__main__":
    unittest.main(verbosity=2)
