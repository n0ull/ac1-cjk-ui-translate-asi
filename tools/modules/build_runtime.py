"""Build a single-language v2 package and publish after independent verification."""

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from importlib.metadata import version
from pathlib import Path

from _runtime_inputs import (
    DEFAULT_LANGUAGE,
    DEFAULT_ORACLE,
    HERE,
    ROOT,
    checked_source,
    corpus,
    effective_dictionary,
    oracle_provenance,
    original_font,
    raster_font,
    read_profile,
    sha,
    source_path,
    write_json,
)


def bake_font(item, required, sources, font_path, cmap, out):
    from _runtime_raster import bake_baseline_cell, padded_ink
    from PIL import Image

    native, original = original_font(item)
    owned = {row[0] for row in native}
    added = sorted(required - owned)
    missing = [f"U+{cp:04X}" for cp in added if cp not in cmap or cmap[cp] == ".notdef"]
    if missing:
        raise ValueError(f"{item['name']}: missing source font glyphs: {missing}")
    font = raster_font(font_path, item)
    item = dict(item)
    if not added:
        item["target_dimensions"] = list(item["original_dimensions"])
    glyphs = []
    for cp in added:
        if cp == 0x3000:
            glyphs.append((cp, None, None))
            continue
        if item["bake"] == "bold_cell":
            cell, bbox, _ = bake_baseline_cell(
                chr(cp),
                font,
                item["font_px"] * 2,
                item["advance"] * 2,
                item["font_baseline"] * 2,
                2,
            )
            left, top = (value // 2 * 2 for value in bbox[:2])
            right, bottom = ((value + 1) // 2 * 2 for value in bbox[2:])
            # Preserve the already accepted bold bearings; storage relocation
            # must not introduce a second centering or baseline correction.
            sample = cell.crop((left, top, right, bottom))
            quad = [
                (right - left) // 2,
                (bottom - top) // 2,
                left // 2,
                bottom // 2 - (item["advance"] - item["native_baseline"]),
                item["advance"],
            ]
        elif item["bake"] == "padded_ink":
            result = padded_ink(
                chr(cp),
                font,
                item["advance"],
                item["font_px"],
                item["font_baseline"],
                item["native_baseline"],
            )
            if result is None:
                raise ValueError(f"Unexpected blank U+{cp:04X}")
            quad, upright = result
            sample = upright.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        else:
            raise ValueError("Unknown raster strategy")
        glyphs.append((cp, quad, sample))
    ink = [sample for _, _, sample in glyphs if sample is not None]
    tile_w = max(((sample.width + 11) // 4) * 4 for sample in ink) if ink else 4
    tile_h = max(((sample.height + 11) // 4) * 4 for sample in ink) if ink else 4
    width, height = item["target_dimensions"]
    old_w, old_h = item["original_dimensions"]
    columns, rows = width // tile_w, (height - old_h) // tile_h
    if columns * rows < len(ink):
        raise ValueError(f"{item['name']}: atlas capacity exceeded without resizing text")
    payload = bytearray(width * height)
    old_stride, new_stride = old_w * 4, width * 4
    for row in range(old_h // 4):
        dest = ((height - old_h) // 4 + row) * new_stride
        payload[dest : dest + old_stride] = original[row * old_stride : (row + 1) * old_stride]
    records, coverage, tile_index = bytearray(), [], 0
    space = next(row for row in native if row[0] == 32)
    for cp, quad, sample in glyphs:
        coverage_row = dict(
            cp=cp,
            char=chr(cp),
            sources=[name for name, cps in sources.items() if cp in cps],
            blank=sample is None,
        )
        if sample is None:
            row = list(space)
            row[0], row[5], row[10], row[11] = cp, item["advance"], 0, 0
            for field in (6, 8):
                row[field] *= old_w / width
            for field in (7, 9):
                row[field] *= old_h / height
            records.extend(struct.pack("<HHHhhh4fHH", *row))
            coverage.append(coverage_row)
            continue
        x, y = (tile_index % columns) * tile_w, (tile_index // columns) * tile_h
        tile = Image.new("L", (tile_w, tile_h))
        tile.paste(sample, (4, 4))
        pixels = tile.tobytes()
        for by in range(0, tile_h, 4):
            for bx in range(0, tile_w, 4):
                alpha = 0
                for dy in range(4):
                    start = (by + dy) * tile_w + bx
                    for dx, value in enumerate(pixels[start : start + 4]):
                        alpha |= (value // 17) << ((dy * 4 + dx) * 4)
                offset = ((y + by) // 4 * (width // 4) + (x + bx) // 4) * 16
                struct.pack_into("<QHHI", payload, offset, alpha, 0xFFFF, 0xFFFF, 0)
        w, h, xoff, yoff, advance = quad
        sx, sy = x + 4, y + 4
        records.extend(
            struct.pack(
                "<HHHhhh4fHH",
                cp,
                w,
                h,
                xoff,
                yoff,
                advance,
                sx / width,
                1 - (sy + h * 2) / height,
                (sx + w * 2) / width,
                1 - sy / height,
                0,
                0,
            )
        )
        coverage_row.update(quad=quad, sample_origin=[sx, sy])
        coverage.append(coverage_row)
        tile_index += 1
    write_json(
        out / f"coverage-{item['name']}.json",
        dict(
            required=len(required),
            retained=sorted(required & owned),
            appended=coverage,
            extra_usage="Only bold usage is native-proven; other fonts conservatively cover all values",
        ),
    )
    return dict(
        profile=item,
        records=bytes(records),
        payload=payload,
        appended=len(added),
        report=dict(
            name=item["name"],
            original=len(native),
            appended=len(added),
            blank=len(added) - len(ink),
            original_dimensions=[old_w, old_h],
            dimensions=[width, height],
            tile=[tile_w, tile_h],
            capacity=columns * rows,
            payload_bytes=len(payload),
            axes=item["axes"],
            logical_font_px=item["font_px"],
            advance=item["advance"],
            font_baseline=item["font_baseline"],
            native_baseline=item["native_baseline"],
            density=2,
        ),
    )


def serialize(dictionary, fonts, limit, language_id):
    ordered = sorted(fonts, key=lambda font: font["profile"]["name"])
    package = bytearray(128 + len(ordered) * 224)

    def segment(raw):
        package.extend(b"\0" * (-len(package) % 16))
        offset = len(package)
        package.extend(raw)
        return offset, len(raw)

    dict_offset, dict_bytes = segment(dictionary)
    for index, font in enumerate(ordered):
        item = font["profile"]
        record_offset, record_bytes = segment(font["records"])
        texture_offset, texture_bytes = segment(font["payload"])
        directory = struct.pack(
            "<16s12I4Q32s32s32s32s",
            item["name"].encode("ascii"),
            item["asset_id"],
            item["class_hash"],
            *item["original_dimensions"],
            *item["target_dimensions"],
            4,
            1,
            item["original_count"],
            font["appended"],
            2,
            0,
            record_offset,
            record_bytes,
            texture_offset,
            texture_bytes,
            bytes.fromhex(item["original_texture_sha256"]),
            bytes.fromhex(item["original_records_sha256"]),
            hashlib.sha256(font["records"]).digest(),
            hashlib.sha256(font["payload"]).digest(),
        )
        package[128 + index * 224 : 128 + (index + 1) * 224] = directory
    if len(package) > limit:
        raise ValueError("Package exceeds the configured byte budget")
    struct.pack_into(
        "<8sIIQIIQQQII32s32s",
        package,
        0,
        b"AC1LANG\0",
        2,
        128,
        len(package),
        0,
        len(ordered),
        dict_offset,
        dict_bytes,
        128,
        224,
        2,
        bytes(32),
        language_id.encode("ascii").ljust(32, b"\0"),
    )
    package[64:96] = hashlib.sha256(package).digest()
    return package


def build(
    dictionary_path,
    font_path,
    profile_path,
    destination,
    *,
    input_root,
    oracle_path=DEFAULT_ORACLE,
    show_success=True,
):
    if destination.exists():
        raise FileExistsError(f"Refusing to overwrite output: {destination}")
    profile = read_profile(profile_path, input_root, oracle_path)
    if profile["runtime_package_version"] != 2:
        raise ValueError("New builds require a language configuration and package v2")
    if dictionary_path.resolve() != Path(profile["dictionary_path"]):
        raise ValueError("Dictionary must come from the language configuration")
    checked_source(str(font_path), profile["font_sha256"])
    oracle = source_path(profile["oracle_path"])
    if not oracle.is_file():
        raise FileNotFoundError(
            "Run tools/modules/build_runtime_oracle.ps1 first, or supply --oracle"
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".runtime-build-", dir=destination.parent))
    try:
        raw_dictionary = dictionary_path.read_bytes()
        (stage / "profile.json").write_bytes(profile["_source_bytes"])
        (stage / "baseline.json").write_bytes(profile["_baseline_bytes"])
        snapshot = stage / "dictionary-source.txt"
        snapshot.write_bytes(raw_dictionary)
        values, audit = effective_dictionary(snapshot, oracle, stage)
        write_json(stage / "dictionary-audit.json", audit)
        write_json(stage / "dictionary-effective.json", values)
        dictionary_cps, extra = corpus(values, profile)
        from fontTools.ttLib import TTFont

        with TTFont(font_path) as source_font:
            cmap = source_font.getBestCmap()
        fonts = []
        for item in profile["fonts"]:
            print(f"Baking {item['name']} ...", flush=True)
            font = bake_font(
                item,
                dictionary_cps | extra,
                dict(dictionary=dictionary_cps, original_ui=extra),
                font_path,
                cmap,
                stage,
            )
            if item["original_count"] + font["appended"] > profile["max_total_records_per_font"]:
                raise ValueError("Native record budget exceeded")
            fonts.append(font)
        raw_package = serialize(
            raw_dictionary, fonts, profile["max_package_bytes"], profile["language_id"]
        )
        package_path = stage / "AC1_Language.bin"
        package_path.write_bytes(raw_package)
        print(
            "Independent reader: structure, production dictionary, all glyph pixels ...", flush=True
        )
        result = subprocess.run(
            [
                sys.executable,
                "-X",
                "utf8",
                str(HERE / "verify_runtime.py"),
                str(package_path),
                "--language",
                str(stage / "profile.json"),
                "--font",
                str(font_path),
                "--input-root",
                str(input_root),
                "--oracle",
                str(oracle),
                "--report",
                str(stage / "verify-report.json"),
            ],
            check=False,
        )
        if result.returncode:
            raise ValueError("Independent package verification failed")
        baseline_path = HERE / "runtime/hosts" / (profile["baseline_id"] + ".json")
        if baseline_path.read_bytes() != profile["_baseline_bytes"]:
            raise ValueError("Shared baseline changed during the language build")
        source_files = [
            HERE / name
            for name in (
                "build_runtime.py",
                "verify_runtime.py",
                "_runtime_inputs.py",
                "_runtime_raster.py",
                "build_runtime_oracle.ps1",
                "native-toolchain.ps1",
                "runtime/dictionary_oracle.cpp",
                "runtime/requirements.in",
            )
        ]
        source_files += [
            ROOT / name
            for name in (
                "tools/modules/_dictionary.py",
                "modules/dict/src/dict.cpp",
                "modules/core/src/file.cpp",
                "modules/core/src/str.cpp",
            )
        ]
        report = dict(
            status="OFFLINE_PASS",
            runtime="NOT_TESTED_BY_TOOL",
            native="NOT_TESTED_BY_TOOL",
            package_bytes=len(raw_package),
            package_sha256=sha(raw_package),
            embedded_digest=raw_package[64:96].hex(),
            package_version=2,
            language_id=profile["language_id"],
            baseline_id=profile["baseline_id"],
            baseline_sha256=sha(profile["_baseline_bytes"]),
            profile_sha256=sha(profile["_source_bytes"]),
            dictionary_sha256=sha(raw_dictionary),
            font_sha256=profile["font_sha256"],
            oracle_sha256=sha(oracle.read_bytes()),
            oracle_provenance=oracle_provenance(oracle),
            source_inputs=dict(
                dictionary=str(dictionary_path), font=str(font_path), input_root=str(input_root)
            ),
            dependencies=dict(
                python=sys.version,
                pillow=version("Pillow"),
                fonttools=__import__("fontTools").__version__,
            ),
            tool_sources={str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in source_files},
            dictionary=audit,
            fonts=[font["report"] for font in fonts],
        )
        write_json(stage / "build-report.json", report)
        lines = [
            "# 单包离线构建结果",
            "",
            "状态：OFFLINE_PASS；本命令未部署或进行实机验证。",
            "",
            f"包：{len(raw_package):,} 字节；SHA-256 `{report['package_sha256']}`。",
            f"生产词典：{len(values)} 有效键；{len(audit['rejected'])} 拒收行（详见 dictionary-audit.json）。",
            "",
            "| 字体 | 原/追加 | 图集 | BC2字节 |",
            "|---|---:|---|---:|",
        ]
        for font in fonts:
            f = font["report"]
            lines.append(
                f"| {f['name']} | {f['original']}/{f['appended']} | {f['dimensions'][0]}×{f['dimensions'][1]} | {f['payload_bytes']} |"
            )
        lines += [
            "",
            "独立回读验证结构/摘要、生产词典语义、每字体覆盖、原纹理逐行保留、新字形白色BC2像素、共享基线与透明滤波边缘。",
            "字集仅为生效译文及显式原UI补充字符；容量不足拒绝构建。现有原码位/图标不覆盖。",
            "共享资源基线与8192尺寸/64MiB包预算是本离线实现的约束，不代表全部GPU或资源生命周期已通过。",
            "",
        ]
        (stage / "build-report.md").write_text("\n".join(lines), encoding="utf-8")
        for name in ("dictionary-source.txt", "dictionary-queries.bin", "dictionary-native.bin"):
            (stage / name).unlink()
        # Publish the whole directory at once so reports cannot describe an
        # older package; a failed build leaves no deployable final path.
        os.rename(stage, destination)
        if show_success:
            print(f"OFFLINE_PASS {destination / 'AC1_Language.bin'}", flush=True)
        return report
    finally:
        if stage.exists():
            if stage.resolve().parent != destination.parent.resolve() or not stage.name.startswith(
                ".runtime-build-"
            ):
                raise ValueError("Refusing to clean an unexpected staging path")
            shutil.rmtree(stage)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--language", type=Path, default=DEFAULT_LANGUAGE)
    parser.add_argument("--input-root", required=True, type=Path)
    parser.add_argument("--oracle", type=Path, default=DEFAULT_ORACLE)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    try:
        profile = read_profile(
            args.language.resolve(), args.input_root.resolve(), args.oracle.resolve()
        )
        build(
            Path(profile["dictionary_path"]),
            args.font.resolve(),
            args.language.resolve(),
            args.out.resolve(),
            input_root=args.input_root.resolve(),
            oracle_path=args.oracle.resolve(),
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
        print(f"BUILD_REJECTED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
