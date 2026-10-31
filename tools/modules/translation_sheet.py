"""LG block decoding and the single-sheet translator workbook."""

import argparse
import hashlib
import re
import shutil
import sys
import tempfile
import unicodedata
import zipfile
from pathlib import Path

import _dictionary as dictionary_text

HEADERS = ("原文", "译文", "LG参考译文", "备注")
SHEET = "翻译"
MARKERS = re.compile(rb"xfhsm_res_(ENG|CHI)_(Start|End)")
ORDER = [(b"ENG", b"Start"), (b"ENG", b"End"), (b"CHI", b"Start"), (b"CHI", b"End")]
MAX_INPUT = 16 * 1024 * 1024


def escape_text(text):
    """Preserve literal backslashes and XML-ineligible code units in editable text."""
    escaped = []
    for ch in text:
        cp = ord(ch)
        if ch == "\\":
            escaped.append("\\\\")
        elif ch == "\n":
            escaped.append("\\n")
        elif ch == "\r":
            escaped.append("\\x000D")
        elif ch == "\t":
            escaped.append("\\t")
        elif cp < 32 or cp == 127 or 0xD800 <= cp <= 0xDFFF or cp in (0xFFFE, 0xFFFF):
            escaped.append(f"\\x{cp:04X}")
        else:
            escaped.append(ch)
    return "".join(escaped)


def decode_reference(raw):
    # Big5's second byte can be '\\'; consume the pair before testing another escape.
    text, cursor = [], 0
    while cursor < len(raw):
        if raw[cursor : cursor + 2] == b"\\x":
            value = dictionary_text.hex4(raw[cursor + 2 : cursor + 6].decode("latin-1"))
            if value is not None:
                text.append(value)
                cursor += 6
                continue
        length = 1 if raw[cursor] < 128 else 2
        text.append(raw[cursor : cursor + length].decode("cp950"))
        cursor += length
    return "".join(text).replace("\0", "").strip(" \t\r\n")


def lg_rows(raw):
    if not raw or len(raw) > MAX_INPUT:
        raise ValueError("LG dictionary must contain 1..16 MiB of source bytes")
    markers = list(MARKERS.finditer(raw))
    if not markers or len(markers) % 4:
        raise ValueError("Expected complete ENG/CHI blocks from LGCStringDict_01.txt")
    grouped = {}
    for offset in range(0, len(markers), 4):
        block = markers[offset : offset + 4]
        number = offset // 4 + 1
        if [marker.groups() for marker in block] != ORDER:
            raise ValueError(f"LG block {number}: missing, nested or out-of-order ENG/CHI marker")
        english = raw[block[0].end() : block[1].start()].replace(b"\0", b"")
        chinese = raw[block[2].end() : block[3].start()]
        try:
            key = dictionary_text.canon(dictionary_text.unescape_value(english.decode("ascii")))
            reference = decode_reference(chinese)
        except UnicodeError as error:
            raise ValueError(f"LG block {number}: invalid ASCII/CP950 data") from error
        if not key:
            raise ValueError(f"LG block {number}: empty original text")
        references, count = grouped.get(key, ([], 0))
        if reference not in references:
            references.append(reference)
        grouped[key] = references, count + 1
    rows, review = [], []
    for key, (references, count) in grouped.items():
        notes = []
        if count > 1:
            notes.append(f"LG重复{count}次，参考译文{len(references)}份")
        unsupported = any(
            ord(ch) < 32 or ord(ch) == 127 or 0xD800 <= ord(ch) <= 0xDFFF for ch in key
        )
        if unsupported:
            notes.append("原文含运行时不支持的码元；留空译文供核对")
        reference = "\n\n".join(
            f"参考{i + 1}：{escape_text(text)}" if len(references) > 1 else escape_text(text)
            for i, text in enumerate(references)
        )
        destination = review if unsupported else rows
        destination.append((escape_text(key), "", reference, "；".join(notes)))
    # Lead with text the translator can use; keep control blocks available for review at the end.
    return rows + review, dict(
        blocks=len(markers) // 4, rows=len(grouped), duplicates=len(markers) // 4 - len(grouped)
    )


