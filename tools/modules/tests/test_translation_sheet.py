"""Exercise LG decoding, literal Excel cells and the translator-to-dictionary contract."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from openpyxl import load_workbook

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import _dictionary as dictionary_text  # noqa: E402
import translation_sheet as sheets  # noqa: E402


def block(key, reference):
    return (
        b"xfhsm_res_ENG_Start\n" + key + b"\nxfhsm_res_ENG_End\n"
        b"xfhsm_res_CHI_Start\n" + reference + b"\nxfhsm_res_CHI_End\n"
    )


class TranslationSheetTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ac1-translation-sheet-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.source = self.work / "LG.txt"
        self.source.write_bytes(block(b" > Back ", b"\\x8FD4\\x56DE"))
        self.output = self.work / "translation.xlsx"

    def edit(self, changes):
        workbook = load_workbook(self.output)
        for address, value in changes.items():
            workbook.active[address] = value
        workbook.save(self.output)
        workbook.close()

    def test_big5_trailing_backslash_and_codeunit_escapes_are_decoded_at_character_starts(self):
        raw = "功".encode("cp950") + b"x1234 \\x4E2D"
        self.assertEqual(sheets.decode_reference(raw), "功x1234 中")
        self.assertEqual(sheets.decode_reference(b"\\xZZZZ"), "\\xZZZZ")
        with self.assertRaises(UnicodeDecodeError):
            sheets.decode_reference(b"\xff")

    def test_duplicate_keys_keep_all_references_without_selecting_a_translation(self):
        rows, stats = sheets.lg_rows(block(b"A\n B", b"one") + block(b" A  B ", b"two"))
        self.assertEqual(stats, dict(blocks=2, rows=1, duplicates=1))
        self.assertEqual(rows[0][0:2], ("A B", ""))
        self.assertIn("one", rows[0][2])
        self.assertIn("two", rows[0][2])
        self.assertIn("2", rows[0][3])

    def test_missing_or_nested_blocks_reject_the_export_instead_of_mispairing(self):
        for raw in (
            b"not LG",
            b"",
            self.source.read_bytes().replace(b"xfhsm_res_CHI_End", b""),
            block(b"A", b"one") + b"xfhsm_res_ENG_Start\nB\nxfhsm_res_ENG_End",
            block(b"A", b"one").replace(b"xfhsm_res_ENG_End", b"xfhsm_res_CHI_End"),
        ):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                sheets.lg_rows(raw)

    def test_control_keys_remain_visible_as_escapes_for_review(self):
        rows, _ = sheets.lg_rows(block(b"A\\x0001", b"one"))
        self.assertEqual(rows[0][0], "A\\x0001")
        self.assertIn("不支持", rows[0][3])
        rows, _ = sheets.lg_rows(block(b"A\\x0001", b"one") + block(b"Back", b"two"))
        self.assertEqual([row[0] for row in rows], ["Back", "A\\x0001"])

    def test_invalid_workbook_has_a_content_error(self):
        self.output.write_bytes(b"not an xlsx")
        with self.assertRaisesRegex(ValueError, "Invalid .xlsx"):
            sheets.workbook_dictionary(self.output)

    def test_export_uses_one_filterable_sheet_blank_translations_and_literal_text(self):
        self.source.write_bytes(block(b"=1+1", b"=2+2") + block(b"001", b"reference"))
        original = self.source.read_bytes()
        sheets.export_workbook(self.source, self.output)
        workbook = load_workbook(self.output, data_only=False)
        self.addCleanup(workbook.close)
        sheet = workbook.active
        self.assertEqual(workbook.sheetnames, ["翻译"])
        self.assertEqual(sheet.freeze_panes, "B2")
        self.assertEqual(sheet.auto_filter.ref, "A1:D3")
        self.assertEqual(sheet["A2"].value, "=1+1")
        self.assertEqual(sheet["A2"].data_type, "s")
        self.assertEqual(sheet["C2"].data_type, "s")
        self.assertEqual(sheet["A3"].value, "001")
        self.assertIsNone(sheet["B2"].value)
        self.assertEqual(self.source.read_bytes(), original)

    def test_existing_translator_edits_are_never_overwritten(self):
        self.output.write_bytes(b"translator edits")
        with self.assertRaises(FileExistsError):
            sheets.export_workbook(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"translator edits")

    def test_newlines_tabs_literal_backslashes_and_numeric_text_survive_the_round_trip(self):
        sheets.export_workbook(self.source, self.output)
        self.edit({"B2": "第一行\n第二行\\n第三行\\tC:\\\\new\\\\file"})
        raw, stats = sheets.workbook_dictionary(self.output)
        self.assertEqual(stats, dict(rows=1, translated=1, pending=0))
        target = self.work / "dictionary.tsv"
        target.write_bytes(raw)
        self.assertEqual(
            dictionary_text.load_supplement(target),
            {"> Back": "第一行\n第二行\n第三行\tC:\\new\\file"},
        )
        self.edit({"B2": "001"})
        self.assertIn(b"\t001\n", sheets.workbook_dictionary(self.output)[0])

    def test_blank_translations_are_pending_and_lg_reference_is_never_used_as_translation(self):
        self.source.write_bytes(block(b"A", b"reference one") + block(b"B", b"reference two"))
        sheets.export_workbook(self.source, self.output)
        with self.assertRaisesRegex(ValueError, "no translations"):
            sheets.workbook_dictionary(self.output)
        self.edit({"B2": "译文"})
        raw, stats = sheets.workbook_dictionary(self.output)
        self.assertEqual(raw, "A\t译文\n".encode())
        self.assertEqual(stats, dict(rows=2, translated=1, pending=1))

    def test_formulas_errors_numeric_cells_and_translations_without_keys_are_rejected(self):
        for changes, message in (
            ({"B2": "=1+1"}, "formulas"),
            ({"B2": "#REF!"}, "errors"),
            ({"B2": 12}, "expected text"),
            ({"B2": True}, "expected text"),
            ({"A2": None, "B2": "译文"}, "no original"),
            ({"B2": "\\xD800"}, "unsupported"),
        ):
            with self.subTest(changes=changes):
                if self.output.exists():
                    self.output.unlink()
                sheets.export_workbook(self.source, self.output)
                self.edit(changes)
                with self.assertRaisesRegex(ValueError, message):
                    sheets.workbook_dictionary(self.output)

    def test_normalized_duplicate_keys_and_changed_headers_are_rejected(self):
        self.source.write_bytes(block(b"A B", b"one") + block(b"C", b"two"))
        sheets.export_workbook(self.source, self.output)
        self.edit({"A3": "A   B", "B2": "一", "B3": "二"})
        with self.assertRaisesRegex(ValueError, "duplicate"):
            sheets.workbook_dictionary(self.output)
        self.edit({"A1": "changed"})
        with self.assertRaisesRegex(ValueError, "headers"):
            sheets.workbook_dictionary(self.output)

    def test_actual_control_newlines_are_not_confused_with_literal_escape_sequences(self):
        text = "line\nnext\ttab\\n\\x0041\r"
        self.assertEqual(dictionary_text.unescape_value(sheets.escape_text(text)), text)

    def test_two_top_level_entrypoints_have_help_from_another_directory(self):
        self.assertEqual(
            {p.name for p in TOOLS.parent.glob("*.py")}, {"pack_language.py", "lg_to_excel.py"}
        )
        for name in ("pack_language.py", "lg_to_excel.py"):
            result = subprocess.run(
                [sys.executable, "-X", "utf8", str(TOOLS.parent / name), "--help"],
                cwd=self.work,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--out", result.stdout)


if __name__ == "__main__":
    unittest.main()
