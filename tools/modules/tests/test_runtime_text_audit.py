"""Bound conclusions from current inventories without turning prefixes into dictionary keys."""

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import audit_runtime_text as audit  # noqa: E402

HEADER = "# === run 2026-10-19 17:25:22 pid=44108 词典=runtime package keys=3 ===\n"


class TextAuditTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ac1-text-audit-test-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.candidate = self.work / "candidate"
        self.package_evidence = self.candidate / "evidence/package"
        self.package_evidence.mkdir(parents=True)
        (self.candidate / "manifest.json").write_text("{}", encoding="utf-8")
        self.policy = self.work / "notranslate.txt"
        self.policy.write_text("_Yes\t内部键\nOBSOLETE: *\t废弃\n", encoding="utf-8")
        self.dictionary({"BACK": "返回", "START": "开始", "TO:": "收件人："})
        manifest = dict(version="test", files={"install/scripts/AC1_Language.bin": {}})
        verifier = patch.object(audit.runtime_candidate, "verify", return_value=manifest)
        self.verify = verifier.start()
        self.addCleanup(verifier.stop)

    def dictionary(self, pairs):
        (self.package_evidence / "dictionary-effective.json").write_text(
            json.dumps(pairs), encoding="utf-8"
        )
        (self.package_evidence / "build-report.json").write_text(
            json.dumps(dict(dictionary=dict(stats=dict(keys=len(pairs))))), encoding="utf-8"
        )

    def inventory(self, lines, name="strings.txt"):
        path = self.work / name
        path.write_text(HEADER + "\n".join(lines) + "\n", encoding="utf-8")
        return path

    def report(self, *paths):
        return audit.build_report(self.candidate, paths, self.policy)

    def test_exact_matches_policy_and_nonascii_text_are_distinguished(self):
        path = self.inventory(
            [
                "[×1] 替换 caller=00892C8B BACK",
                "[×1] 未命中 caller=00892EC7 Start",
                "[×1] 未命中 caller=00892C8B _Yes",
                "[×1] 未命中 caller=00892C8B OBSOLETE: MENU",
                "[×1] 未命中 caller=00892C8B 02 46 18 54",
                "[×1] 未命中·非ASCII caller=00892C8B Jerusalén",
            ]
        )
        report = self.report(path)
        entries = {row["text"]: row for row in report["entries"]}
        self.assertEqual(entries["BACK"]["category"], "replaced")
        self.assertEqual(entries["Start"]["category"], "review")
        self.assertEqual(entries["Start"]["case_candidates"], ["START"])
        self.assertEqual(entries["_Yes"]["category"], "excluded")
        self.assertEqual(entries["OBSOLETE: MENU"]["category"], "excluded")
        self.assertEqual(entries["02 46 18 54"]["category"], "no_letters")
        self.assertEqual(entries["Jerusalén"]["category"], "review")
        self.verify.assert_called_once_with(self.candidate)

    def test_48_unit_keys_and_colliding_prefixes_are_never_counted_as_complete(self):
        prefix = "A" * 48
        self.dictionary({prefix: "完整但无法证明", prefix + "tail": "另一个键"})
        path = self.inventory(["[×1] 替换 caller=00892C8B " + prefix])
        entry = self.report(path)["entries"][0]
        self.assertEqual(entry["category"], "prefix_only")
        self.assertEqual(entry["dictionary_prefix_candidates"], [prefix, prefix + "tail"])
        self.assertNotIn("translation", entry)

    def test_new_nonascii_and_unchanged_tags_do_not_claim_replacement(self):
        self.dictionary({"On": "On", "日本語": "訳文"})
        path = self.inventory(
            [
                "[×1] 同值 caller=00892C8B On",
                "[×1] 未命中·非ASCII caller=00892C8B 未訳",
                "[×1] 命中 caller=00892C8B 日本語",
            ]
        )
        report = self.report(path)
        entries = {row["text"]: row for row in report["entries"]}
        self.assertEqual(entries["On"]["category"], "unchanged")
        self.assertEqual(entries["未訳"]["category"], "review")
        self.assertEqual(entries["日本語"]["category"], "in_dictionary")
        self.assertEqual(report["format_version"], 2)

    def test_length_is_utf16_units_rather_than_unicode_code_points(self):
        text = "🙂" * 23 + "AB"
        self.assertEqual(len(text), 25)
        path = self.inventory(["[×1] 未命中 caller=00892C8B " + text])
        entry = self.report(path)["entries"][0]
        self.assertEqual(entry["utf16_units"], 48)
        self.assertEqual(entry["category"], "prefix_only")

    def test_snapshot_counts_and_statuses_are_kept_per_observation(self):
        first = self.inventory(["[×4] 命中 caller=00892C8B BACK"])
        second = self.inventory(["[×2] 替换 caller=00892EC7 BACK"], "second.txt")
        entry = self.report(first, second)["entries"][0]
        self.assertEqual(entry["category"], "replaced")
        self.assertEqual(entry["statuses"], ["命中", "替换"])
        self.assertEqual([r["count_snapshot"] for r in entry["observations"]], [4, 2])
        self.assertEqual([r["inventory"] for r in entry["observations"]], [0, 1])
        self.assertNotIn("count", entry)

    def test_capacity_candidate_is_not_a_capacity_failure(self):
        path = self.inventory(["[×1] 替换 caller=00892C8B TO:"])
        (row,) = self.report(path)["capacity_candidates"]
        self.assertEqual((row["key_units"], row["value_units"]), (3, 4))
        self.assertEqual(row["capacity"], "UNKNOWN")
        self.assertEqual(row["complete_key_statuses"], ["替换"])

    def test_invalid_or_silent_partial_input_is_rejected(self):
        for content in (
            "[×1] 未命中 caller=00892C8B BACK\n",
            HEADER + "[×1] unrecognized caller=00892C8B BACK\n",
            HEADER + "[×0] 未命中 caller=00892C8B BACK\n",
            HEADER + "[×1] 未命中 caller=00892C8B " + "A" * 49 + "\n",
            HEADER,
        ):
            with self.subTest(content=content):
                path = self.work / "invalid.txt"
                path.write_text(content, encoding="utf-8")
                with self.assertRaises(ValueError):
                    self.report(path)
        path.write_bytes(HEADER.encode() + b"\xff")
        with self.assertRaises(UnicodeError):
            self.report(path)

    def test_report_publication_does_not_overwrite_inputs_or_existing_output(self):
        path = self.inventory(["[×1] 未命中 caller=00892C8B START | <TAG>"])
        original = path.read_bytes()
        out = self.work / "audit"
        audit.audit(self.candidate, [path], self.policy, out)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual({p.name for p in out.iterdir()}, {"audit.json", "audit.md"})
        self.assertIn("START &#124; &lt;TAG&gt;", (out / "audit.md").read_text(encoding="utf-8"))
        before = (out / "audit.json").read_bytes()
        with self.assertRaises(FileExistsError):
            audit.audit(self.candidate, [path], self.policy, out)
        self.assertEqual((out / "audit.json").read_bytes(), before)

    def test_output_inside_candidate_and_duplicate_inputs_are_rejected(self):
        path = self.inventory(["[×1] 替换 caller=00892C8B BACK"])
        with self.assertRaisesRegex(ValueError, "immutable candidate"):
            audit.audit(self.candidate, [path], self.policy, self.candidate / "audit")
        with self.assertRaisesRegex(ValueError, "same inventory"):
            audit.audit(self.candidate, [path, path], self.policy, self.work / "audit")
        self.verify.assert_not_called()

    def test_candidate_rejection_leaves_no_report(self):
        self.verify.side_effect = ValueError("Candidate digest mismatch")
        out = self.work / "audit"
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            audit.audit(self.candidate, [], self.policy, out)
        self.assertFalse(out.exists())

    def test_dictionary_count_must_match_the_native_receipt(self):
        (self.package_evidence / "dictionary-effective.json").write_text(
            '{"BACK": "返回"}', encoding="utf-8"
        )
        with self.assertRaisesRegex(ValueError, "production receipt"):
            self.report(self.inventory(["[×1] 替换 caller=00892C8B BACK"]))


if __name__ == "__main__":
    unittest.main()
