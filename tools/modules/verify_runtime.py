"""Independent package reader: explicit offsets, no writer or baking helper imports."""

import argparse
import hashlib
import math
import re
import struct
import sys
import tempfile
from pathlib import Path

from _runtime_inputs import (
    DEFAULT_LANGUAGE,
    DEFAULT_ORACLE,
    checked_source,
    corpus,
    effective_dictionary,
    original_font,
    raster_font,
    read_profile,
    sha,
    source_path,
    write_json,
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_package(path, profile):
    size = path.stat().st_size
    require(128 <= size <= profile["max_package_bytes"], "Package size outside bounds")
    raw = path.read_bytes()

    def u32(at):
        return struct.unpack_from("<I", raw, at)[0]

    def u64(at):
        return struct.unpack_from("<Q", raw, at)[0]

    require(raw[:8] == b"AC1LANG\0", "Bad magic")
    require(u32(8) == 2 and u32(12) == 128, "Unsupported version/header")
    require(u64(16) == len(raw) and u32(24) == 0, "Package length or flags mismatch")
    require(
        u32(28) == 4 and u32(56) == 224 and u32(60) == profile["dictionary_rules_version"],
        "Font count/directory/rules mismatch",
    )
    label = raw[96:128].split(b"\0", 1)[0]
    require(
        re.fullmatch(rb"[a-z][a-z0-9]*(?:-[a-z0-9]+)*", label) and len(label) < 32,
        "Invalid language identifier",
    )
    require(
        raw[96:128] == label.ljust(32, b"\0") and label.decode("ascii") == profile["language_id"],
        "Package language mismatch",
    )
    digest = hashlib.sha256(raw[:64] + bytes(32) + raw[96:]).digest()
    require(digest == raw[64:96], "Package digest mismatch")
    segments = [(0, 128, "header")]

    def segment(offset, length, name):
        require(offset % 16 == 0 and offset >= 128, f"Unaligned segment: {name}")
        require(
            offset <= len(raw) and length <= len(raw) - offset, f"Out of bounds segment: {name}"
        )
        segments.append((offset, offset + length, name))
        return memoryview(raw)[offset : offset + length]

    require(0 < u64(40) <= 16 * 1024 * 1024, "Dictionary size outside bounds")
    dictionary = segment(u64(32), u64(40), "dictionary")
    directory = u64(48)
    segment(directory, 4 * 224, "directory")
    fonts, names, identities = [], [], set()
    profiles = {item["name"]: item for item in profile["fonts"]}
    for index in range(4):
        base = directory + index * 224
        name_bytes = raw[base : base + 16]
        name = name_bytes.split(b"\0", 1)[0].decode("ascii")
        require(
            name in profiles and name_bytes == name.encode().ljust(16, b"\0"),
            "Unknown or malformed font name",
        )
        names.append(name)
        item = dict(profiles[name])
        fields = [u32(base + 16 + i * 4) for i in range(12)]
        (
            asset,
            class_hash,
            old_w,
            old_h,
            width,
            height,
            fmt,
            mips,
            original_count,
            appended,
            density,
            reserved,
        ) = fields
        original_size = [width, height] == [old_w, old_h]
        require(
            original_size if not appended else width >= old_w and height > old_h,
            f"{name}: dimensions/count mismatch",
        )
        identity = (asset, class_hash)
        require(identity not in identities, "Duplicate resource identity")
        identities.add(identity)
        require(
            identity == (item["asset_id"], item["class_hash"]),
            f"{name}: resource identity mismatch",
        )
        require(
            [old_w, old_h] == item["original_dimensions"]
            and [width, height]
            == (item["target_dimensions"] if appended else item["original_dimensions"]),
            f"{name}: dimensions mismatch",
        )
        require(
            fmt == 4 and mips == 1 and density == item["density"] and reserved == 0,
            f"{name}: format/mips/density/reserved mismatch",
        )
        require(
            original_count == item["original_count"]
            and original_count + appended <= min(65535, profile["max_total_records_per_font"]),
            f"{name}: record count exceeds budget",
        )
        rec_offset, rec_bytes, tex_offset, tex_bytes = [u64(base + 64 + i * 8) for i in range(4)]
        require(
            rec_bytes == appended * 32 and tex_bytes == width * height,
            f"{name}: segment size mismatch",
        )
        records = segment(rec_offset, rec_bytes, name + " records")
        payload = segment(tex_offset, tex_bytes, name + " texture")
        require(
            raw[base + 96 : base + 128].hex() == item["original_texture_sha256"],
            f"{name}: original texture identity mismatch",
        )
        require(
            raw[base + 128 : base + 160].hex() == item["original_records_sha256"],
            f"{name}: original record identity mismatch",
        )
        require(
            hashlib.sha256(records).digest() == raw[base + 160 : base + 192],
            f"{name}: record digest mismatch",
        )
        require(
            hashlib.sha256(payload).digest() == raw[base + 192 : base + 224],
            f"{name}: texture digest mismatch",
        )
        rows = list(struct.iter_unpack("<HHHhhh4fHH", records))
        item["target_dimensions"] = [width, height]
        cps = [row[0] for row in rows]
        require(cps == sorted(set(cps)), f"{name}: codepoints must be ascending and unique")
        for row in rows:
            cp, w, h, xoff, yoff, advance, u0, v0, u1, v1, page, padding = row
            valid_cp = cp >= 32 and cp != 127 and not 0xD800 <= cp <= 0xDFFF
            require(valid_cp, f"{name}: invalid glyph codepoint")
            require(
                w > 0 and h > 0 and advance == item["advance"], f"{name}: invalid quad or advance"
            )
            require(page == 0 and padding == 0, f"{name}: nonzero glyph page/padding")
            require(
                all(math.isfinite(v) and 0 <= v <= 1 for v in (u0, v0, u1, v1)),
                f"{name}: invalid UV",
            )
            require(u0 < u1 and v0 < v1, f"{name}: reversed UV rectangle")
        fonts.append(dict(profile=item, rows=rows, payload=payload))
    require(names == sorted(profiles), "Directory names duplicated or not sorted")
    end = 0
    for start, stop, name in sorted(segments):
        require(start >= end, f"Overlapping segment: {name}")
        require(not any(raw[end:start]), f"Nonzero inter-segment padding: {name}")
        end = stop
    require(end == len(raw), "Unreferenced trailing data")
    return raw, bytes(dictionary), fonts


def pixel_rectangle(payload, width, box):
    """Decode BC2 alpha and require white endpoints for every sampled block."""
    from PIL import Image

    left, top, right, bottom = box
    require(
        0 <= left < right <= width and 0 <= top < bottom <= len(payload) // width,
        "Pixel rectangle outside atlas",
    )
    pixels = bytearray((right - left) * (bottom - top))
    for y in range(top, bottom):
        for x in range(left, right):
            offset = ((y // 4) * (width // 4) + x // 4) * 16
            alpha, c0, c1, selectors = struct.unpack_from("<QHHI", payload, offset)
            # Native originals may use arbitrary RGB, but new glyph pixels
            # must remain white so the engine can apply its normal text color.
            nibble = (alpha >> (4 * ((y % 4) * 4 + x % 4))) & 15
            if nibble:
                require(c0 == c1 == 0xFFFF and selectors == 0, "Nonwhite new glyph BC2 block")
            pixels[(y - top) * (right - left) + x - left] = nibble * 17
    return Image.frombytes("L", (right - left, bottom - top), bytes(pixels))


def verify_glyphs(font, required, font_path):
    from PIL import Image, ImageDraw

    item, rows, payload = font["profile"], font["rows"], font["payload"]
    name = item["name"]
    native, original = original_font(item)
    owned = {row[0] for row in native}
    require({row[0] for row in rows} == required - owned, f"{name}: per-font coverage mismatch")
    width, height = item["target_dimensions"]
    old_w, old_h = item["original_dimensions"]
    for by in range(old_h // 4):
        start = ((height - old_h) // 4 + by) * width * 4
        require(
            payload[start : start + old_w * 4] == original[by * old_w * 4 : (by + 1) * old_w * 4],
            f"{name}: original BC2 row changed",
        )
    source_font = raster_font(font_path, item)
    occupied = bytearray(width * height // 16)
    space = next(row for row in native if row[0] == 32)
    ink_count = blank_count = 0
    for row in rows:
        cp, w, h, xoff, yoff, advance, u0, v0, u1, v1, _, _ = row
        if cp == 0x3000:
            expected = list(space)
            expected[0], expected[5], expected[10], expected[11] = cp, item["advance"], 0, 0
            for field in (6, 8):
                expected[field] *= old_w / width
            for field in (7, 9):
                expected[field] *= old_h / height
            require(
                struct.pack("<HHHhhh4fHH", *row) == struct.pack("<HHHhhh4fHH", *expected),
                f"{name}: blank-space record changed",
            )
            box = [
                round(u0 * width),
                round((1 - v1) * height),
                round(u1 * width),
                round((1 - v0) * height),
            ]
            require(
                pixel_rectangle(payload, width, box).getbbox() is None,
                f"{name}: native space not transparent",
            )
            blank_count += 1
            continue
        coords = (u0 * width, (1 - v1) * height, u1 * width, (1 - v0) * height)
        require(
            all(abs(v - round(v)) < 0.0001 for v in coords), f"{name}: UV not on texel boundaries"
        )
        left, top, right, bottom = map(round, coords)
        require(
            right - left == w * 2 and bottom - top == h * 2, f"{name}: quad/UV density mismatch"
        )
        storage = (left - 4, top - 4, ((right + 7) // 4) * 4, ((bottom + 7) // 4) * 4)
        sx, sy, ex, ey = storage
        require(
            0 <= sx < ex <= width and 0 <= sy < ey <= height - old_h,
            f"{name}: storage outside appended band",
        )
        require(sx % 4 == 0 and sy % 4 == 0, f"{name}: invalid filter guard alignment")
        for by in range(sy // 4, ey // 4):
            a, b = by * (width // 4) + sx // 4, by * (width // 4) + ex // 4
            require(not any(occupied[a:b]), f"{name}: overlapping glyph/filter storage")
            occupied[a:b] = b"\1" * (b - a)
        guarded = pixel_rectangle(payload, width, storage)
        guarded_pixels = guarded.copy()
        ImageDraw.Draw(guarded_pixels).rectangle((4, 4, 4 + w * 2 - 1, 4 + h * 2 - 1), fill=0)
        require(guarded_pixels.getbbox() is None, f"{name}: transparent filter guard contaminated")
        sample = guarded.crop((4, 4, 4 + w * 2, 4 + h * 2)).transpose(
            Image.Transpose.FLIP_TOP_BOTTOM
        )
        margin = item["font_px"] * 4
        canvas_size = (margin * 2 + item["advance"] * 2,) * 2
        source = Image.new("L", canvas_size)
        ImageDraw.Draw(source).text(
            (margin, margin + item["font_baseline"] * 2),
            chr(cp),
            font=source_font,
            fill=255,
            anchor="ls",
        )
        if item["bake"] == "padded_ink":
            source = source.point(lambda a: ((a * 15 + 127) // 255) * 17)
        bbox = source.getbbox()
        require(
            bbox is not None
            and min(bbox[:2]) > 0
            and bbox[2] < source.width
            and bbox[3] < source.height,
            f"{name}: missing/clipped source ink",
        )
        expected = Image.new("L", canvas_size)
        ink_left = (item["advance"] * 2 - (bbox[2] - bbox[0])) // 2
        expected.paste(source.crop(bbox), (margin + ink_left, bbox[1]))
        expected = expected.point(lambda a: ((a * 15 + 127) // 255) * 17)
        replay = Image.new("L", canvas_size)
        target = (margin + xoff * 2, margin + (item["native_baseline"] - yoff) * 2)
        require(
            target[0] >= 0
            and target[1] >= 0
            and target[0] + sample.width <= replay.width
            and target[1] + sample.height <= replay.height,
            f"{name}: replay outside reference canvas",
        )
        replay.paste(sample, target)
        require(
            replay.tobytes() == expected.tobytes(),
            f"{name} U+{cp:04X}: source alpha or shared baseline mismatch",
        )
        ink_count += 1
    blocks_wide = width // 4
    for index, (alpha, _c0, _c1, _selectors) in enumerate(struct.iter_unpack("<QHHI", payload)):
        if not alpha or occupied[index]:
            continue
        by, bx = divmod(index, blocks_wide)
        in_original = by >= (height - old_h) // 4 and bx < old_w // 4
        require(in_original, f"{name}: ink outside glyph storage/protected original")
    return dict(
        name=name,
        original_rows_preserved=old_h // 4,
        ink_glyphs=ink_count,
        blank_glyphs=blank_count,
        codepoints=len(rows),
        pixel_baseline_guard_white="PASS",
    )


def verify(path, profile_path, *, font_path, input_root, oracle_path=DEFAULT_ORACLE):
    profile = read_profile(profile_path, input_root, oracle_path)
    raw, dictionary, fonts = read_package(path, profile)
    checked_source(str(font_path), profile["font_sha256"])
    with tempfile.TemporaryDirectory(prefix="ac1-runtime-verify-") as temp:
        work = Path(temp)
        dictionary_file = work / "embedded-dictionary.txt"
        dictionary_file.write_bytes(dictionary)
        values, audit = effective_dictionary(
            dictionary_file,
            source_path(profile["oracle_path"]),
            work,
        )
        cps, extra = corpus(values, profile)
        results = [verify_glyphs(font, cps | extra, font_path) for font in fonts]
    return dict(
        status="OFFLINE_PASS",
        package_version=profile["runtime_package_version"],
        language_id=profile.get("language_id"),
        runtime="NOT_TESTED_BY_TOOL",
        native="NOT_TESTED_BY_TOOL",
        package_sha256=sha(raw),
        package_bytes=len(raw),
        dictionary_sha256=sha(dictionary),
        effective_keys=len(values),
        rejected_lines=len(audit["rejected"]),
        fonts=results,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--language", type=Path, default=DEFAULT_LANGUAGE)
    parser.add_argument("--input-root", required=True, type=Path)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--oracle", type=Path, default=DEFAULT_ORACLE)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    try:
        if args.report and args.report.exists():
            raise FileExistsError(f"Refusing to overwrite report: {args.report}")
        report = verify(
            args.package.resolve(),
            args.language.resolve(),
            font_path=args.font.resolve(),
            input_root=args.input_root.resolve(),
            oracle_path=args.oracle.resolve(),
        )
        if args.report:
            write_json(args.report, report)
        print(
            f"OFFLINE_PASS {report['effective_keys']} keys; "
            + ", ".join(f"{f['name']}={f['codepoints']}" for f in report["fonts"]),
            flush=True,
        )
        return 0
    except ImportError as error:
        print(
            f"DEPENDENCY_ERROR: {error}; install tools/modules/runtime/requirements.in",
            file=sys.stderr,
        )
        return 2
    except OSError as error:
        print(f"INPUT_ERROR: {error}", file=sys.stderr)
        return 2
    except (ValueError, KeyError, struct.error) as error:
        print(f"PACKAGE_REJECTED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