def export_workbook(source, out):
    from openpyxl import Workbook
    from openpyxl.comments import Comment
    from openpyxl.styles import Alignment, Font, PatternFill

    source, out = source.resolve(), out.resolve()
    if out.exists():
        raise FileExistsError(f"Refusing to overwrite workbook: {out}")
    if out.suffix.casefold() != ".xlsx":
        raise ValueError("Output must be an .xlsx workbook")
    raw = source.read_bytes()
    rows, stats = lg_rows(raw)
    workbook = Workbook()
    workbook.properties.title = "AC1 翻译词典"
    workbook.properties.description = f"LG source SHA-256: {hashlib.sha256(raw).hexdigest()}"
    sheet = workbook.active
    sheet.title = SHEET
    sheet.sheet_view.showGridLines = False
    sheet.append(HEADERS)
    sheet.freeze_panes = "B2"
    sheet.auto_filter.ref = f"A1:D{len(rows) + 1}"
    widths = (55, 55, 65, 38)
    for column, width in zip("ABCD", widths):
        sheet.column_dimensions[column].width = width
    for cell in sheet[1]:
        cell.font = Font(name="Arial", size=11, bold=True, color="FFFFFF")
        cell.fill = PatternFill("solid", fgColor="24465E")
        cell.alignment = Alignment(vertical="center")
    sheet.row_dimensions[1].height = 28
    sheet["B1"].comment = Comment(
        "填写目标语言译文。空译文保留原文。支持实际换行或\\n；\\t为制表符，\\\\为字面反斜杠。"
        "LG参考译文不会自动打包。原文是匹配键，请保持不变。",
        "AC1",
    )
    for number, row in enumerate(rows, 2):
        if any(len(value) > 32767 for value in row):
            raise ValueError(f"LG row {number}: text exceeds Excel's cell limit")
        sheet.append(row)
        longest = 1
        for value, width in zip(row, widths):
            wrapped = 0
            for line in value.split("\n"):
                units = sum(2 if unicodedata.east_asian_width(ch) in "WF" else 1 for ch in line)
                wrapped += max(1, (units + width - 1) // width)
            longest = max(longest, wrapped)
        sheet.row_dimensions[number].height = min(409, max(45, longest * 18 + 8))
        for cell in sheet[number]:
            # Excel must retain '=...', numeric labels and leading zeroes as literal text.
            cell.data_type = "s"
            cell.number_format = "@"
            editable = cell.column in (2, 4)
            cell.font = Font(name="Arial", size=11, color="174EA6" if editable else "515B65")
            cell.fill = PatternFill("solid", fgColor="FFF8DD" if editable else "F2F4F6")
            cell.alignment = Alignment(wrap_text=True, vertical="top")
    out.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=".translation-", dir=out.parent) as temporary:
            staged = Path(temporary) / "translation.xlsx"
            workbook.save(staged)
            # Exclusive creation preserves any existing translator edits, including races.
            with out.open("xb") as destination:
                try:
                    with staged.open("rb") as origin:
                        shutil.copyfileobj(origin, destination)
                except BaseException:
                    destination.close()
                    out.unlink()
                    raise
    finally:
        workbook.close()
    return stats


def workbook_dictionary(path):
    from _runtime_inputs import validate_text
    from openpyxl import load_workbook

    if path.stat().st_size > MAX_INPUT:
        raise ValueError("Workbook exceeds 16 MiB")
    try:
        with zipfile.ZipFile(path) as archive:
            if sum(item.file_size for item in archive.infolist()) > 64 * 1024 * 1024:
                raise ValueError("Workbook expanded size exceeds 64 MiB")
        workbook = load_workbook(path, read_only=True, data_only=False, keep_links=False)
    except (zipfile.BadZipFile, KeyError, SyntaxError) as error:
        raise ValueError("Invalid .xlsx workbook") from error
    rows, seen, pending = [], set(), 0
    try:
        if workbook.sheetnames != [SHEET]:
            raise ValueError(f"Workbook must contain the single {SHEET!r} sheet")
        sheet = workbook[SHEET]
        if sheet.max_column != 4 or sheet.max_row > 100000:
            raise ValueError("Workbook must have four columns and at most 100000 rows")
        header = tuple(cell.value for cell in next(sheet.iter_rows(min_row=1, max_row=1)))
        if header != HEADERS:
            raise ValueError(f"Workbook headers must be {HEADERS}")
        for number, cells in enumerate(sheet.iter_rows(min_row=2), 2):
            original, translated = cells[:2]
            for cell in (original, translated):
                if cell.data_type in ("f", "e"):
                    raise ValueError(
                        f"{cell.coordinate}: formulas and Excel errors are unsupported"
                    )
                if cell.value is not None and not isinstance(cell.value, str):
                    raise ValueError(f"{cell.coordinate}: expected text, not a number/date/boolean")
            if original.value is None and translated.value is None:
                continue
            key = dictionary_text.canon(dictionary_text.unescape_value(original.value or ""))
            if not key:
                raise ValueError(f"Row {number}: translation has no original text")
            if key in seen:
                raise ValueError(f"Row {number}: duplicate normalized original text")
            seen.add(key)
            value = dictionary_text.trim_value(
                dictionary_text.unescape_value(translated.value or "")
            )
            if not value:
                pending += 1
                continue
            validate_text(key, f"workbook A{number}")
            validate_text(value, f"workbook B{number}")
            rows.append(escape_text(key) + "\t" + escape_text(value) + "\n")
    finally:
        workbook.close()
    if not rows:
        raise ValueError("Workbook contains no translations; fill the 译文 column first")
    return "".join(rows).encode("utf-8"), dict(
        rows=len(seen), translated=len(rows), pending=pending
    )


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="将 LG 主词典导出为 Excel；只填写译文列即可交给打包器。"
    )
    parser.add_argument("--input", type=Path, required=True, help="LGCStringDict_01.txt")
    parser.add_argument("--out", type=Path, required=True, help="新的 .xlsx 翻译表")
    args = parser.parse_args(argv)
    try:
        stats = export_workbook(args.input, args.out)
    except (ImportError, OSError) as error:
        print(f"INPUT_ERROR: {error}", file=sys.stderr)
        return 2
    except (ValueError, TypeError) as error:
        print(f"EXPORT_REJECTED: {error}", file=sys.stderr)
        return 1
    print(
        f"EXPORTED {args.out}; {stats['rows']} rows, {stats['duplicates']} duplicate blocks merged"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
